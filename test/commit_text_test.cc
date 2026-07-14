// Processor-level regression tests for the Space-commit behavior.
//
// These guard the bug where a long input that was split into multiple segments
// committed only the LAST selected candidate (dropping the earlier ones). They
// drive ComputeSpaceCommitText — the exact decision the AutoSpacer Space
// handler makes — with a hand-built multi-segment Context, so no full Rime
// engine / IMK session is required.

#include <gtest/gtest.h>

#include "auto_spacer.h"

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include <string>
#include <vector>

using namespace rime;

namespace {

// Build one already-selected segment carrying a single candidate.
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

// Populate ctx with a composition of the given selected candidate texts, as if
// the user had picked a candidate for each segment of a long input.
void SetupSelectedComposition(Context* ctx, const std::vector<std::string>& texts) {
  Composition& comp = ctx->composition();
  comp.Reset(std::string(2 * texts.size(), 'x'));  // placeholder raw input
  for (size_t i = 0; i < texts.size(); ++i) {
    comp.push_back(MakeSelectedSegment(2 * i, 2 * (i + 1), texts[i]));
  }
}

}  // namespace

TEST(SpaceCommit, MultiSegmentCommitsWholeComposition) {
  Context ctx;
  SetupSelectedComposition(&ctx, {"我", "是"});

  // Sanity: the last segment alone (the old buggy source) is just "是".
  ASSERT_EQ("是", ctx.composition().back().GetSelectedCandidate()->text());

  std::string committed = ComputeSpaceCommitText(&ctx, "", "", /*enable_right_space=*/true);

  EXPECT_EQ("我是", committed);  // all segments, not just the last
  EXPECT_NE("是", committed);    // regression guard for the dropped-segments bug
}

TEST(SpaceCommit, ThreeSegments) {
  Context ctx;
  SetupSelectedComposition(&ctx, {"我", "爱", "你"});

  EXPECT_EQ("我爱你", ComputeSpaceCommitText(&ctx, "", "", true));
}

TEST(SpaceCommit, WholeCompositionIsSpacedAgainstSurrounding) {
  Context ctx;
  SetupSelectedComposition(&ctx, {"我", "是"});

  // English before the boundary -> a leading space is added to the FULL text.
  EXPECT_EQ(" 我是", ComputeSpaceCommitText(&ctx, "abc", "", true));
  // English after -> trailing space when enabled...
  EXPECT_EQ("我是 ", ComputeSpaceCommitText(&ctx, "", "def", true));
  // ...and not when disabled.
  EXPECT_EQ("我是", ComputeSpaceCommitText(&ctx, "", "def", false));
}

TEST(SpaceCommit, SingleSegmentUnaffected) {
  Context ctx;
  SetupSelectedComposition(&ctx, {"你好"});

  EXPECT_EQ("你好", ComputeSpaceCommitText(&ctx, "", "", true));
}

TEST(SpaceCommit, NoCandidateFallsBackToRawInput) {
  // Empty composition (e.g. composing raw ASCII): commit ctx->input() instead,
  // treated as ASCII content for spacing.
  Context ctx;
  ctx.set_input("hello");

  EXPECT_EQ("hello", ComputeSpaceCommitText(&ctx, "", "", true));
  // ASCII committed after Chinese gets a leading space.
  EXPECT_EQ(" hello", ComputeSpaceCommitText(&ctx, "中", "", true));
}

TEST(SpaceCommit, EnglishCandidateInMiddleGetsBothSpaces) {
  // Selecting an English candidate `test` between 你 and 好 must be spaced on
  // BOTH sides — identical to committing raw English via Enter (你 test 好).
  // The candidate text is ASCII, so ASCII-content spacing rules must apply even
  // though a candidate is selected.
  Context ctx;
  SetupSelectedComposition(&ctx, {"test"});

  EXPECT_EQ(" test ", ComputeSpaceCommitText(&ctx, "你", "好", true));
}

TEST(SpaceCommit, CjkCandidateInMiddleNotSpaced) {
  // A CJK candidate between CJK is NOT spaced (regression guard for the fix).
  Context ctx;
  SetupSelectedComposition(&ctx, {"世界"});

  EXPECT_EQ("世界", ComputeSpaceCommitText(&ctx, "你", "好", true));
}
