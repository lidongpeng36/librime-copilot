#include "copilot.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/deployer.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>

#include <algorithm>
#include <ctime>
#include <memory>
#include <set>
#include <string>

#include "auto_spacer.h"
#include "caret_context.h"
#include "context_identity.h"
#include "copilot_engine.h"
#include "ime_bridge.h"
#include "prediction_context.h"
#include "rerank.h"  // TrailingCjkRun
#include "select_character.h"
#include "surrounding_source.h"
#include "telemetry_commit.h"
#include "tmux_source.h"
#include "utils.h"  // copilot::IsACPowerConnected, RegisterPowerChange

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

Copilot::Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine,
                 an<RerankTraceStore> rerank_traces, an<telemetry::Writer> telemetry,
                 const telemetry::Options& telemetry_options)
    : Processor(ticket),
      copilot_engine_(copilot_engine),
      rerank_traces_(rerank_traces),
      telemetry_(telemetry),
      telemetry_options_(telemetry_options) {
  // update copilot on context change.
  auto* context = engine_->context();
  select_connection_ = context->select_notifier().connect([this](Context* ctx) { OnSelect(ctx); });
  context_update_connection_ =
      context->update_notifier().connect([this](Context* ctx) { OnContextUpdate(ctx); });
  commit_connection_ = context->commit_notifier().connect([this](Context* ctx) { OnCommit(ctx); });

  // Read disabled plugins from config
  std::set<string> disabled_plugins;
  if (auto* config = engine_->schema()->config()) {
    config->GetBool("copilot/telemetry/enable", &telemetry_options_.enable);
    config->GetInt("copilot/telemetry/top_n", &telemetry_options_.top_n);
    int max_file_bytes = static_cast<int>(telemetry_options_.max_file_bytes);
    if (config->GetInt("copilot/telemetry/max_file_bytes", &max_file_bytes)) {
      telemetry_options_.max_file_bytes = max_file_bytes;
    }
    config->GetInt("copilot/telemetry/keep_generations", &telemetry_options_.keep_generations);
    config->GetInt("copilot/telemetry/sample_ok", &telemetry_options_.sample_ok);
    config->GetBool("copilot/telemetry/auto_sync", &telemetry_options_.auto_sync);
    telemetry::ClampOptions(telemetry_options_);

    config->GetBool("copilot/context_memory/enable", &context_memory_options_.enable);
    config->GetBool("copilot/context_memory/use_pane_command",
                    &context_memory_options_.use_pane_command);
    config->GetInt("copilot/context_memory/max_entries", &context_memory_options_.max_entries);
    config->GetBool("copilot/context_memory/debug", &context_memory_options_.debug);
    // Absent leaves it at -1, "no opinion", which is the pre-existing
    // behaviour: a pane with no history keeps whatever mode it arrived in.
    bool default_ascii = false;
    if (config->GetBool("copilot/context_memory/default_ascii_mode", &default_ascii)) {
      context_memory_options_.default_ascii_mode = default_ascii ? 1 : 0;
    }
    if (context_memory_options_.max_entries < 1) context_memory_options_.max_entries = 1;

    // Both the IMK hook and the tmux pane scrape return this many characters
    // — the prediction context and the re-ranking filter each have their own
    // length, and both surrounding-text sources should reach equally deep.
    // Which declarations count, and the clamping, live in
    // SurroundingPrefixChars (surrounding_source.h) so they can be tested
    // without an engine; this block only reads the keys.
    SurroundingConsumers consumers;
    config->GetBool("copilot/use_surrounding_context", &use_surrounding_context_);
    config->GetInt("copilot/surrounding_context_chars", &surrounding_context_chars_);
    surrounding_context_chars_ =
        std::clamp(surrounding_context_chars_, 1, kMaxSurroundingPrefixChars);
    consumers.use_surrounding_context = use_surrounding_context_;
    consumers.surrounding_context_chars = surrounding_context_chars_;
    bool rerank_enable = true;
    int rerank_chars = 8;
    config->GetBool("copilot/rerank/enable", &rerank_enable);
    config->GetInt("copilot/rerank/max_context_chars", &rerank_chars);
    rerank_max_context_chars_ = std::clamp(rerank_chars, 1, kMaxSurroundingPrefixChars);
    consumers.rerank_enable = rerank_enable;
    consumers.rerank_max_context_chars = rerank_max_context_chars_;
    // The SCORER's own context length -- a different and longer string than the
    // db's Han-only tail. Read here because WarmRerankContext keys the warm
    // cache on it while CopilotRerankFilter reads the same key for Apply(); it
    // was a hard-coded 32 (copilot.h) against a filter that read config, and
    // the two agreed only because the schema happened to say 32 and the
    // 8-character source ceiling made every value above 8 indistinguishable.
    //
    // Now a term in the fetch depth as well, which is what makes that ceiling
    // go away: with it declared but not folded in, the sources stopped at 8
    // whatever the schema said, and 71% of measured fetches ended in
    // kByConfig. It is clamped here for the same reason -- an unclamped key
    // that only ever truncated a string it could not lengthen now sizes a
    // per-keystroke query.
    config->GetInt("copilot/rerank/llm/context_chars", &rerank_llm_context_chars_);
    rerank_llm_context_chars_ =
        std::clamp(rerank_llm_context_chars_, 1, kMaxSurroundingPrefixChars);
    config->GetBool("copilot/rerank/llm/enable", &consumers.llm_enable);
    consumers.llm_context_chars = rerank_llm_context_chars_;
    config->GetBool("copilot/rerank/llm/battery_active", &rerank_llm_battery_active_);
    const int prefix_chars = SurroundingPrefixChars(consumers);
    surrounding_prefix_chars_ = prefix_chars;
    LOG(INFO) << "[copilot] surrounding prefix_chars=" << prefix_chars
              << " (surrounding=" << consumers.surrounding_context_chars
              << " use=" << consumers.use_surrounding_context
              << ", rerank=" << consumers.rerank_max_context_chars
              << " enable=" << consumers.rerank_enable << ", llm=" << consumers.llm_context_chars
              << " enable=" << consumers.llm_enable << ")";
#ifdef __APPLE__
    SetIMKSurroundingPrefixChars(prefix_chars);
#endif

    // Gates the "which source won" line in surrounding_source.cc and the
    // refusal lines in tmux_source.cc with LOG(INFO) instead of DLOG: the
    // librime build this ships in compiles -DNDEBUG, under which DLOG never
    // prints, so without this the user has no way to see which surrounding
    // source answered (or why tmux refused) in the build they actually run.
    bool surrounding_debug = false;
    config->GetBool("copilot/surrounding_debug", &surrounding_debug);
    SetSurroundingDebug(surrounding_debug);

    TmuxSourceConfig tmux_config;
    tmux_config.debug = surrounding_debug;
    config->GetBool("copilot/tmux_source/enabled", &tmux_config.enabled);
    config->GetString("copilot/tmux_source/binary", &tmux_config.binary);
    config->GetString("copilot/tmux_source/socket", &tmux_config.socket);
    config->GetInt("copilot/tmux_source/timeout_ms", &tmux_config.timeout_ms);
    tmux_config.timeout_ms = std::clamp(tmux_config.timeout_ms, kMinTimeoutMs, kMaxTimeoutMs);
    tmux_config.prefix_chars = prefix_chars;
    // Left empty, ConfigureTmuxSource substitutes DefaultTerminalBundleIds().
    // A schema that does set the list *replaces* the built-in one: the default
    // exists so the out-of-the-box behavior is safe, not to forbid a
    // deliberate override, and intersecting would lock out terminals we never
    // listed. The user then owns what they put in it.
    if (auto list = config->GetList("copilot/tmux_source/app_bundle_ids")) {
      for (size_t i = 0; i < list->size(); ++i) {
        if (auto item = list->GetValueAt(i)) {
          string bundle_id;
          if (item->GetString(&bundle_id)) {
            tmux_config.app_bundle_ids.push_back(bundle_id);
          }
        }
      }
    }
    ConfigureTmuxSource(tmux_config);

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

    // AsciiComposer returns kRejected for letter keys while ascii_mode is true
    // (librime ascii_composer.cc:129-140) and the engine breaks the processor
    // loop on kRejected (engine.cc:104-105). Ordered after it, this processor
    // never runs in English mode, and context memory is dead in the
    // English-to-Chinese direction only -- which reads as flakiness, not as
    // misconfiguration. Warn loudly rather than let that be diagnosed by hand.
    //
    // Inside this block rather than re-fetching the config for itself: the
    // re-fetch also dereferenced the result without a null check, which the
    // `if` opening this block is exactly what provides.
    if (context_memory_options_.enable) {
      if (auto processors = config->GetList("engine/processors")) {
        int copilot_at = -1, ascii_at = -1;
        for (size_t i = 0; i < processors->size(); ++i) {
          auto value = processors->GetValueAt(i);
          if (!value) continue;
          const std::string name = value->str();
          if (copilot_at < 0 && name == "copilot") copilot_at = static_cast<int>(i);
          if (ascii_at < 0 && name == "ascii_composer") ascii_at = static_cast<int>(i);
        }
        if (copilot_at >= 0 && ascii_at >= 0 && copilot_at > ascii_at) {
          LOG(WARNING) << "[ctxmem] `copilot` is ordered AFTER `ascii_composer` in "
                          "engine/processors. Context memory will not work in the "
                          "English-to-Chinese direction. Move `copilot` before "
                          "`ascii_composer`.";
        }
      }
    }
  }

  // Only worth tracking AC/battery state -- and paying for the monitor
  // callback -- when there is a scorer to gate at all, and the config cares
  // (battery_active already means "run regardless"). Same condition and same
  // pattern CopilotRerankFilter uses for the identical reason.
  if (copilot_engine_ && copilot_engine_->scorer() && !rerank_llm_battery_active_) {
    is_on_ac_ = ::copilot::IsACPowerConnected();
    power_token_ =
        ::copilot::RegisterPowerChange([this](bool is_ac_power) { is_on_ac_ = is_ac_power; });
  }

  // Register processors based on config
  if (disabled_plugins.find("ime_bridge") == disabled_plugins.end()) {
    processors_.emplace_back(std::make_shared<ImeBridge>(ticket));
  }
  if (disabled_plugins.find("auto_spacer") == disabled_plugins.end()) {
    // AutoSpacer's Space/Enter/number commits bypass Context::Commit()
    // entirely (engine_->CommitText() + ctx->Clear(), never ctx->Commit()),
    // so OnCommit's warm (hung off commit_notifier()) never sees them. This
    // is how those commits reach WarmRerankContext instead -- see
    // auto_spacer.h's constructor comment.
    processors_.emplace_back(std::make_shared<AutoSpacer>(
        ticket, [this](Context* ctx, const string& committed, bool selection_commit) {
          WarmRerankContext(ctx, committed);
          // Same reasoning as OnCommit: telemetry must see the commits that
          // actually happen, and in this configuration these ARE the commits
          // -- see EmitCommitTelemetry's comment (copilot.h). `selection_commit`
          // is false on AutoSpacer's two bail-out paths (Enter, the number-key
          // raw fallback): the user discarded every candidate, so the
          // still-highlighted one must not be recorded as accepted --
          // BuildCommitEvents' own comment (telemetry_commit.h) has the
          // reasoning; WarmRerankContext above still runs unconditionally,
          // since warming for the next input is unrelated to that accounting.
          EmitCommitTelemetry(ctx, selection_commit);
        }));
  }
  if (disabled_plugins.find("select_character") == disabled_plugins.end()) {
    processors_.emplace_back(std::make_shared<SelectCharacter>(ticket, [this](const string& text) {
      auto* ctx = engine_->context();
      CopilotAndUpdate(ctx, text);  // ✨ 立即启动后续预测
    }));
  }
  // The table is process-wide (context_memory.h); only last_key_ lives here,
  // in the per-session Step. Built unconditionally so ProcessKeyEvent never
  // has to null-check it -- with `enable` false the Step itself is inert.
  context_memory_step_ =
      std::make_unique<context_memory::Step>(&context_memory::Instance(), context_memory_options_);

  LOG(INFO) << "Copilot plugin Loaded. Disabled plugins: " << disabled_plugins.size();
}

Copilot::~Copilot() {
  select_connection_.disconnect();
  context_update_connection_.disconnect();
  commit_connection_.disconnect();
  // Session end: this processor dies on schema redeploy or session teardown,
  // either of which can arrive well inside the periodic window, and "no
  // stats line beyond what is meaningful" (FlushStatsIfAny's own gate) means
  // a short session that never crosses kStatsFlushIntervalSec would
  // otherwise report nothing at all.
  FlushStatsIfAny();
  // After the flush, never before: the stats line it just wrote belongs in
  // the copy. Unconditional at session end for the same reason the flush is
  // -- a session shorter than kTelemetrySyncIntervalSec would otherwise ship
  // nothing at all, and schema redeploy ends one of these on every deploy.
  if (telemetry_options_.auto_sync) {
    SyncTelemetry();
  }
  // The monitor is a process-wide singleton: leaving the `this`-capturing
  // callback registered means the next plug/unplug writes into freed memory
  // (this processor dies on every schema redeploy) -- same hazard and same
  // fix as CopilotRerankFilter (rerank_filter.cc) and LLMProvider.
  ::copilot::UnregisterPowerChange(power_token_);
  power_token_ = 0;
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

  // Start of a key event: the tmux pane may have moved on, so drop the
  // memoized scrape. Everything downstream in this event -- AutoSpacer, the
  // re-ranking filter's menu build, GetPredictionContext -- then shares one
  // `posix_spawn` instead of forking two or three times per keystroke.
  //
  // Not while composing, for the same reason imk_client.mm skips its query
  // there: the preedit is drawn by the frontend and never reaches the PTY, so
  // tmux's grid cannot change until something is committed. That freezes the
  // snapshot at composition start, which is also what GetPredictionContext
  // already documents itself as relying on.

  // A scope guard because ProcessKeyEvent has seven return points. Seven
  // hand-written assignments will eventually be six, and the branch that loses
  // one is where the mode was just toggled -- after which the feature records
  // the wrong value silently, which is exactly the bug it exists to fix.
  //
  // `step` stays null unless the head below actually ran, and `resolved` stays
  // false unless the head resolved an identity THIS event: without that
  // condition an event whose identity is momentarily unavailable (tmux timed
  // out, the server went away) would have the guard write the current mode
  // into the slot of the pane the user was in before.
  //
  // `bridge_writes` is sampled at the head and compared here because
  // ImeBridge's pending-action queue carries no context: an action queued by
  // pane A's nvim is applied by RunProcessors on whatever pane the next
  // keystroke lands in, and without this the tail would read the bridge's
  // value and record it as that pane's -- turning a one-keystroke
  // misapplication into a remembered one. Precedence is unchanged: the bridge
  // still wins the mode, it just does not get to name a pane.
  struct ContextMemoryTail {
    context_memory::Step* step;
    Context* ctx;
    std::string key;
    bool resolved;
    uint64_t bridge_writes;
    bool debug;
    ~ContextMemoryTail() {
      if (!step) return;
      if (resolved && ctx) {
        const bool bridge_moved =
            ImeBridgeServer::Instance().applied_mode_writes() != bridge_writes;
        const bool recorded = ctx->get_option("ascii_mode");
        step->OnTail(key, recorded, bridge_moved);
        // The other half of the diagnostic, and the half that was missing: the
        // head says what was restored, and only this says what was WRITTEN.
        // A wrong value in the table is invisible until the next visit reads
        // it back, by which time the keystroke that wrote it is long gone.
        if (debug) {
          LOG(INFO) << "[ctxmem] tail " << key
                    << (bridge_moved ? " NOT recorded (bridge wrote)" : " recorded ascii_mode=")
                    << (bridge_moved ? "" : (recorded ? "1" : "0"));
        }
      } else {
        step->OnTailUnresolved();
      }
    }
  } tail{nullptr, ctx, std::string(), false, 0, context_memory_options_.debug};

  if (!ctx || !ctx->IsComposing()) {
    InvalidateTmuxSnapshot();

    // Inside this guard on purpose, and not as an optimisation: the preedit is
    // drawn by the frontend and never reaches the PTY, so the pane provably
    // cannot change mid-composition -- and changing ascii_mode there would be
    // destructive.
    if (context_memory_options_.enable && ctx) {
      tail.step = context_memory_step_.get();
      // Sampled BEFORE RunProcessors, which is where ImeBridge::Process drains
      // the queue -- a sample taken any later would already include the write
      // it exists to detect.
      tail.bridge_writes = ImeBridgeServer::Instance().applied_mode_writes();
      if (auto resolved = GetContextIdentity()) {
        const std::string key =
            context_memory::MakeKey(resolved->id, context_memory_options_.use_pane_command);
        const std::string previous = context_memory_step_->last_key();
        // Contains, not Get: Get splices the LRU, and a read with a side
        // effect has no place in what is only ever a diagnostic. Evaluated
        // unconditionally rather than under `debug` on purpose -- hoisting it
        // under the flag would make the flag a behaviour switch.
        const bool before = ctx->get_option("ascii_mode");
        // The same MakeKey, with the command left off: the pane alone. Built
        // here rather than split back out of `key` so the delimiter rule stays
        // in one place -- see Step::OnHead.
        const std::string pane_key = context_memory::MakeKey(resolved->id, false);
        const auto outcome = context_memory_step_->OnHead(
            key, pane_key, before, [ctx](bool v) { ctx->set_option("ascii_mode", v); });
        if (context_memory_options_.debug && key != previous) {
          // Reported by OnHead, not inferred here from before/after. The
          // inferred version printed "no memory" over an event where the
          // default had just been applied -- and this is the one line the
          // feature is debugged through.
          const bool after = ctx->get_option("ascii_mode");
          std::string what;
          using Outcome = context_memory::Step::HeadOutcome;
          switch (outcome) {
            case Outcome::kRestored:
              what = std::string(" restored ascii_mode=") + (after ? "1" : "0");
              break;
            case Outcome::kAlreadyMatched:
              what = std::string(" remembered ascii_mode=") + (after ? "1" : "0") + ", already set";
              break;
            case Outcome::kDefaulted:
              what = std::string(" new pane; default_ascii_mode -> ") + (after ? "1" : "0") +
                     (after == before ? " (already set)" : "");
              break;
            case Outcome::kNothingKnown:
              what = " no memory and no default; leaving ascii_mode as-is";
              break;
            case Outcome::kSameContext:
              what = " (same context)";
              break;
          }
          LOG(INFO) << "[ctxmem] " << (previous.empty() ? "(none)" : previous) << " -> " << key
                    << " via " << DescribeIdentitySource(resolved->source) << what;
        }
        tail.key = key;
        tail.resolved = true;
      } else {
        context_memory_step_->OnUnresolved();
      }
    }
  }

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
  // A composition that ends without a commit — Esc, a click elsewhere, any
  // Context::Clear() — must not leave its re-ranking decisions behind. The
  // next composition can reuse the same input and span, and a stale trace
  // matched against it would credit a promotion that never happened, with the
  // wrong `ctx`. See the header of rerank_trace.h: dropping an event is fine,
  // misattributing one is not.
  //
  // Here rather than in ProcessKeyEvent's !IsComposing() branch because
  // Context::Clear() fires update_notifier_ itself (librime
  // src/rime/context.cc:106-111) and AbortComposition() goes through Clear(),
  // so this runs the instant the composition is abandoned instead of waiting
  // for the next keystroke. Above the guards below on purpose: neither the
  // re-ranking filter nor telemetry consults the `copilot` switch, so a user
  // who has it off still accumulates traces that must still be cleared.
  if (ctx && !ctx->IsComposing() && rerank_traces_) {
    rerank_traces_->Clear();
  }

  // Warm the re-ranking scorer the instant a composition begins: the
  // surrounding text is definitely current here, unlike a commit-time warm,
  // which can already be stale if the caret moved before the user typed again
  // (task-5-brief.md). Sampled on the false->true edge of IsComposing() so
  // this fires once per composition, not once per keystroke. Above the guard
  // below on purpose, same reasoning as the trace-clear above: the re-ranking
  // filter consults neither the `copilot` switch nor self_updating_, so
  // gating the warm behind either would silently miss the inputs the filter
  // still re-ranks. Any resulting WarmUp() call that lands on a context an
  // OnCommit warm already queued is deduped inside the scorer itself
  // (LlmScorer::WarmUp), not here.
  const bool composing_now = ctx && ctx->IsComposing();
  if (composing_now && !was_composing_) {
    WarmRerankContext(ctx, {});
  }
  was_composing_ = composing_now;

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

string Copilot::GetPredictionContext(const string& committed) const {
  if (!use_surrounding_context_) {
    return {};
  }
  auto caret = GetCaretContext(engine_->context(), AllowReconstruction::kNo);
  if (!caret) {
    return {};  // no real context; the providers fall back to commit history
  }
  // The snapshot was taken before this key event was handled (and is frozen
  // while composing), so it does not contain the text this commit just added.
  return BuildPredictionContext(caret->before, committed);
}

void Copilot::CopilotAndUpdate(Context* ctx, const string& context_query) {
  // The `copilot` switch is the single authority on whether predictions
  // appear, and the check belongs HERE rather than at the call sites --
  // OnContextUpdate had one and SelectCharacter's on_accept callback did not,
  // so picking a character off a candidate popped a prediction menu on a
  // schema whose switch reads 關閉預測. A gate at the choke point cannot be
  // forgotten by the next caller; OnContextUpdate's own check stays, since it
  // also guards self_updating_ and the punctuation path above it.
  if (!ctx || !ctx->get_option("copilot")) {
    return;
  }
  // auto history = copilot_engine_->history();
  // LOG(INFO) << "CopilotAndUpdate: " << history->get_chars(10)
  //           << " context_query: " << context_query;
  const string prediction_context = GetPredictionContext(context_query);
  DLOG(INFO) << "[Copilot] prediction context: '" << prediction_context << "'";
  if (copilot_engine_->Copilot(ctx, context_query, prediction_context)) {
    copilot_engine_->CreateCopilotSegment(ctx);
    self_updating_ = true;
    ctx->update_notifier()(ctx);
    self_updating_ = false;
  }
}

void Copilot::WarmRerankContext(Context* ctx, const string& extra_committed) {
  if (!copilot_engine_) {
    return;
  }
  Scorer* scorer = copilot_engine_->scorer();
  if (!scorer) {
    return;  // rerank/llm disabled or unconfigured: no-op, same as before Task 5
  }
  // The same battery gate CopilotRerankFilter::Apply applies before it will
  // even consult the warm cache (rerank_filter.cc) -- warming here must not
  // load the ~1 GB model on battery power for a context Apply() would refuse
  // to score against anyway.
  if (!rerank_llm_battery_active_ && !is_on_ac_) {
    DLOG(INFO) << "[copilot] rerank warm: skipped, on battery";
    return;
  }
  auto caret = GetCaretContext(ctx, AllowReconstruction::kNo);
  if (!caret) {
    return;  // no real surrounding text -- same guard the filter applies
  }
  // BuildScoringContextFor, not TrailingCjkRun: the warm cache is keyed by the
  // exact string, and CopilotRerankFilter asks it about the same function's
  // result. Warming a different string does not fail loudly -- every warm
  // succeeds and every Apply() finds the cache cold, so the feature silently
  // never runs.
  //
  // BuildScoringContextFor takes a SurroundingText and reads only `.before` --
  // the rest plays no role here, so a stub built from caret->before is
  // equivalent to passing the real query result.
  SurroundingText for_scoring;
  for_scoring.before = caret->before;
  const string context =
      BuildScoringContextFor(for_scoring, extra_committed, rerank_llm_context_chars_);
  if (context.empty()) {
    return;
  }
  scorer->WarmUp(context);
}

void Copilot::OnCommit(Context* ctx) {
  // A `dumb` commit emits no text -- it exists only to notify. AutoSpacer
  // raises it (NotifyForLearning) after it has already emitted the decorated
  // text itself and already called on_commit_, which is where this function's
  // two jobs have just been done. Running them again would write a duplicate
  // telemetry line per commit, and warm the scorer with GetCommitText(),
  // which under `dumb` is the empty string -- so the duplicate would also be
  // the wrong one.
  //
  // Keying on the option rather than an in-flight flag owned by AutoSpacer:
  // `dumb` already means "not going to commit anything" (switcher.cc:24), so
  // this is the existing contract rather than new cross-object state.
  if (ctx && ctx->get_option("dumb")) {
    return;
  }

  // Context::Commit() fires this notifier before Clear() (librime
  // src/rime/context.cc:18-26), so the composition, its menus and every
  // selected_index are still readable here -- the same requirement
  // EmitCommitTelemetry's own comment (copilot.h) states for its other call
  // site, AutoSpacer's on_commit_.

  // The context for the NEXT input is fully known the instant this commit
  // lands -- GetCommitText() is the whole composition's confirmed text at
  // once, the commit-time analogue of ConfirmedPrefix walking a still-
  // composing one segment at a time (rerank_filter.cc). The user then takes
  // on the order of a second to type again, comfortably more than the LLM
  // prefill this hides. Deliberately above the telemetry early-return below:
  // the two features are unrelated, and a user who has telemetry off must
  // still get a warm scorer.
  if (ctx) {
    WarmRerankContext(ctx, ctx->GetCommitText());
  }

  EmitCommitTelemetry(ctx);
}

void Copilot::EmitCommitTelemetry(Context* ctx, bool selection_commit) {
  // Every decision lives in BuildCommitEvents, which is tested against
  // hand-built compositions. Keep this function a call plus a loop: a
  // condition added here would be a condition with no test.
  if (!telemetry_ || !telemetry_options_.enable) {
    return;
  }
  // The SAME function GetTelemetryWriter derives the filename from, which is
  // what actually keeps the `machine` field and the file it lives in in step.
  // Two copies of the fallback used to sit here under a comment claiming they
  // could not disagree; they disagreed for four days, because only one of the
  // two re-reads user_id (telemetry.h, MachineName). Sharing the spelling is
  // not what fixes that -- GetTelemetryWriter rebuilding on a change is -- but
  // one spelling is what makes the two provably the same string.
  const string machine = telemetry::MachineName(Service::instance().deployer().user_id);
  // Only worth accumulating when there is an LLM path to report on -- same
  // condition the constructor already uses for the AC/battery monitor.
  // Without this, a schema with telemetry on but the LLM off would
  // eventually flush an all-zero stats line, which is noise, not a "no LLM,
  // no stats lines beyond what is meaningful" state.
  telemetry::StatsAccumulator* stats =
      (copilot_engine_ && copilot_engine_->scorer()) ? &stats_ : nullptr;
  const auto events = telemetry::BuildCommitEvents(
      ctx, rerank_traces_.get(), telemetry_options_, machine,
      engine_->schema() ? engine_->schema()->schema_id() : string(),
      telemetry::FormatTimestamp(std::time(nullptr)), stats, selection_commit, &telemetry_ok_seen_);
  for (const auto& e : events) {
    telemetry_->Write(telemetry::SerializeJsonl(e));
  }

  if (stats) {
    const std::time_t now = std::time(nullptr);
    if (last_stats_flush_ == 0) {
      last_stats_flush_ = now;  // open the first window rather than firing immediately
    } else if (now - last_stats_flush_ >= kStatsFlushIntervalSec) {
      FlushStatsIfAny();
      last_stats_flush_ = now;
    }
  }

  if (telemetry_options_.auto_sync) {
    const std::time_t now = std::time(nullptr);
    if (last_telemetry_sync_ == 0) {
      last_telemetry_sync_ = now;  // open the first window rather than firing immediately
    } else if (telemetry::ShouldSync(last_telemetry_sync_, now, kTelemetrySyncIntervalSec)) {
      SyncTelemetry();
      last_telemetry_sync_ = now;
    }
  }

  if (rerank_traces_) {
    rerank_traces_->Clear();
  }
}

void Copilot::SyncTelemetry() {
  if (!telemetry_ || !telemetry_options_.enable) {
    return;
  }
  // The same deployer copilot.cc already takes user_id from, and the same
  // field tools/sync_telemetry.sh parses out of installation.yaml -- read
  // directly, so nothing here parses YAML.
  const auto& sync_dir = Service::instance().deployer().sync_dir;
  if (sync_dir.empty()) {
    // Once per process: this runs on a timer, so a line per commit would be
    // noise nobody reads. Not silent, though -- an unset sync_dir is the
    // likely state and looks exactly like a working install from outside.
    static bool warned = false;
    if (!warned) {
      warned = true;
      LOG(ERROR) << "[copilot] telemetry auto_sync is on but sync_dir is unset; add "
                    "`sync_dir: \"/path/to/sync\"` to installation.yaml -- Squirrel never "
                    "writes one";
    }
    return;
  }
  // Flat under the sync dir, exactly where tools/sync_telemetry.sh puts it,
  // and deliberately NOT under <sync_dir>/<installation_id>/, which Rime's
  // own sync task manages.
  const int copied = telemetry_->SyncTo(sync_dir / "copilot_telemetry");
  DLOG(INFO) << "[copilot] telemetry: synced " << copied << " file(s) to " << sync_dir;
}

void Copilot::FlushStatsIfAny() {
  if (!telemetry_ || !telemetry_options_.enable) {
    return;
  }
  if (stats_.segments() == 0) {
    return;  // nothing observed since construction or the last flush
  }
  telemetry_->Write(telemetry::SerializeStatsJsonl(
      stats_.Snapshot(telemetry::FormatTimestamp(std::time(nullptr)), surrounding_prefix_chars_)));
  stats_.Reset();
}

CopilotComponent::CopilotComponent(an<CopilotEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

CopilotComponent::~CopilotComponent() {}

Copilot* CopilotComponent::Create(const Ticket& ticket) {
  telemetry::Options telemetry_options;
  string schema_id;
  if (auto* schema = ticket.schema) {
    schema_id = schema->schema_id();
    if (auto* config = schema->config()) {
      config->GetBool("copilot/telemetry/enable", &telemetry_options.enable);
      config->GetInt("copilot/telemetry/top_n", &telemetry_options.top_n);
      int max_file_bytes = static_cast<int>(telemetry_options.max_file_bytes);
      if (config->GetInt("copilot/telemetry/max_file_bytes", &max_file_bytes)) {
        telemetry_options.max_file_bytes = max_file_bytes;
      }
      config->GetInt("copilot/telemetry/keep_generations", &telemetry_options.keep_generations);
      config->GetInt("copilot/telemetry/sample_ok", &telemetry_options.sample_ok);
      config->GetBool("copilot/telemetry/auto_sync", &telemetry_options.auto_sync);
    }
  }
  telemetry::ClampOptions(telemetry_options);
  return new Copilot(ticket, engine_factory_->GetInstance(ticket),
                     engine_factory_->GetRerankTraces(schema_id),
                     engine_factory_->GetTelemetryWriter(telemetry_options), telemetry_options);
}

}  // namespace rime
