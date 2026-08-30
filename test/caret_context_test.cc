#include "caret_context.h"

#include <gtest/gtest.h>
#include <rime/commit_history.h>
#include <rime/context.h>

#include "imk_client.h"  // SurroundingText

namespace {

std::optional<rime::SurroundingText> g_surrounding;
std::string g_history_text;

std::optional<rime::SurroundingText> SurroundingHook() { return g_surrounding; }
std::string HistoryTextHook() { return g_history_text; }

class CaretContextTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_surrounding.reset();
    g_history_text.clear();
    rime::SetCaretContextTestHooks(&SurroundingHook, &HistoryTextHook);
  }
  void TearDown() override { rime::SetCaretContextTestHooks(nullptr, nullptr); }
};

rime::SurroundingText Real(const char* before, const char* after, rime::SurroundingSource source) {
  rime::SurroundingText s;
  s.before = before;
  s.after = after;
  s.source = source;
  return s;
}

}  // namespace

// The pass-through, not merely the absence of one wrong value: Real()'s
// source is set explicitly (not left at the struct's kNone default), so an
// implementation that hard-coded c.source on the rungs-1-3 path -- discarding
// which source actually answered -- cannot pass by accident. Task 3 branches
// on exactly `caret->source != kReconstructed` to pick which of AutoSpacer's
// two implementations runs, so this field surviving the pass-through matters
// as much as `before`/`after` do.
TEST_F(CaretContextTest, RealTextWinsAndIsNotMarkedReconstructed) {
  g_surrounding = Real("你好", "世界", rime::SurroundingSource::kTmux);
  g_history_text = "IGNORED";
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before, "你好");
  EXPECT_EQ(c->after, "世界");
  EXPECT_EQ(c->source, rime::SurroundingSource::kTmux);
}

// A second, different source: kTmux alone could be satisfied by a hard-coded
// constant just as easily as kNone could. Two distinct sources coming back
// distinct is what actually pins the pass-through.
TEST_F(CaretContextTest, RealTextPassesThroughAnyDistinctSource) {
  g_surrounding = Real("hi", "there", rime::SurroundingSource::kIMK);
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->source, rime::SurroundingSource::kIMK);
}

TEST_F(CaretContextTest, FallsBackToReconstructionWhenAllowed) {
  g_history_text = "你好";
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before, "你好");
  EXPECT_EQ(c->after, "");
  EXPECT_EQ(c->source, rime::SurroundingSource::kReconstructed);
}

// This is the whole point of the parameter: a consumer that declines a
// reconstruction must get nothing, exactly as it gets nothing today.
TEST_F(CaretContextTest, RefusesToReconstructWhenNotAllowed) {
  g_history_text = "你好";
  EXPECT_FALSE(rime::GetCaretContext(nullptr, rime::AllowReconstruction::kNo).has_value());
}

TEST_F(CaretContextTest, NothingAnywhereIsNullopt) {
  EXPECT_FALSE(rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes).has_value());
  EXPECT_FALSE(rime::GetCaretContext(nullptr, rime::AllowReconstruction::kNo).has_value());
}

// An unusable reconstruction (empty history) must not be laundered into an
// empty-but-present answer: an empty `before` is a positive claim.
TEST_F(CaretContextTest, UnusableReconstructionIsNullopt) {
  g_history_text = "";
  EXPECT_FALSE(rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes).has_value());
}

// The hook proves the CHAIN works; it proves nothing about the one call that
// reads a real Context. This is the only test that installs no history hook
// (nullptr for HistoryTextFn, surrounding hook still returns nullopt), so
// rung 4 must fall through to `ctx->commit_history().latest_text()` -- the
// line that does rung 4's real work in production -- exactly as
// commit_text_test.cc hand-builds a Context without standing up an engine.
TEST_F(CaretContextTest, ReconstructsFromRealContextCommitHistory) {
  rime::SetCaretContextTestHooks(&SurroundingHook, nullptr);
  rime::Context ctx;
  ctx.commit_history().Push(rime::CommitRecord{"raw", "你好"});

  auto c = rime::GetCaretContext(&ctx, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before, "你好");
  EXPECT_EQ(c->source, rime::SurroundingSource::kReconstructed);
}

// AutoSpacer's fallback and the chain's rung 4 must produce the same string,
// or moving the call site would silently change what the spacing rules see.
// Both sides derive it from commit_history().latest_text(); this pins that
// they still agree after the move.
TEST_F(CaretContextTest, ReconstructedBeforeEqualsTheLatestCommitText) {
  g_history_text = " hello";
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before, g_history_text);
  EXPECT_EQ(c->source, rime::SurroundingSource::kReconstructed);
}

// client_key must pass through on the real-text rungs (1-3): AutoSpacer looks
// up per-client state by it, and a caller re-deriving it with a second
// GetSurroundingContext() call can race a disconnecting client between the
// two reads. The chain answers it in one snapshot instead.
TEST_F(CaretContextTest, ClientKeyPassesThroughOnRealText) {
  rime::SurroundingText s = Real("hi", "there", rime::SurroundingSource::kBridge);
  s.client_key = "app:instance-42";
  g_surrounding = s;
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->client_key, "app:instance-42");
}

// Rung 4 has no connection to identify -- a reconstruction is not associated
// with any client -- so client_key must come back empty rather than carrying
// over whatever a previous real snapshot happened to leave in scope.
TEST_F(CaretContextTest, ClientKeyIsEmptyOnReconstruction) {
  g_history_text = "你好";
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->source, rime::SurroundingSource::kReconstructed);
  EXPECT_TRUE(c->client_key.empty());
}

// before_depth/truncation must pass through on the real-text rungs, for the
// same reason client_key does and with a sharper failure mode. Until this
// field existed, CopilotRerankFilter::Apply took the caret from one
// GetCaretContext() and these two from a SECOND GetSurroundingContext() call
// -- three independent lookups of which only tmux memoises, with
// ImeBridgeServer::GetActiveContext() re-evaluating its TTL on every call, so
// the two could be answered by different sources and the trace would record
// one source's name beside the other's depth.
TEST_F(CaretContextTest, ProvenancePassesThroughOnRealText) {
  rime::SurroundingText s = Real("你好", "", rime::SurroundingSource::kBridge);
  s.before_depth = 7;
  s.truncation = rime::Truncation::kByConfig;
  g_surrounding = s;
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before_depth, 7);
  EXPECT_EQ(c->truncation, rime::Truncation::kByConfig);
}

// Rung 4 must claim nothing it cannot know. A reconstruction sees only the
// last commit record, so it has no honest depth and no way to say why the
// text stopped -- kUnknown is "the source cannot say" (imk_client.h), and -1
// is that same "cannot say" for before_depth (rerank_trace.h,
// telemetry_event.h); 0 would instead be the depth of a string it never
// measured.
TEST_F(CaretContextTest, ProvenanceIsUnclaimedOnReconstruction) {
  g_history_text = "你好";
  auto c = rime::GetCaretContext(nullptr, rime::AllowReconstruction::kYes);
  ASSERT_TRUE(c.has_value());
  EXPECT_EQ(c->before_depth, -1);
  EXPECT_EQ(c->truncation, rime::Truncation::kUnknown);
}
