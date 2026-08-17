// Unit tests for the contextual re-ranking decision (rerank.h).
//
// Two pure steps: turn the raw text before the caret into a usable n-gram
// context, then decide which candidate (if any) to promote. No db, no Rime
// engine, no IMK client.

#include <gtest/gtest.h>

#include "rerank.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include <string>
#include <vector>

using copilot::Entry;
using copilot::ProviderType;
using rime::Composition;
using rime::ConfirmedPrefix;
using rime::PickPromotion;
using rime::Segment;
using rime::TrailingCjkRun;

namespace {

Entry E(const std::string& text, double freq) { return {text, freq, ProviderType::kDB}; }

constexpr int kMax = 8;
constexpr int kMaxRank = 50;

// Same builders as test/commit_text_test.cc (see its header comment) —
// duplicated locally rather than shared because both files build them as
// small, self-contained anonymous-namespace helpers.

// One already-selected segment carrying a single candidate.
Segment MakeSelectedSegment(size_t start, size_t end, const std::string& text) {
  Segment seg(static_cast<int>(start), static_cast<int>(end));
  seg.status = Segment::kSelected;
  auto menu = rime::New<rime::Menu>();
  auto translation = rime::New<rime::FifoTranslation>();
  translation->Append(rime::New<rime::SimpleCandidate>("test", start, end, text));
  menu->AddTranslation(translation);
  seg.menu = menu;
  seg.selected_index = 0;
  return seg;
}

// A selected segment spanning [start, seg_end) whose candidate only converts
// [start, cand_end) — Rime's "typed yyuu, highlighted 云 (just yy)".
Segment MakeSelectedPartialSegment(size_t start, size_t seg_end, size_t cand_end,
                                   const std::string& text) {
  Segment seg(static_cast<int>(start), static_cast<int>(seg_end));
  seg.status = Segment::kSelected;
  auto menu = rime::New<rime::Menu>();
  auto translation = rime::New<rime::FifoTranslation>();
  translation->Append(rime::New<rime::SimpleCandidate>("test", start, cand_end, text));
  menu->AddTranslation(translation);
  seg.menu = menu;
  seg.selected_index = 0;
  return seg;
}

// A not-yet-selected segment (the shape of `current`, or of something sitting
// unselected ahead of it).
Segment MakeUnselectedSegment(size_t start, size_t end) {
  return Segment(static_cast<int>(start), static_cast<int>(end));
}

}  // namespace

// ------------------------------------------------------------ ConfirmedPrefix --

TEST(ConfirmedPrefix, EmptyCompositionIsEmptyPrefix) {
  Composition comp;
  Segment elsewhere = MakeUnselectedSegment(0, 1);  // not in `comp`
  EXPECT_EQ("", ConfirmedPrefix(comp, &elsewhere));
}

TEST(ConfirmedPrefix, CurrentIsTheOnlySegmentAndUnselected) {
  // The current segment never contributes its own text, selected or not: the
  // walk stops the instant it reaches `current`.
  Composition comp;
  comp.push_back(MakeUnselectedSegment(0, 2));
  EXPECT_EQ("", ConfirmedPrefix(comp, &comp.back()));
}

TEST(ConfirmedPrefix, TwoSelectedSegmentsBeforeCurrentAreConcatenatedInOrder) {
  Composition comp;
  comp.push_back(MakeSelectedSegment(0, 2, "这个"));
  comp.push_back(MakeSelectedSegment(2, 4, "顺序"));
  comp.push_back(MakeUnselectedSegment(4, 6));  // current: still being composed
  EXPECT_EQ("这个顺序", ConfirmedPrefix(comp, &comp.back()));
}

TEST(ConfirmedPrefix, StopsAtAnUnselectedSegmentRatherThanSkippingIt) {
  // selected, then UNSELECTED, then current: an unselected segment before
  // `current` means anything after it isn't actually confirmed, so the walk
  // must not skip over it to reach segments beyond.
  Composition comp;
  comp.push_back(MakeSelectedSegment(0, 2, "这个"));
  comp.push_back(MakeUnselectedSegment(2, 4));
  comp.push_back(MakeSelectedSegment(4, 6, "故意"));  // would-be "current"
  EXPECT_EQ("这个", ConfirmedPrefix(comp, &comp.back()));
}

TEST(ConfirmedPrefix, PartialCandidateContributesItsOwnTextNotTheInputSubstring) {
  // Typed "yyuu", picked 云 (spanning only "yy" of the segment's [0,4)): the
  // prefix must be the candidate's text, not a substring of the segment's
  // input range.
  Composition comp;
  comp.push_back(MakeSelectedPartialSegment(0, 4, 2, "云"));
  comp.push_back(MakeUnselectedSegment(4, 6));  // current
  EXPECT_EQ("云", ConfirmedPrefix(comp, &comp.back()));
}

TEST(ConfirmedPrefix, NullCurrentYieldsEmptyPrefix) {
  // No known "current" segment means the walk has nothing to safely stop at —
  // guessing which prefix of the composition is "before" an unidentified
  // segment would risk fabricating context, so this returns no prefix at all
  // rather than the whole composition. Mirrors the "drop rather than
  // misattribute" rule already used for pending_trace_span_ in
  // rerank_filter.cc.
  Composition comp;
  comp.push_back(MakeSelectedSegment(0, 2, "这个"));
  comp.push_back(MakeSelectedSegment(2, 4, "顺序"));
  EXPECT_EQ("", ConfirmedPrefix(comp, nullptr));
}

// ---------------------------------------------------------------- context --

TEST(TrailingCjk, KeepsTheRunTouchingTheCaret) {
  EXPECT_EQ("高屋建", TrailingCjkRun("高屋建", kMax));
  EXPECT_EQ("高屋建", TrailingCjkRun("今天很好。高屋建", kMax));
  EXPECT_EQ("高屋建", TrailingCjkRun("see 高屋建", kMax));
  EXPECT_EQ("瓴", TrailingCjkRun("高屋建，瓴", kMax));
}

TEST(TrailingCjk, AnythingButAHanCharacterEndsTheContext) {
  // Whatever sits right before the caret decides it: the run must be adjacent.
  EXPECT_EQ("", TrailingCjkRun("高屋建。", kMax));  // CJK punctuation
  EXPECT_EQ("", TrailingCjkRun("高屋建.", kMax));   // ASCII punctuation
  EXPECT_EQ("", TrailingCjkRun("高屋建 ", kMax));   // space
  EXPECT_EQ("", TrailingCjkRun("高屋建\n", kMax));  // newline
  EXPECT_EQ("", TrailingCjkRun("高屋建１", kMax));  // fullwidth digit
  EXPECT_EQ("", TrailingCjkRun("高屋建🙂", kMax));  // emoji
  EXPECT_EQ("", TrailingCjkRun("高屋建a", kMax));   // latin letter
}

TEST(TrailingCjk, HonorsTheCharacterCap) {
  EXPECT_EQ("势很足", TrailingCjkRun("高屋建瓴的气势很足", 3));
  EXPECT_EQ("足", TrailingCjkRun("高屋建瓴的气势很足", 1));
  // A cap larger than the run is just the run.
  EXPECT_EQ("高屋建", TrailingCjkRun("高屋建", 99));
}

TEST(TrailingCjk, DegenerateInput) {
  EXPECT_EQ("", TrailingCjkRun("", kMax));
  EXPECT_EQ("", TrailingCjkRun("abc", kMax));
  EXPECT_EQ("", TrailingCjkRun("123", kMax));
  EXPECT_EQ("", TrailingCjkRun("。，！", kMax));
  EXPECT_EQ("", TrailingCjkRun("高屋建", 0));  // non-positive cap
}

// -------------------------------------------------------------- promotion --

TEST(Promotion, ExactMatchIsPromoted) {
  // 高屋建 -> 瓴, and 瓴 is sitting at the back of the candidate list.
  auto p = PickPromotion({"令", "领", "瓴"}, {E("瓴", 100.0)}, kMaxRank);
  EXPECT_EQ(2, p.index);
  EXPECT_EQ(1, p.rank);
}

TEST(Promotion, CandidateMayBeTheStartOfTheContinuation) {
  // 高屋 -> 建瓴 while the user has only typed the syllable for 建.
  auto p = PickPromotion({"见", "建"}, {E("建瓴", 100.0)}, kMaxRank);
  EXPECT_EQ(1, p.index);
}

TEST(Promotion, ContinuationMayBeTheStartOfTheCandidate) {
  // The db knows 建, the candidate list offers the longer 建瓴之势.
  auto p = PickPromotion({"见", "建瓴之势"}, {E("建", 100.0)}, kMaxRank);
  EXPECT_EQ(1, p.index);
}

TEST(Promotion, ExactBeatsAMoreLikelyPrefixMatch) {
  // 令 starts the far more frequent 令牌, but 瓴 matches exactly — match
  // quality outranks raw likelihood.
  auto p = PickPromotion({"令", "瓴"}, {E("令牌", 70.0), E("瓴", 30.0)}, kMaxRank);
  EXPECT_EQ(1, p.index);
}

TEST(Promotion, WithinTheSameLevelTheLikelierWins) {
  auto p = PickPromotion({"建议", "建瓴"}, {E("建议", 90.0), E("建瓴", 10.0)}, kMaxRank);
  EXPECT_EQ(0, p.index);
  EXPECT_EQ(1, p.rank);
}

TEST(Promotion, RankedTooLowNothingMoves) {
  // Guard against noise: a one-character key has thousands of continuations,
  // and lifting a long-tail one would be worse than leaving the order alone.
  std::vector<Entry> many;
  for (int i = 0; i < 60; ++i) many.push_back(E("其他" + std::to_string(i), 1000.0 + i));
  many.push_back(E("瓴", 1.0));  // dead last
  EXPECT_EQ(-1, PickPromotion({"令", "瓴"}, many, /*max_rank=*/10).index);
  // Same data, a lenient cap: now it promotes and reports where it sat.
  auto p = PickPromotion({"令", "瓴"}, many, /*max_rank=*/100);
  EXPECT_EQ(1, p.index);
  EXPECT_EQ(61, p.rank);
}

TEST(Promotion, NoMatchLeavesTheOrderAlone) {
  EXPECT_EQ(-1, PickPromotion({"令", "领"}, {E("瓴", 100.0)}, kMaxRank).index);
}

TEST(Promotion, DegenerateInput) {
  EXPECT_EQ(-1, PickPromotion({}, {E("瓴", 100.0)}, kMaxRank).index);
  EXPECT_EQ(-1, PickPromotion({"瓴"}, {}, kMaxRank).index);
  EXPECT_EQ(-1, PickPromotion({""}, {E("瓴", 100.0)}, kMaxRank).index);
  // Entries with no usable mass cannot promote anything.
  EXPECT_EQ(-1, PickPromotion({"瓴"}, {E("瓴", 0.0)}, kMaxRank).index);
}

TEST(Promotion, AlreadyFirstIsStillReported) {
  // The caller decides that index 0 needs no move; the pure decision stands.
  auto p = PickPromotion({"瓴", "令"}, {E("瓴", 100.0)}, kMaxRank);
  EXPECT_EQ(0, p.index);
}

TEST(Promotion, RankCountsTiesAsTheSamePosition) {
  // Merged dictionaries produce ties; a tie must not push the winner past the
  // cap by counting equal weights as better.
  auto p = PickPromotion({"瓴"}, {E("甲", 100.0), E("乙", 100.0), E("瓴", 100.0)}, 1);
  EXPECT_EQ(0, p.index);
  EXPECT_EQ(1, p.rank);
}

// The match level is what distinguishes an exact single-character hit (the
// shape 了/吗 always take) from a partial one. The telemetry cannot test the
// "level 3 dominates" hypothesis unless PickPromotion reports it.
TEST(PickPromotion, ReportsAnExactMatchLevel) {
  std::vector<std::string> texts = {"建"};
  std::vector<::copilot::Entry> conts = {
      {"建", 100.0, ::copilot::ProviderType::kDB},
  };
  auto p = rime::PickPromotion(texts, conts, 50);
  ASSERT_EQ(p.index, 0);
  EXPECT_EQ(p.level, 3);
}

TEST(PickPromotion, ReportsACandidateStartingAContinuation) {
  std::vector<std::string> texts = {"建"};
  std::vector<::copilot::Entry> conts = {
      {"建瓴", 100.0, ::copilot::ProviderType::kDB},
  };
  auto p = rime::PickPromotion(texts, conts, 50);
  ASSERT_EQ(p.index, 0);
  EXPECT_EQ(p.level, 2);
}

TEST(PickPromotion, ReportsAContinuationStartingACandidate) {
  std::vector<std::string> texts = {"建瓴之势"};
  std::vector<::copilot::Entry> conts = {
      {"建", 100.0, ::copilot::ProviderType::kDB},
  };
  auto p = rime::PickPromotion(texts, conts, 50);
  ASSERT_EQ(p.index, 0);
  EXPECT_EQ(p.level, 1);
}

TEST(PickPromotion, ReportsNoLevelWhenNothingMatches) {
  std::vector<std::string> texts = {"无"};
  std::vector<::copilot::Entry> conts = {
      {"建", 100.0, ::copilot::ProviderType::kDB},
  };
  auto p = rime::PickPromotion(texts, conts, 50);
  EXPECT_EQ(p.index, -1);
  EXPECT_EQ(p.level, 0);
}
