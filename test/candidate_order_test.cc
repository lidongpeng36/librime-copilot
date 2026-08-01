// Unit tests for the provider merge / display order (copilot_engine.h).
//
// Weight semantics across the whole pipeline are "higher = more likely":
// make_copilot_data drops entries *below* --filter-weight and emits them
// weight-descending, and DBProvider keeps the top-N by weight. The merged list
// feeds a FifoTranslation, so its order IS the order the user sees.

// gtest first: the plugin headers pull in Rime's X11-style key table, whose
// `Bool` macro would otherwise clobber gtest's Bool() generator.
#include <gtest/gtest.h>

#include "copilot_engine.h"

#include <string>
#include <vector>

using copilot::Entry;
using copilot::ProviderType;
using rime::MergeProviderCandidates;
using rime::RankedCandidates;

namespace {

std::vector<std::string> Texts(const std::vector<Entry>& entries) {
  std::vector<std::string> out;
  for (const auto& e : entries) out.push_back(e.text);
  return out;
}

Entry Db(const std::string& text, double weight) { return {text, weight, ProviderType::kDB}; }
Entry Llm(const std::string& text) { return {text, 4.0, ProviderType::kLLM}; }

}  // namespace

TEST(CandidateOrder, UnrankedProviderIsMostLikelyFirst) {
  // Regression: the merge sorted ascending, so the least likely prediction of
  // the kept top-N was offered as candidate #1.
  auto merged = MergeProviderCandidates({
      {-1, {Db("低", 1.0), Db("高", 100.0), Db("中", 50.0)}},
  });

  EXPECT_EQ(std::vector<std::string>({"高", "中", "低"}), Texts(merged));
}

TEST(CandidateOrder, SeveralUnrankedProvidersMergeByWeight) {
  auto merged = MergeProviderCandidates({
      {-1, {Db("a", 10.0), Db("c", 1.0)}},
      {-1, {Db("b", 5.0)}},
  });

  EXPECT_EQ(std::vector<std::string>({"a", "b", "c"}), Texts(merged));
}

TEST(CandidateOrder, RankedProviderIsPinnedToItsIndex) {
  // rank is a 0-based display position (LLMProvider turns the 1-based config
  // value into it), so the LLM candidate lands at index 2 regardless of weight.
  auto merged = MergeProviderCandidates({
      {-1, {Db("a", 10.0), Db("b", 9.0), Db("c", 8.0), Db("d", 7.0)}},
      {2, {Llm("𝓛")}},
  });

  EXPECT_EQ(std::vector<std::string>({"a", "b", "𝓛", "c", "d"}), Texts(merged));
}

TEST(CandidateOrder, RankZeroPinsToTheFront) {
  // rank 0 (config `rank: 1`) means "first candidate" — it must not fall back
  // into the weight-sorted pool.
  auto merged = MergeProviderCandidates({
      {-1, {Db("a", 10.0), Db("b", 9.0)}},
      {0, {Llm("𝓛")}},
  });

  EXPECT_EQ(std::vector<std::string>({"𝓛", "a", "b"}), Texts(merged));
}

TEST(CandidateOrder, RankBeyondTheEndAppends) {
  auto merged = MergeProviderCandidates({
      {-1, {Db("a", 10.0)}},
      {9, {Llm("𝓛")}},
  });

  EXPECT_EQ(std::vector<std::string>({"a", "𝓛"}), Texts(merged));
}

TEST(CandidateOrder, EmptyInputYieldsEmptyOutput) {
  EXPECT_TRUE(MergeProviderCandidates({}).empty());
  EXPECT_TRUE(MergeProviderCandidates({{-1, {}}, {3, {}}}).empty());
}
