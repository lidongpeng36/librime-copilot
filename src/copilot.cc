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
#include "copilot_engine.h"

namespace rime {

Copilot::Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine)
    : Processor(ticket), copilot_engine_(copilot_engine) {
  // update copilot on context change.
  auto* context = engine_->context();
  select_connection_ = context->select_notifier().connect([this](Context* ctx) { OnSelect(ctx); });
  context_update_connection_ =
      context->update_notifier().connect([this](Context* ctx) { OnContextUpdate(ctx); });
  LOG(INFO) << "Copilot plugin Loaded.";
}

Copilot::~Copilot() {
  select_connection_.disconnect();
  context_update_connection_.disconnect();
}

static inline bool IsNavigationKey(const KeyEvent& key_event) {
  auto keycode = key_event.keycode();
  // return (keycode >= XK_Up && keycode <= XK_Down);
  return (keycode >= XK_Left && keycode <= XK_Begin);
}

static inline bool IsContinuingInput(const KeyEvent& key_event) {
  if (IsNavigationKey(key_event)) {
    return true;
  }
  auto keycode = key_event.keycode();
  bool is_modifier = keycode >= XK_Shift_L && keycode <= XK_Hyper_R;
  bool is_alphabet = ((keycode >= XK_0 && keycode <= XK_9) ||
                      (keycode >= XK_a && keycode <= XK_z) || (keycode >= XK_A && keycode <= XK_Z));
  return is_modifier || is_alphabet;
}

ProcessResult Copilot::ProcessKeyEvent(const KeyEvent& key_event) {
  if (!engine_ || !copilot_engine_) {
    return kNoop;
  }
  // auto* ctx = engine_->context();
  // const auto& preedit = ctx->GetPreedit().text;
  // auto is_composing = ctx->IsComposing();
  // LOG(INFO) << "IsCompusing: " << ctx->IsComposing() << ", HasMenu:" << ctx->HasMenu()
  //           << ", Preedit:" << ctx->GetPreedit().text;

  // LOG(INFO) << "Modifier: " << key_event.modifier() << ", Keycode: " << key_event.keycode();
  auto keycode = key_event.keycode();
  if (keycode == XK_BackSpace) {
    last_action_ = kDelete;
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    auto* ctx = engine_->context();
    if (ctx) {
      if (ctx->composition().empty()) {
        copilot_engine_->BackSpace();
      } else {
        auto& seg = ctx->composition().back();
        if (!seg.HasTag("abc")) {
          copilot_engine_->BackSpace();
        }
        if (seg.HasTag("copilot")) {
          ctx->Clear();
        }
      }
    }
    return kNoop;
  }
  bool is_punct =
      (keycode > XK_space && keycode <= XK_slash) || (keycode >= XK_colon && keycode <= XK_at);
  // if (keycode == XK_Escape || keycode == XK_Return || keycode == XK_KP_Enter || is_punct) {
  if (!IsContinuingInput(key_event)) {
    last_action_ = kSpecial;
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    auto* ctx = engine_->context();
    if (is_punct) {
      copilot_engine_->history()->add(std::string(1, static_cast<char>(keycode)));
    }
    if (!ctx->composition().empty() && ctx->composition().back().HasTag("copilot")) {
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
    //            << self_updating_ << ", copilot_engine_=" << copilot_engine_
    //            << ", ctx=" << ctx << ", composition=" << ctx->composition()
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
  copilot_engine_->history()->add(last_commit.text);
  if (last_commit.type == "punct" || last_commit.type == "raw" || last_commit.type == "thru") {
    copilot_engine_->Clear();
    iteration_counter_ = 0;
    return;
  }
  if (last_commit.type == "copilot") {
    int max_iterations = copilot_engine_->max_iterations();
    iteration_counter_++;
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
