#pragma once

#include <functional>
#include <optional>
#include <string>
#include <unordered_map>

#include "copilot_plugin.h"
#include "imk_client.h"

namespace rime {

class Candidate;
class Context;

// True when `cand` — the candidate the user just picked — converts only a
// PREFIX of the current input, so Rime would confirm that segment and keep
// composing the rest (typing 电脑 as "dmnc" and picking 电, which spans "dm").
//
// The AutoSpacer must defer to Rime's Selector in that state instead of
// committing: Composition::GetCommitText() appends the unconverted tail
// verbatim, so committing would put "云uu" on screen and steal the user's
// chance to pick 枢.
bool SelectionLeavesUnconvertedInput(Context* ctx, const an<Candidate>& cand);

// Compute the text to commit when Space finalizes the current composition,
// including CJK/Latin auto-spacing against the surrounding `before`/`after`.
//
// The whole composition is committed (all selected segments concatenated via
// Context::GetCommitText), NOT just the last segment — committing only the
// last segment drops earlier selections of a long, multi-segment input.
//
// Declared here (rather than kept file-local) so it can be unit-tested with a
// hand-built Context, without standing up a full Rime engine.
std::string ComputeSpaceCommitText(Context* ctx, const std::string& before,
                                   const std::string& after, bool enable_right_space);

class AutoSpacer : public CopilotPlugin<AutoSpacer> {
 public:
  // `on_commit` fires for every commit AutoSpacer itself performs (Space,
  // Enter, and the two number-key paths in ProcessWithSurroundingContext) --
  // the commits that bypass Context::Commit()/commit_notifier() entirely
  // (engine_->CommitText() + ctx->Clear(), never ctx->Commit()). Those are
  // the only commits available in the surrounding-text configuration, which
  // is also the only configuration the re-ranking filter runs in, so
  // Copilot::OnCommit (hung off commit_notifier()) never sees them -- this
  // is how Copilot reaches them instead, to warm the scorer for the next
  // input. Called with the same Context and the exact decorated text about to
  // be committed.
  //
  // ORDER, corrected: this fires BEFORE the commit, not after. On master the
  // order differed by site: the two bail-outs (Enter, the number-key
  // fallback) ran CommitText -> push_back -> on_commit_; the two sites that
  // actually commit a selected candidate (Space, the number-key select) ran
  // CommitText -> on_commit_ -> NotifyForLearning -> push_back. Every call
  // site now runs on_commit_ -> CommitThroughPlugin, because the commit
  // primitive ends in ctx->Clear() and those same two callbacks read the live
  // composition that Clear() destroys. Restoring the old order is therefore
  // not available at those sites.
  //
  // What makes the new order safe TODAY, and it is luck rather than design:
  // the only `on_commit` installed is Copilot's WarmRerankContext +
  // EmitCommitTelemetry (copilot.cc), neither of which reads
  // ctx->commit_history() -- so neither can observe whether the record for
  // this commit has been pushed yet -- and WarmRerankContext takes its caret
  // text from GetCaretContext(..., kNo), i.e. rungs 1-3 only, which are
  // snapshots frozen for the duration of a key event. A future consumer that
  // wanted commit_history().back() to already name this commit would be
  // relying on the order this comment used to promise, and would be wrong.
  //
  // Default null: the standalone `auto_spacer` processor registration
  // (copilot_module.cc) has no CopilotEngine to warm anyway.
  //
  // The bool is `selection_commit`: true when the committed text reflects a
  // candidate the user actually picked (Space; the number-key select at the
  // bottom of ProcessWithSurroundingContext), false on the two bail-out paths
  // (Enter's raw commit; the number-key fallback's raw commit) where the user
  // discarded every candidate on offer and committed raw ASCII instead. On a
  // bail-out, `ctx`'s composition can still show a highlighted candidate --
  // it was never committed, so a caller that treats it as accepted (e.g.
  // telemetry) would be wrong. See Copilot::EmitCommitTelemetry (copilot.h)
  // for the one caller that reads this.
  using CommitCallback = std::function<void(Context*, const std::string&, bool)>;
  explicit AutoSpacer(const Ticket& ticket, CommitCallback on_commit = nullptr);

  ProcessResult Process(const KeyEvent& key_event);

 private:
  ProcessResult Process(Context* ctx, const KeyEvent& key_event);

  // Path 1: Process with real surrounding context (completely independent)
  ProcessResult ProcessWithSurroundingContext(Context* ctx, const KeyEvent& key_event,
                                              const SurroundingText& surrounding,
                                              const std::string& client_key);

  // Path 2: Process with commit_history (original logic)
  ProcessResult ProcessWithCommitHistory(Context* ctx, const KeyEvent& key_event,
                                         const std::string& before);

  ProcessResult HandleNumberKey(Context* ctx, const KeyEvent& key_event) const;

  struct ClientState {
    // Stores boundary when composition starts, used at commit time.
    // During composition, IMK context may reflect marked text position.
    std::string before;
    std::string after;
  };
  std::unordered_map<std::string, ClientState> client_states_;
  bool enable_right_space_ = true;
  CommitCallback on_commit_;
};

}  // namespace rime
