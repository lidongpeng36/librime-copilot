#include "copilot.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>

#include <set>

#include "auto_spacer.h"
#include "copilot_engine.h"
#include "ime_bridge.h"
#include "select_character.h"

namespace rime {

namespace {
enum struct SegmentTag : uint8_t {
  kTagNone = 0,
  kTagCopilot = 1,
  kTagAbc = 2,
};

inline bool IsNavigationKey(int keycode) {
  // return (keycode >= XK_Up && keycode <= XK_Down);
  if (keycode == XK_Tab) {
    return true;
  }
  return (keycode >= XK_Left && keycode <= XK_Begin);
}

inline bool IsAlphabetKey(int keycode) {
  return ((keycode >= XK_0 && keycode <= XK_9) || (keycode >= XK_a && keycode <= XK_z) ||
          (keycode >= XK_A && keycode <= XK_Z));
}

// 字母/数字/方向键 直接上屏, 停止预测
inline bool IsContinuingInput(const KeyEvent& key_event) {
  auto keycode = key_event.keycode();
  if (IsNavigationKey(keycode)) {
    return true;
  }
  bool is_modifier = (keycode >= XK_Shift_L && keycode <= XK_Hyper_R);
  return is_modifier || IsAlphabetKey(keycode);
}
}  // namespace

Copilot::Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine)
    : Processor(ticket), copilot_engine_(copilot_engine) {
  // update copilot on context change.
  auto* context = engine_->context();
  select_connection_ = context->select_notifier().connect([this](Context* ctx) { OnSelect(ctx); });
  context_update_connection_ =
      context->update_notifier().connect([this](Context* ctx) { OnContextUpdate(ctx); });

  // Read disabled plugins from config
  std::set<string> disabled_plugins;
  if (auto* config = engine_->schema()->config()) {
    if (auto list = config->GetList("copilot/disabled_plugins")) {
      for (size_t i = 0; i < list->size(); ++i) {
        if (auto item = list->GetValueAt(i)) {
          string name;
          if (item->GetString(&name)) {
            disabled_plugins.insert(name);
          }
        }
      }
    }
  }

  // Register processors based on config
  if (disabled_plugins.find("ime_bridge") == disabled_plugins.end()) {
    processors_.emplace_back(std::make_shared<ImeBridge>(ticket));
  }
  if (disabled_plugins.find("auto_spacer") == disabled_plugins.end()) {
    processors_.emplace_back(std::make_shared<AutoSpacer>(ticket));
  }
  if (disabled_plugins.find("select_character") == disabled_plugins.end()) {
    processors_.emplace_back(std::make_shared<SelectCharacter>(ticket, [this](const string& text) {
      auto* ctx = engine_->context();
      CopilotAndUpdate(ctx, text);  // ✨ 立即启动后续预测
    }));
  }
  LOG(INFO) << "Copilot plugin Loaded. Disabled plugins: " << disabled_plugins.size();
}

Copilot::~Copilot() {
  select_connection_.disconnect();
  context_update_connection_.disconnect();
}

ProcessResult Copilot::RunProcessors(const KeyEvent& key_event) {
  for (auto& p : processors_) {
    auto result = p->ProcessKeyEvent(key_event);
    if (result != kNoop) {
      return result;
    }
  }
  return kNoop;
}

ProcessResult Copilot::ProcessKeyEvent(const KeyEvent& key_event) {
  if (!engine_ || !copilot_engine_ || key_event.release()) {
    return kNoop;
  }
  auto* ctx = engine_->context();
  auto keycode = key_event.keycode();

  // LOG(INFO) << "IsCompusing: " << ctx->IsComposing() << ", HasMenu:" << ctx->HasMenu()
  //           << ", Preedit:'" << ctx->GetPreedit().text << "'"
  //           << ", Commit:" << ctx->GetCommitText() << ", Input:" << ctx->input();

  // LOG(INFO) << "Modifier: " << std::showbase << std::hex << key_event.modifier()
  //           << ", Keycode: " << key_event.repr() << "[" << keycode << "]"
  //           << ", Release:" << key_event.release();

  SegmentTag tag = SegmentTag::kTagNone;
  if (ctx) {
    if (!ctx->composition().empty()) {
      auto& seg = ctx->composition().back();
      if (seg.HasTag("abc")) {
        tag = SegmentTag::kTagAbc;
      } else if (seg.HasTag("copilot")) {
        tag = SegmentTag::kTagCopilot;
      }
    }
  }

  if (keycode == XK_BackSpace) {
    last_action_ = kDelete;
    last_keycode_ = keycode;
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    if (ctx) {
      if (tag != SegmentTag::kTagAbc) {
        copilot_engine_->BackSpace();
      }
      if (tag == SegmentTag::kTagCopilot) {
        ctx->Clear();
      }
    }
    return RunProcessors(key_event);
  }
  if (keycode == XK_space) {
    // 仅在输入状态启用预测: 预测候选仅能通过数字选择
    if (!ctx->input().empty() || IsNavigationKey(last_keycode_)) {
      last_action_ = kUnspecified;
      last_keycode_ = keycode;
      return RunProcessors(key_event);
    }
  }

  last_keycode_ = keycode;

  // 非连续输入 (标点、回车等): 先清理 copilot 状态，再执行子处理器。
  // 原因: Rime 的 Punctuator 通过检查 comp.back().HasTag("punct") 来决定是
  // 否直接上屏 (ConfirmUniquePunct / AutoCommitPunct)。如果 copilot 的
  // placeholder 片段残留在 composition 末尾 (Engine::CalculateSegmentation
  // 会跳过对 placeholder 的 Trim), 会导致 comp.back() 不是 punct 片段,
  // Punctuator 无法自动上屏, 用户就会看到候选框。
  // 顺序调整后 (与 BackSpace 分支一致), 子处理器和后续 Rime 处理器都能
  // 看到一个干净的 context。
  if (!IsContinuingInput(key_event)) {
    last_action_ = kSpecial;
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    bool is_punct =
        (keycode > XK_space && keycode <= XK_slash) || (keycode >= XK_colon && keycode <= XK_at);
    if (is_punct) {
      copilot_engine_->history()->add(std::string(1, static_cast<char>(keycode)));
    }
    if (tag == SegmentTag::kTagCopilot) {
      ctx->Clear();
    }
    auto result = RunProcessors(key_event);
    if (result != kNoop) {
      return result;
    }
    return kNoop;
  }

  // 连续输入 (字母、数字、方向键、修饰键): 正常执行子处理器。
  // 子处理器跑完后 last_action_ 一律回到 kUnspecified — 之前这里先写回了
  // 进入时的旧值, 又被下一行立即覆盖, 那次写回是死代码。
  last_action_ = kUnspecified;
  auto result = RunProcessors(key_event);
  if (result != kNoop) {
    // LOG(INFO) << "Processor result: " << result;
    return result;
  }
  last_action_ = kUnspecified;
  return kNoop;
}

void Copilot::OnSelect(Context* ctx) { last_action_ = kSelect; }

void Copilot::OnContextUpdate(Context* ctx) {
  if (self_updating_ || !copilot_engine_ || !ctx || !ctx->get_option("copilot")) {
    return;
  }

  // 中文多形标点自动上屏: 例如 '@' 在 schema 里是 ["＠", "☯"] (ConfigList),
  // Rime 的 Punctuator 不会自动确认, 会弹出候选框等待用户选择. 这里在用户
  // 刚按下标点 (last_action_ == kSpecial) 时检测到 pending 的 punct 段,
  // 主动确认默认候选让它直接上屏. 同时处理两个场景:
  //   1) 中文 → '@'            : '@' 段单独 pending, 直接 commit 成 '＠'
  //   2) '@' → '.' (多段遗留)  : 多段时, 内部非末尾的 punct 段同样强制 commit,
  //                              防止候选框残留
  if (last_action_ == kSpecial && !ctx->composition().empty()) {
    Composition& comp = ctx->composition();
    Segment& back = comp.back();
    if (back.HasTag("punct") && back.status < Segment::kSelected && back.menu &&
        !back.menu->empty()) {
      self_updating_ = true;
      ctx->ConfirmCurrentSelection();
      self_updating_ = false;
      return;
    }
  }

  if (!ctx->composition().empty()) {
    return;
  }
  if (last_action_ == kSpecial) {
    return;
  }
  if (last_action_ == kDelete) {
    return;
  }
  if (ctx->commit_history().empty()) {
    // CopilotAndUpdate(ctx, "$");
    return;
  }
  auto last_commit = ctx->commit_history().back();
  auto history = copilot_engine_->history();
  DLOG(INFO) << "last history: " << history->last() << " last commit: " << last_commit.text;
  if (history->last() == last_commit.text) {
    DLOG(INFO) << "Same Commit. Skip";
    return;
  }
  history->add(last_commit.text);
  if (last_commit.type == "punct" || last_commit.type == "raw" || last_commit.type == "thru") {
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    return;
  }
  if (last_commit.type == "copilot") {
    int max_iterations = copilot_engine_->max_iterations();
    ++iteration_counter_;
    if (max_iterations > 0 && iteration_counter_ >= max_iterations) {
      copilot_engine_->Clear();
      iteration_counter_ = 0;
      auto* ctx = engine_->context();
      if (!ctx->composition().empty() && ctx->composition().back().HasTag("copilot")) {
        ctx->Clear();
      }
      return;
    }
  }
  CopilotAndUpdate(ctx, last_commit.text);
}

void Copilot::CopilotAndUpdate(Context* ctx, const string& context_query) {
  // auto history = copilot_engine_->history();
  // LOG(INFO) << "CopilotAndUpdate: " << history->get_chars(10)
  //           << " context_query: " << context_query;
  if (copilot_engine_->Copilot(ctx, context_query)) {
    copilot_engine_->CreateCopilotSegment(ctx);
    self_updating_ = true;
    ctx->update_notifier()(ctx);
    self_updating_ = false;
  }
}

CopilotComponent::CopilotComponent(an<CopilotEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

CopilotComponent::~CopilotComponent() {}

Copilot* CopilotComponent::Create(const Ticket& ticket) {
  return new Copilot(ticket, engine_factory_->GetInstance(ticket));
}

}  // namespace rime
