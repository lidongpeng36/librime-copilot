// Unit tests for the contextual re-ranking decision (rerank.h).
//
// Two pure steps: turn the raw text before the caret into a usable n-gram
// context, then decide which candidate (if any) to promote. No db, no Rime
// engine, no IMK client.

#include <gtest/gtest.h>

#include "rerank.h"

#include <string>
#include <vector>

using copilot::Entry;
using copilot::ProviderType;
using rime::PickPromotion;
using rime::TrailingCjkRun;

namespace {

Entry E(const std::string& text, double freq) { return {text, freq, ProviderType::kDB}; }

constexpr int kMax = 8;
constexpr int kMaxRank = 50;

}  // namespace

// ---------------------------------------------------------------- context --

TEST(TrailingCjk, KeepsTheRunTouchingTheCaret) {
  EXPECT_EQ("高屋建", TrailingCjkRun("高屋建", kMax));
  EXPECT_EQ("高屋建", TrailingCjkRun("今天很好。高屋建", kMax));
  EXPECT_EQ("高屋建", TrailingCjkRun("see 高屋建", kMax));
  EXPECT_EQ("瓴", TrailingCjkRun("高屋建，瓴", kMax));
}

TEST(TrailingCjk, AnythingButAHanCharacterEndsTheContext) {
  // Whatever sits right before the caret decides it: the run must be adjacent.
  EXPECT_EQ("", TrailingCjkRun("高屋建。", kMax));   // CJK punctuation
  EXPECT_EQ("", TrailingCjkRun("高屋建.", kMax));    // ASCII punctuation
  EXPECT_EQ("", TrailingCjkRun("高屋建 ", kMax));    // space
  EXPECT_EQ("", TrailingCjkRun("高屋建\n", kMax));   // newline
  EXPECT_EQ("", TrailingCjkRun("高屋建１", kMax));   // fullwidth digit
  EXPECT_EQ("", TrailingCjkRun("高屋建🙂", kMax));  // emoji
  EXPECT_EQ("", TrailingCjkRun("高屋建a", kMax));    // latin letter
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
