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

// Fire Context::commit_notifier_ for a commit AutoSpacer has already emitted
// itself, so Rime's user dictionary learns from it -- WITHOUT clearing the
// context. The caller clears.
//
// AutoSpacer commits with engine_->CommitText() + ctx->Clear() and never
// ctx->Commit(), so commit_notifier_ never fires -- and that notifier is the
// only route to Memory::OnCommit, which is the only thing that writes to
// private.userdb. Measured consequence: 220 bytes and /tick 0 after a week of
// daily use.
//
// The text must not be emitted twice, and `dumb` is how Rime already says
// "notify, but do not commit anything" -- Switcher sets it for the same
// reason (switcher.cc:24). Under it Context::GetCommitText() returns "", so
// ConcreteEngine::OnCommit's sink_(text) appends nothing and
// Session::HasCommit() stays false (service.cc:56, :42), while
// Memory::OnCommit reads ctx->composition() and is unaffected.
//
// The previous value of `dumb` is restored rather than hard-set to false:
// this function does not own that flag.
//
// This does NOT call ctx->Commit(): that is commit_notifier_(this) followed
// by Clear() (context.cc:18-26), and Clear() fires update_notifier_
// SYNCHRONOUSLY (context.cc:106-111) -- before this function, or the caller,
// has pushed the caller's own decorated record onto commit_history(). A
// consumer of update_notifier_ that reads commit_history().back() at that
// instant (Copilot::OnContextUpdate does) would see whatever
// ConcreteEngine::OnCommit's per-segment push left there instead, and act on
// the wrong, undecorated text. So this fires commit_notifier_ directly and
// leaves Clear() to the caller, to run after the caller's own push. See each
// call site for the exact ordering this requires.
//
// PRECONDITION, and the one a new caller is most likely to miss: call this
// only where the user actually chose the candidate being committed -- where
// `on_commit_`'s `selection_commit` is true. Memory memorises
// seg.GetSelectedCandidate(), the still-highlighted one, so on a bail-out
// (Enter's raw commit, the number-key fallback) this would memorise the
// candidate the user just rejected. Language::intelligible does not save you:
// that candidate is usually a perfectly legitimate Han phrase.
//
// Marks the last segment kConfirmed before notifying -- see the definition for
// why the notification is inert without it. Returns whether it notified
// (false when the context was not composing); both current callers ignore the
// result, since they clear unconditionally either way.
//
// Declared here rather than kept file-local so it can be unit-tested with a
// hand-built Context, without standing up a full Rime engine.
bool NotifyForLearning(Context* ctx);

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
  // input. Called with the same Context and the exact decorated text just
  // handed to engine_->CommitText(), before ctx->Clear() -- see each call
  // site below. Default null: the standalone `auto_spacer` processor
  // registration (copilot_module.cc) has no CopilotEngine to warm anyway.
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
  ProcessResult ProcessWithCommitHistory(Context* ctx, const KeyEvent& key_event);

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
