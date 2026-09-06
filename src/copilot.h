#pragma once

#include <rime/processor.h>

#include <ctime>
#include <memory>

#include "context_memory_step.h"
#include "rerank_trace.h"
#include "telemetry.h"
#include "telemetry_stats.h"
#include "utils.h"  // copilot::PowerChangeToken

namespace rime {

class Context;
class CopilotEngine;
class CopilotEngineComponent;

class Copilot : public Processor {
 public:
  Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine,
          an<RerankTraceStore> rerank_traces, an<telemetry::Writer> telemetry,
          const telemetry::Options& telemetry_options);
  virtual ~Copilot();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 protected:
  void OnContextUpdate(Context* ctx);
  void OnSelect(Context* ctx);
  void CopilotAndUpdate(Context* ctx, const string& context_query);
  void OnCommit(Context* ctx);

 private:
  ProcessResult RunProcessors(const KeyEvent& key_event);
  // Text before the caret to predict from: the frontend's surrounding-text
  // snapshot plus `committed`, or empty when no real context is available (the
  // engine then falls back to the plugin's own commit history).
  string GetPredictionContext(const string& committed) const;

  // Prefills the re-ranking filter's scorer (CopilotEngine::scorer(), Task 5)
  // for the context it will need next, before the filter itself would have to
  // ask on a cache miss -- one keystroke too late to help that segment.
  // `extra_committed` is text about to reach the app that the surrounding-text
  // snapshot does not know about yet: empty when called from a fresh
  // composition (nothing committed since the snapshot), or the composition's
  // full commit text when called from OnCommit. Builds the context exactly as
  // CopilotRerankFilter::Apply does, or the warm can never be hit -- see
  // rerank_filter.cc. A no-op whenever there is no scorer to warm (rerank/llm
  // disabled) or no real surrounding text to warm it with.
  void WarmRerankContext(Context* ctx, const string& extra_committed);

  // The telemetry half of OnCommit, factored out so it can also run from
  // AutoSpacer's on_commit_ callback (constructor). AutoSpacer's Space/Enter/
  // number-key commits go through engine_->CommitText() + ctx->Clear(), never
  // ctx->Commit(), so commit_notifier() -- what OnCommit hangs off -- never
  // fires for them; they are also the ONLY commits available in the
  // surrounding-text configuration, which is the only configuration
  // re-ranking runs in at all. Without this, the telemetry this feature
  // exists to produce is fed from a path the feature itself bypasses, and
  // worse: ctx->Clear() (called right after on_commit_ returns) fires
  // update_notifier(), and OnContextUpdate clears rerank_traces_ -- so those
  // segments' traces would be discarded before anything observes them.
  // Requires the same thing OnCommit's own doc comment does: composition,
  // menus and selected_index still readable, i.e. called before ctx->Clear().
  //
  // `selection_commit` is false for AutoSpacer's two bail-out paths -- Enter's
  // raw commit and the number-key fallback's raw commit -- where the user
  // discarded every candidate and committed raw ASCII instead. Passed straight
  // through to BuildCommitEvents (telemetry_commit.h), whose header comment
  // has the full reasoning: no Event is produced for a bail-out (it would
  // misreport the still-highlighted candidate as accepted), but stats still
  // observe it. Defaults to true so OnCommit's own call -- always a real
  // Context::Commit(), never a bail-out -- is unaffected.
  void EmitCommitTelemetry(Context* ctx, bool selection_commit = true);

  // Writes the accumulated stats line (telemetry_stats.h) and resets the
  // accumulator, but only when there is something to say: an empty window
  // would be a stats line of all-zero counters forever whenever the LLM path
  // is unconfigured, which is exactly the "no stats lines beyond what is
  // meaningful" requirement -- gating on segments() rather than on the LLM
  // config directly keeps this one function correct regardless of how that
  // config is read elsewhere.
  void FlushStatsIfAny();

  // Copies this machine's telemetry into Rime's sync directory
  // (telemetry::Writer::SyncTo). A no-op when telemetry is off; loud, once,
  // when `sync_dir` is unset -- that is the likely state, not the odd one,
  // since Squirrel never writes one, and a feature that silently does
  // nothing for a week is exactly what auto-sync exists to prevent.
  void SyncTelemetry();

  enum Action { kUnspecified, kSelect, kDelete, kSpecial };
  Action last_action_ = kUnspecified;
  bool self_updating_ = false;
  int iteration_counter_ = 0;  // times has been copiloted

  an<CopilotEngine> copilot_engine_;
  connection select_connection_;
  connection context_update_connection_;
  connection delete_connection_;
  connection commit_connection_;
  an<RerankTraceStore> rerank_traces_;
  an<telemetry::Writer> telemetry_;
  telemetry::Options telemetry_options_;
  // Plain successes seen since construction, for Options::sample_ok's 1-in-N.
  // Never reset: the sampling only needs to be uniform, not windowed.
  int64_t telemetry_ok_seen_ = 0;
  // Window for the aggregate stats line (telemetry_stats.h /
  // telemetry_event.h's StatsLine) -- observed on every OnCommit alongside
  // the per-event writes, flushed through the same telemetry_->Write() call
  // those use rather than a second mechanism, so it never sits on the input
  // thread any more than a commit already does. Every field defaults to
  // empty/zero, which is also the "not yet observed anything" state
  // FlushStatsIfAny relies on to skip an all-zero line.
  telemetry::StatsAccumulator stats_;
  // The previous context handed to Scorer::WarmUp, for ObserveWarm's
  // classification. Not cleared with the stats window: the question is whether
  // consecutive warms extend each other, and a window boundary is not a break
  // in that sequence.
  string last_warm_context_;
  // 0 means "no window open yet"; the first commit after construction (or
  // after a flush) opens one instead of firing immediately, so a session that
  // never commits again after startup does not need a special case here --
  // the destructor's FlushStatsIfAny() covers it.
  std::time_t last_stats_flush_ = 0;
  // 5 minutes: frequent enough that a session lasting less than the interval
  // still gets covered by the destructor's session-end flush, infrequent
  // enough that this is a rare Write() call, not a periodic timer thread --
  // there is no new mechanism here, only a check piggybacked on OnCommit.
  static constexpr std::time_t kStatsFlushIntervalSec = 300;
  // Same shape and the same zero convention as last_stats_flush_ above, for
  // copilot/telemetry/auto_sync. Not a thread: the file measured 8.5 KB over
  // two days against an 8 MB cap, so the copy is microseconds -- there is no
  // new mechanism here either, only a second check piggybacked on OnCommit.
  std::time_t last_telemetry_sync_ = 0;
  // 30 minutes. Longer than the stats flush because the unit of work is a
  // whole-file copy into an iCloud-backed directory rather than an append,
  // and because nothing downstream reads it sooner than a person does.
  static constexpr std::time_t kTelemetrySyncIntervalSec = 1800;

  int last_keycode_ = 0;
  bool use_surrounding_context_ = true;
  int surrounding_context_chars_ = 8;
  // copilot/rerank/max_context_chars, copilot/rerank/llm/context_chars and
  // copilot/rerank/llm/battery_active, all three taken from
  // ReadCopilotSharedConfig (copilot_config.h) -- the same call
  // CopilotRerankFilterComponent::Create makes. That is what lets
  // WarmRerankContext build the identical context and honor the identical
  // battery gate the filter applies before it would ever call WarmUp(); the
  // two used to read and clamp these keys separately, and a disagreement does
  // not fail loudly -- every warm lands on a string nobody asks about and the
  // feature silently never runs.
  //
  // The initializers below are dead weight now (the reader supplies its own
  // defaults, and clamps both lengths) and are kept only so an instance is
  // never momentarily uninitialized. The SCORER's context is a different and
  // longer string than the db's Han-only tail: a language model reads
  // punctuation and Latin, an n-gram key cannot.
  int rerank_max_context_chars_ = 8;
  int rerank_llm_context_chars_ = 32;
  bool rerank_llm_battery_active_ = false;
  // SurroundingPrefixChars' result, kept only so FlushStatsIfAny can stamp it
  // on every stats line. `trunc_counts["config"]` is a count of fetches this
  // cap cut short, and that count means nothing without the cap -- which
  // changed on 2026-08-28 when context_chars became a term in it. Config-fixed,
  // read once in the constructor.
  int surrounding_prefix_chars_ = 1;
  // Cached rather than queried on every warm attempt -- same reason and same
  // pattern as CopilotRerankFilter (rerank_filter.h/.cc) and LLMProvider
  // (llm_provider.cc): kept current by a process-wide power monitor callback
  // instead of a syscall per commit/composition-start. Assumed true (AC)
  // until told otherwise, same fallback IsACPowerConnected() itself uses.
  bool is_on_ac_ = true;
  ::copilot::PowerChangeToken power_token_ = 0;
  // Composition-start detection for WarmRerankContext: sampled once per
  // OnContextUpdate (which update_notifier fires on every IsComposing()-
  // affecting change -- PushInput/Pop/DeleteInput/Clear), so the warm fires
  // exactly on the false->true edge instead of once per keystroke.
  bool was_composing_ = false;
  // copilot/context_memory/*. `enable` defaults to false: the feature ships
  // off, and every read below only ever narrows that.
  context_memory::Options context_memory_options_;
  // Holds last_key_; see context_memory_step.h for why it is per-session while
  // the table behind it is process-wide.
  std::unique_ptr<context_memory::Step> context_memory_step_;
  std::vector<std::shared_ptr<Processor>> processors_;
};

class CopilotComponent : public Copilot::Component {
 public:
  explicit CopilotComponent(an<CopilotEngineComponent> engine_factory);
  virtual ~CopilotComponent();

  Copilot* Create(const Ticket& ticket) override;

 protected:
  an<CopilotEngineComponent> engine_factory_;
};

}  // namespace rime
