#include <gtest/gtest.h>

#include "wordhead_table.h"

#include <cmath>
#include <sstream>

using rime::wordhead::Load;
using rime::wordhead::Parse;
using rime::wordhead::ParseResult;
using rime::wordhead::PriorOf;
using rime::wordhead::Table;
using rime::wordhead::WindowPriors;

namespace {

Table TableOf(const std::string& text) {
  std::istringstream in(text);
  return Parse(in).table;
}

}  // namespace

// The C++ half of the two-hop pin: tools/test/wordhead_test.py generates this
// exact file from a hand-written corpus; here it must parse to those values.
TEST(WordHeadTable, ParsesTheGoldenThePythonGeneratorWrites) {
  ParseResult r;
  std::string error;
  ASSERT_TRUE(Load(std::string(COPILOT_TEST_DATA_DIR) + "/wordhead_golden.txt", &r, &error))
      << error;
  EXPECT_EQ(r.bad_lines, 0);
  ASSERT_EQ(r.table.mass.size(), 4u);
  EXPECT_FLOAT_EQ(r.table.mass.at("先"), 0.3333f);
  EXPECT_FLOAT_EQ(r.table.mass.at("发"), 0.6000f);
  EXPECT_FLOAT_EQ(r.table.mass.at("现"), 0.6667f);
  EXPECT_FLOAT_EQ(r.table.mass.at("走"), 0.4000f);
}

TEST(WordHeadTable, AMissingFileIsAnErrorNamingThePath) {
  ParseResult r;
  std::string error;
  EXPECT_FALSE(Load("/nonexistent/wordhead.txt", &r, &error));
  EXPECT_NE(error.find("/nonexistent/wordhead.txt"), std::string::npos);
}

TEST(WordHeadTable, BadLinesAreCountedAndDroppedNeverGuessed) {
  std::istringstream in(
      "# comment\n"
      "\n"
      "先\t0.3\n"
      "先生\t0.5\n"     // two characters
      "a\t0.5\n"        // not Han
      "现 0.9\n"        // no tab
      "安\tabc\n"       // not a number
      "爱\t1.5\n"       // out of range
      "啊\t-0.1\n"      // out of range
      "先\t0.9\n"       // duplicate: first value wins
      "按\t0.2\r\n");   // CRLF tolerated
  const ParseResult r = Parse(in);
  EXPECT_EQ(r.bad_lines, 7);
  ASSERT_EQ(r.table.mass.size(), 2u);
  EXPECT_FLOAT_EQ(r.table.mass.at("先"), 0.3f);
  EXPECT_FLOAT_EQ(r.table.mass.at("按"), 0.2f);
}

TEST(WordHeadPrior, IsWeightTimesLogOfOneMinusMass) {
  const Table t = TableOf("现\t0.98\n先\t0.04\n");
  EXPECT_NEAR(PriorOf(t, "现", 1.0f), std::log(0.02f), 1e-5f);
  EXPECT_NEAR(PriorOf(t, "先", 2.0f), 2.0f * std::log(0.96f), 1e-5f);
  EXPECT_FLOAT_EQ(PriorOf(t, "走", 1.0f), 0.0f);    // not in the table
  EXPECT_FLOAT_EQ(PriorOf(t, "现在", 1.0f), 0.0f);  // not a single character
}

TEST(WordHeadPrior, MassIsClampedSoACertainWordHeadIsNotMinusInfinity) {
  const Table t = TableOf("现\t1.0\n");
  EXPECT_NEAR(PriorOf(t, "现", 1.0f), std::log(1.0f - 0.999f), 1e-4f);
}

TEST(WordHeadWindow, AllSingleCharacterWindowGetsParallelPriors) {
  const Table t = TableOf("现\t0.98\n先\t0.04\n");
  const auto p = WindowPriors(t, {"先", "现", "线"}, 1.0f);
  ASSERT_EQ(p.size(), 3u);
  EXPECT_NEAR(p[0], std::log(0.96f), 1e-5f);
  EXPECT_NEAR(p[1], std::log(0.02f), 1e-5f);
  EXPECT_FLOAT_EQ(p[2], 0.0f);  // 线 is not in the table
}

// Window-level, not per-candidate: the measurement zeroed any mixed pair, and
// a per-candidate gate would penalise 数 against 又有 -- one of the six
// cross-length promotions, all six harmless.
TEST(WordHeadWindow, AnyMultiCharacterHanCandidateTurnsTheWholeWindowOff) {
  const Table t = TableOf("数\t0.9\n");
  EXPECT_TRUE(WindowPriors(t, {"又有", "数", "书"}, 1.0f).empty());
}

TEST(WordHeadWindow, NonHanCandidatesDoNotDisableItAndGetZero) {
  const Table t = TableOf("现\t0.98\n");
  const auto p = WindowPriors(t, {"先", "xm", "现"}, 1.0f);
  ASSERT_EQ(p.size(), 3u);
  EXPECT_FLOAT_EQ(p[1], 0.0f);
  EXPECT_LT(p[2], 0.0f);
}

TEST(WordHeadWindow, ZeroWeightOrEmptyTableOrNoHanIsNoPriorAtAll) {
  const Table t = TableOf("现\t0.98\n");
  EXPECT_TRUE(WindowPriors(t, {"先", "现"}, 0.0f).empty());
  EXPECT_TRUE(WindowPriors(Table{}, {"先", "现"}, 1.0f).empty());
  EXPECT_TRUE(WindowPriors(t, {"xm", "xian"}, 1.0f).empty());
}
