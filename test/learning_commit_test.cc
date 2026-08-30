// The mechanism that lets AutoSpacer's own commits reach the user dictionary.
//
// AutoSpacer commits with engine_->CommitText() and never ctx->Commit(), so
// commit_notifier_ never fires and Memory::OnCommit -- the only thing that
// writes to private.userdb -- never runs. NotifyForLearning fires the
// notifier without emitting text a second time, by borrowing the `dumb`
// option Switcher already uses for exactly that (switcher.cc:24): under it
// Context::GetCommitText() returns "", so ConcreteEngine::OnCommit's
// sink_(text) is a no-op while Memory::OnCommit -- which reads
// ctx->composition(), not GetCommitText() -- still sees everything.
//
// It deliberately does NOT call ctx->Commit() and does NOT clear the context:
// Commit() is commit_notifier_(this) followed by Clear() (context.cc:18-26),
// and Clear() fires update_notifier_ SYNCHRONOUSLY (context.cc:106-111) --
// before the caller has pushed its own decorated record onto
// commit_history(). Copilot::OnContextUpdate reads commit_history().back()
// from inside that notifier; if this function cleared, back() at that moment
// would still be whatever ConcreteEngine::OnCommit's per-segment,
// UNDECORATED push (also driven off commit_notifier_) left there, and
// Copilot::OnContextUpdate would act on the wrong text. So the caller pushes
// its own record and clears afterward -- this function's job stops at
// notifying.
//
// Driven with a hand-built Context, so no Rime engine is required. The same
// approach commit_text_test.cc takes, and for the same reason.

#include <gtest/gtest.h>

#include "plugin_commit.h"

#include <rime/candidate.h>
#include <rime/commit_history.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include <string>

using namespace rime;

namespace {

Segment MakeSelectedSegment(size_t start, size_t end, const std::string& text) {
  Segment seg(static_cast<int>(start), static_cast<int>(end));
  seg.status = Segment::kSelected;
  auto menu = New<Menu>();
  auto translation = New<FifoTranslation>();
  translation->Append(New<SimpleCandidate>("test", start, end, text));
  menu->AddTranslation(translation);
  seg.menu = menu;
  seg.selected_index = 0;
  return seg;
}

// A Context carrying one selected segment, the shape AutoSpacer commits in.
//
// Composition::Reset(raw) is the idiom commit_text_test.cc already uses;
// set_input additionally populates Context::input_, which only matters here
// because IsComposing() (and the "still composing after return" assertions)
// would be vacuous without it (Reset does not touch input_ -- context.cc:280).
void SeedComposition(Context* ctx, const std::string& input, const std::string& text) {
  ctx->set_input(input);
  Composition& comp = ctx->composition();
  comp.Reset(input);
  comp.push_back(MakeSelectedSegment(0, input.size(), text));
}

}  // namespace

TEST(NotifyForLearning, FiresTheCommitNotifierExactlyOnce) {
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  int fired = 0;
  ctx.commit_notifier().connect([&fired](Context*) { ++fired; });

  EXPECT_TRUE(NotifyForLearning(&ctx));
  EXPECT_EQ(1, fired);
}

TEST(NotifyForLearning, DumbIsSetWhileTheNotifierRuns) {
  // This is the whole point: Memory::OnCommit must run, but
  // ConcreteEngine::OnCommit's sink_(GetCommitText()) must be handed "".
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  bool dumb_inside = false;
  ctx.commit_notifier().connect(
      [&dumb_inside](Context* c) { dumb_inside = c->get_option("dumb"); });

  NotifyForLearning(&ctx);
  EXPECT_TRUE(dumb_inside);
}

TEST(NotifyForLearning, TheCompositionIsStillReadableInsideTheNotifier) {
  // Memory::ProcessSegmentOnCommit walks ctx->composition() and reads each
  // segment's selected candidate. This must not clear before notifying, or
  // learning would silently memorise nothing.
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  std::string seen;
  ctx.commit_notifier().connect([&seen](Context* c) {
    if (!c->composition().empty()) {
      if (auto cand = c->composition().back().GetSelectedCandidate()) {
        seen = cand->text();
      }
    }
  });

  NotifyForLearning(&ctx);
  EXPECT_EQ("好", seen);
}

TEST(NotifyForLearning, DumbIsRestoredToWhatItWas) {
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  NotifyForLearning(&ctx);
  EXPECT_FALSE(ctx.get_option("dumb"));
}

TEST(NotifyForLearning, DumbIsRestoredToTrueWhenItWasAlreadyTrue) {
  // Switcher sets `dumb` for its own reasons (switcher.cc:24). Hard-setting
  // false on the way out would clear a flag this function does not own.
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  ctx.set_option("dumb", true);
  NotifyForLearning(&ctx);
  EXPECT_TRUE(ctx.get_option("dumb"));
}

TEST(NotifyForLearning, DoesNotClearTheContext) {
  // The central property of this version: Commit() would end in Clear(), but
  // that fires update_notifier_ before the caller has pushed its own
  // decorated commit_history() record -- see the file header. The caller
  // owns Clear() now.
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  NotifyForLearning(&ctx);
  EXPECT_FALSE(ctx.input().empty());
  EXPECT_FALSE(ctx.composition().empty());
}

TEST(NotifyForLearning, NotifiesNothingAndReturnsFalseWhenNotComposing) {
  Context ctx;
  int fired = 0;
  ctx.commit_notifier().connect([&fired](Context*) { ++fired; });

  EXPECT_FALSE(NotifyForLearning(&ctx));
  EXPECT_EQ(0, fired);
}

TEST(NotifyForLearning, CallersPushWinsBackOverTheNotifiersOwnPush) {
  // The regression this whole fix exists for: Copilot::OnContextUpdate reads
  // commit_history().back() from inside update_notifier_, which the caller's
  // ctx->Clear() (not exercised here -- that needs a full Engine) fires AFTER
  // the caller's own push. What this function must guarantee, at the level
  // this test can reach, is that firing the notifier does not itself land
  // the last word: a notifier-side push (standing in for
  // ConcreteEngine::OnCommit's per-segment, undecorated one, also driven off
  // commit_notifier_) must not survive as back() once the caller pushes its
  // own record afterward.
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  ctx.commit_notifier().connect(
      [](Context* c) { c->commit_history().push_back({"table", "好"}); });

  NotifyForLearning(&ctx);
  ASSERT_FALSE(ctx.commit_history().empty());
  EXPECT_EQ("table", ctx.commit_history().back().type);

  // The caller's own push, exactly as both AutoSpacer call sites do it.
  ctx.commit_history().push_back({"raw", "好"});
  EXPECT_EQ("raw", ctx.commit_history().back().type);
}

TEST(NotifyForLearning, TheCommittedSegmentIsConfirmedBeforeTheNotifierRuns) {
  // ScriptTranslator::ProcessSegmentOnCommit (script_translator.cc:273-287)
  // pushes the phrase into a MEMBER queue_ and flushes it only when
  // `!recognized || seg.status >= Segment::kConfirmed`. kConfirmed is assigned
  // in exactly one place in all of librime -- ConcreteEngine::OnSelect
  // (engine.cc:264) -- reached only through select_notifier_, i.e.
  // Context::Select() / ConfirmCurrentSelection(), and AutoSpacer's commit
  // paths deliberately go through neither (the number-key site assigns
  // seg.selected_index directly to avoid Rime's select path).
  //
  // Left below kConfirmed, the phrase is never saved on its own commit: it
  // waits in the queue and is later concatenated with unrelated commits,
  // producing exactly the cross-word-boundary fragments
  // tools/rime_copilot/clean.py exists to prune -- generated into the user
  // dictionary this time rather than imported. Measured before this was
  // fixed: two single-segment Space commits (`测试`, `天气`) wrote two EMPTY
  // LevelDB WriteBatches and learned nothing at all.
  //
  // Marking it here rather than calling ConfirmCurrentSelection() is
  // deliberate: that fires select_notifier_ -> ConcreteEngine::OnSelect, which
  // also runs seg.Close() and composition().Forward(), and under
  // `_auto_commit` calls ctx->Commit() -- a second commit of text this caller
  // has already emitted itself.
  Context ctx;
  SeedComposition(&ctx, "hao", "好");
  ASSERT_EQ(Segment::kSelected, ctx.composition().back().status);

  Segment::Status seen = Segment::kVoid;
  ctx.commit_notifier().connect([&seen](Context* c) {
    if (!c->composition().empty()) {
      seen = c->composition().back().status;
    }
  });

  NotifyForLearning(&ctx);
  EXPECT_EQ(Segment::kConfirmed, seen);
}
