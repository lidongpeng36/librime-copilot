// Unit tests for the prediction-context helpers (prediction_context.h).
//
// These turn the caret's surrounding text into the n-gram lookup keys the
// copilot db is built around (tools/make_copilot_data emits word-prefix keys),
// so they are pure string logic — no IMK client, no db, no Rime engine.

#include "prediction_context.h"

#include <gtest/gtest.h>

#include <string>
#include <vector>

using rime::BuildLookupKeys;
using rime::BuildPredictionContext;

TEST(PredictionContext, AppendsTheTextJustCommitted) {
  // The IMK snapshot is taken before the key is handled, so the word committed
  // by this very key event is not in it yet.
  EXPECT_EQ("今天天气", BuildPredictionContext("今天", "天气"));
  EXPECT_EQ("天气", BuildPredictionContext("", "天气"));
  EXPECT_EQ("今天", BuildPredictionContext("今天", ""));
  EXPECT_EQ("", BuildPredictionContext("", ""));
}

TEST(PredictionContext, DoesNotDuplicateAnAlreadyPresentCommit) {
  // Some clients update their marked/selected range early enough that the
  // snapshot already ends with the committed text; appending it again would
  // fabricate "天气天气" as the lookup context.
  EXPECT_EQ("今天天气", BuildPredictionContext("今天天气", "天气"));
  EXPECT_EQ("天气", BuildPredictionContext("天气", "天气"));
  // Only a real suffix counts.
  EXPECT_EQ("天气很好", BuildPredictionContext("天气", "很好"));
}

TEST(LookupKeys, ShortestFirstUpToTheContextLength) {
  // Keys are the last 1, 2, ... characters before the caret — the same shape
  // DBProvider derives from History today (back(), then get_chars(2..n)).
  EXPECT_EQ(std::vector<std::string>({"气", "天气", "今天气"}),
            BuildLookupKeys("今天气", /*max_hints=*/0));
}

TEST(LookupKeys, RespectsMaxHints) {
  EXPECT_EQ(std::vector<std::string>({"好", "很好"}), BuildLookupKeys("天气很好", /*max_hints=*/2));
  EXPECT_EQ(std::vector<std::string>({"好"}), BuildLookupKeys("天气很好", /*max_hints=*/1));
  // A cap longer than the context is just the context.
  EXPECT_EQ(std::vector<std::string>({"好", "很好"}), BuildLookupKeys("很好", /*max_hints=*/99));
}

TEST(LookupKeys, SlicesOnUtf8CharacterBoundaries) {
  // Mixed-width input must never be cut mid-character.
  EXPECT_EQ(std::vector<std::string>({"文", "a文", "中a文"}),
            BuildLookupKeys("中a文", /*max_hints=*/0));
}

TEST(LookupKeys, EmptyContextYieldsNoKeys) {
  EXPECT_TRUE(BuildLookupKeys("", /*max_hints=*/0).empty());
  EXPECT_TRUE(BuildLookupKeys("", /*max_hints=*/5).empty());
}

TEST(LookupKeys, NonPositiveMaxHintsMeansUnlimited) {
  // DBProvider::Config treats <= 0 as "no limit"; keep the same reading here.
  EXPECT_EQ(3u, BuildLookupKeys("今天气", /*max_hints=*/0).size());
  EXPECT_EQ(3u, BuildLookupKeys("今天气", /*max_hints=*/-1).size());
}
