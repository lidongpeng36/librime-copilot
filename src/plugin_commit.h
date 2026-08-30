#pragma once

// One commit, as one operation.
//
// The plugin commits by composing three librime operations that librime never
// designed as a unit: CommitText, the learning notification, and Clear(). Each
// appends to commit_history, and the third synchronously wakes a reader of
// .back(). Before this existed, that sequence -- and the re-assert that
// compensates for NotifyForLearning burying .back() -- was rewritten at nine
// call sites and enforced only by a paragraph in CLAUDE.md.
//
// The order is expressed against CommitSteps, not against Rime, because the
// invariant cannot be tested through a real engine: no test in this tree can
// stand one up.

#include <rime/processor.h>

#include <string>

namespace rime {

class Context;
class Engine;

// There is deliberately no CommitKind parameter. It was threaded through this
// function and read by nothing, and it cannot be made to mean anything here:
//
//   - The record for a commit is pushed by librime itself, inside
//     ConcreteEngine::CommitText (engine.cc:244), hard-coded {"raw", text}.
//     The only way to make a commit's record say "punct" or "thru" instead is
//     to push a SECOND record after it -- two history entries for one commit,
//     which is exactly what WithoutLearningThereIsNoReassert pins as wrong.
//   - "raw", "punct" and "thru" are librime's own vocabulary
//     (commit_history.h, commit_history.cc:47, candidate types), so a kind
//     could not have introduced a fourth value either.
//   - The two readers outside AutoSpacer do not distinguish them:
//     copilot.cc:590 treats punct/raw/thru as one set and filters.cc:63 asks
//     only whether the type is "thru".
//
// The one caller that used to forge a non-"raw" record is AutoSpacer's
// punctuation branch, which now records "raw" like everything else.
//
// An earlier version of this comment concluded from the two readers above
// that "nothing distinguishes them downstream", full stop. That was wrong,
// and the retraction is kept because the false version is the kind of claim
// someone widens code on the strength of: NeedAddSpace (auto_spacer.cc) is a
// third reader, and it branches on `type == "raw" || type == "thru"` -- a
// block the forged {"punct", ...} record used to skip and the {"raw", ...}
// one now enters. What keeps the change invisible is the block's second
// condition, IsAlphabetKey(LastAsciiCharCode(latest_text)): no
// punctuator/full_shape or punctuator/symbols value ends in an ASCII letter
// or digit, so the block still decides nothing. The site's own comment
// carries the reachability argument.

// Fire Context::commit_notifier_ for a commit the plugin has already emitted
// itself, so Rime's user dictionary learns from it -- WITHOUT clearing the
// context. RunCommitSequence, below, clears.
//
// It lives here rather than in auto_spacer.h (where it was declared until this
// was written) because it has no caller in that file any more: it is a step of
// the commit sequence, and RunCommitSequence is the only thing that runs it.
// Left there, plugin_commit.cc had to include the specific sub-plugin's header
// to reach a function the generic primitive owns.
//
// The plugin commits with engine_->CommitText() + ctx->Clear() and never
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
// SYNCHRONOUSLY (context.cc:106-111) -- before the decorated record has been
// pushed onto commit_history(). A consumer of update_notifier_ that reads
// commit_history().back() at that instant (Copilot::OnContextUpdate does)
// would see whatever ConcreteEngine::OnCommit's per-segment push left there
// instead, and act on the wrong, undecorated text. So this fires
// commit_notifier_ directly and leaves Clear() to RunCommitSequence, which
// runs it after CommitSteps::PushRecord.
//
// PRECONDITION: notify only where the user actually chose the candidate being
// committed. Memory memorises seg.GetSelectedCandidate(), the
// still-highlighted one, so on a bail-out (Enter's raw commit, the number-key
// fallback) this would memorise the candidate the user just rejected;
// Language::intelligible does not save you, since that candidate is usually a
// perfectly legitimate Han phrase. Enforcing it is no longer this function's
// caller's problem one site at a time: it is RunCommitSequence's `learn`
// parameter, which AutoSpacer passes true at exactly the two sites where
// CommitCallback's `selection_commit` is also true (Space, the number-key
// select) and false at the two bail-outs.
//
// Marks the last segment kConfirmed before notifying -- see the definition for
// why the notification is inert without it. Returns whether it notified (false
// when the context was not composing); RunCommitSequence, its one caller,
// ignores the result and clears either way.
//
// Declared here rather than kept file-local so it can be unit-tested with a
// hand-built Context, without standing up a full Rime engine
// (test/learning_commit_test.cc).
bool NotifyForLearning(Context* ctx);

struct CommitSteps {
  virtual ~CommitSteps() = default;
  // librime pushes {"raw", text} itself inside CommitText (engine.cc:244-246).
  virtual void CommitText(const std::string& text) = 0;
  // Reaches librime's OnCommit, which appends per-segment records and so
  // buries whatever was at .back().
  virtual void NotifyForLearning() = 0;
  // Re-asserts .back() after the above. Needed ONLY when learning ran.
  virtual void PushRecord(const std::string& text) = 0;
  // Fires update_notifier_ synchronously; its reader takes .back().text.
  virtual void Clear() = 0;
};

void RunCommitSequence(CommitSteps& steps, const std::string& text, bool learn);

// The Rime binding: builds the CommitSteps that talk to engine/ctx and runs
// the sequence above. Returns kAccepted when anything was committed.
ProcessResult CommitThroughPlugin(Engine* engine, Context* ctx, const std::string& text,
                                  bool learn);

}  // namespace rime
