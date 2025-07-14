#include "copilot.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>

#include "auto_spacer.h"
#include "copilot_engine.h"
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

  processors_.emplace_back(std::make_shared<AutoSpacer>(ticket));
  processors_.emplace_back(std::make_shared<SelectCharacter>(ticket, [this](const string& text) {
    auto* ctx = engine_->context();
    CopilotAndUpdate(ctx, text);  // ✨ 立即启动后续预测
  }));
  LOG(INFO) << "Copilot plugin Loaded.";
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
  auto last_action = last_action_;
  last_action_ = kUnspecified;
  auto result = RunProcessors(key_event);
  if (result != kNoop) {
    // LOG(INFO) << "Processor result: " << result;
    return result;
  }
  last_action_ = last_action;

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
      // return kAccepted;
      return kNoop;
    }
  } else {
    last_action_ = kUnspecified;
  }
  return kNoop;
}

void Copilot::OnSelect(Context* ctx) { last_action_ = kSelect; }

void Copilot::OnContextUpdate(Context* ctx) {
  if (self_updating_ || !copilot_engine_ || !ctx || !ctx->composition().empty() ||
      !ctx->get_option("copilot")) {
    // LOG(ERROR) << "Copilot::OnContextUpdate: "
    //               "self_updating_="
    //            << self_updating_ << ", copilot_engine_=" << copilot_engine_ << ", ctx=" << ctx
    //            << ", composition=" << ctx->composition()
    //            << ", get_option(copilot)=" << ctx->get_option("copilot")
    //            << ", last_action_=" << last_action_;
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
