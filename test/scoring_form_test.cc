#include "scoring_form.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

using rime::AlignToTrainingForm;
using rime::BuildScoringContext;
using rime::kEosCarrier;
using rime::TokenizeScoringForm;

namespace {
std::vector<int> ByteTokens(const std::string& s) {
  std::vector<int> out;
  for (char c : s) out.push_back(static_cast<unsigned char>(c));
  return out;
}
}  // namespace

// The whole point of this file. If it fails, C++ and Python have drifted, and
// every offline number measured after the drift is wrong while every run
// reports success.
TEST(ScoringForm, MatchesThePythonGoldenFixture) {
  const std::string path = std::string(COPILOT_TEST_DATA_DIR) + "/scoring_form_golden.jsonl";
  std::ifstream in(path);
  ASSERT_TRUE(in.is_open()) << "missing fixture: " << path
                            << " -- regenerate with `tools/rime-train scoring-form "
                               "--out test/data/scoring_form_golden.jsonl`";
  std::string line;
  int cases = 0;
  while (std::getline(in, line)) {
    if (line.empty()) continue;
    const auto j = nlohmann::json::parse(line);
    const auto input = j.at("in").get<std::string>();
    const auto expected = j.at("out").get<std::string>();
    EXPECT_EQ(expected, AlignToTrainingForm(input)) << "input: " << input;
    ++cases;
  }
  // The fixture has 21 lines today. A truncated file (e.g. 11 lines) would
  // still pass a >10 check; this is a backstop, not the real guard -- the
  // real one is Python's own `test_golden_fixture_is_in_step_with_scoring_form`,
  // which compares the whole emitted stream against this file byte for byte.
  EXPECT_GE(cases, 20) << "the fixture is suspiciously small";
}

// Order of operations. Whitespace collapsing changes the character count, so
// truncating first would make max_chars mean "characters before alignment".
TEST(ScoringForm, AlignsBeforeTruncating) {
  // 8 raw characters, 5 after the blanks collapse.
  EXPECT_EQ("好 我们走", BuildScoringContext("你好   我们走", 5));
  // The carrier counts as the one character it becomes.
  EXPECT_EQ(std::string(1, kEosCarrier) + "我们", BuildScoringContext("你好。我们", 3));
}

// What the user's own technical writing looks like, and the reason the limit
// counts characters uniformly rather than bytes: 10 back from the end of
// "先去修 build.py 吧" lands inside the Latin run, and every one of those
// 1-byte characters is context the model was trained to read exactly like the
// 3-byte Han characters around them.
TEST(ScoringForm, MixedLatinAndHanTruncatesByCharacterNotByte) {
  EXPECT_EQ("build.py 吧", BuildScoringContext("先去修 build.py 吧", 10));
  EXPECT_EQ("先去修 build.py 吧", BuildScoringContext("先去修 build.py 吧", 32));
}

// Tab is Cc: normalize() deletes it, so no space appears where it was. The
// U+3000 case beside it is Zs and DOES become a space. Getting these two the
// same way round is the difference between matching the training stream and
// quietly not. Verified against the real normalize() before this was written.
TEST(ScoringForm, ControlCharactersAreDeletedNotFolded) {
  EXPECT_EQ("你好我们", AlignToTrainingForm("你好\t\t我们"));
  EXPECT_EQ("你好我们", AlignToTrainingForm("你好\r我们"));
  EXPECT_EQ("你好我们", AlignToTrainingForm("你好\x0b我们"));
  EXPECT_EQ("你好 我们", AlignToTrainingForm("你好\n我们"));
  EXPECT_EQ("你好 我们", AlignToTrainingForm("你好　我们"));
}

TEST(ScoringForm, BuildScoringContextDegenerateInputs) {
  EXPECT_EQ("", BuildScoringContext("", 64));
  EXPECT_EQ("", BuildScoringContext("你好", 0));
  EXPECT_EQ("", BuildScoringContext("   ", 64));
  EXPECT_EQ(std::string("。") + kEosCarrier, BuildScoringContext("。", 64));
}

// The carrier does NOT become token 2 on its own: llama_tokenize's
// parse_special matches a special token's TEXT ("</s>"), so a raw 0x02 byte
// byte-falls-back to the <0x02> piece -- an embedding as untrained as the BOS
// this change removes.
TEST(ScoringForm, TokenizeSplicesEosBetweenRuns) {
  const std::string aligned = std::string("ab") + kEosCarrier + "cd";
  EXPECT_EQ((std::vector<int>{'a', 'b', 2, 'c', 'd'}), TokenizeScoringForm(aligned, 2, ByteTokens));
}

TEST(ScoringForm, TokenizeHandlesCarriersAtBothEnds) {
  EXPECT_EQ((std::vector<int>{2, 'a'}),
            TokenizeScoringForm(std::string(1, kEosCarrier) + "a", 2, ByteTokens));
  EXPECT_EQ((std::vector<int>{'a', 2}),
            TokenizeScoringForm(std::string("a") + kEosCarrier, 2, ByteTokens));
  EXPECT_EQ((std::vector<int>{2, 2}),
            TokenizeScoringForm(std::string(2, kEosCarrier), 2, ByteTokens));
  EXPECT_TRUE(TokenizeScoringForm(std::string(""), 2, ByteTokens).empty());
}

TEST(ScoringForm, TokenizeHandlesNoCarrierAtAll) {
  // The commonest live input: an unfinished sentence with no sentence ender
  // at all, so no carrier is ever spliced in.
  EXPECT_EQ((std::vector<int>{'a', 'b', 'c'}), TokenizeScoringForm("abc", 2, ByteTokens));
}

// The fixture cannot cover this: SCORING_FORM_CASES is hand-written and has
// one non-Cc category-C case, which sits inside an already-covered range.
// Python drops every character of major category C (Cc, Cf, Cs, Co), not just
// Cc/Cf -- and Co is where Nerd Font and powerline glyphs live, which the
// tmux source puts in `before` on every prompt.
TEST(ScoringForm, DropsEveryCategoryCClass) {
  EXPECT_EQ("运行失败", AlignToTrainingForm("运行失败"));  // Co, powerline separator
  EXPECT_EQ("运行失败", AlignToTrainingForm("运行失败"));  // Co, Apple logo (PUA)
  EXPECT_EQ("你好世界", AlignToTrainingForm("你好᠎世界"));  // Cf, Mongolian vowel separator
  EXPECT_EQ("你好世界", AlignToTrainingForm("你好⁦世界"));  // Cf, bidi isolate
  EXPECT_EQ("你好世界", AlignToTrainingForm("你好­世界"));        // Cf, soft hyphen
}

// A single malformed byte must not discard everything after it: `before`
// comes from a tmux `capture-pane` slice or from IMK surrounding text, and
// the caret-adjacent text sits wherever it sits relative to a stray byte
// upstream. Matches `copilot::UTF8`'s `SplitU8` (utf8_index.h), which skips one
// bad byte and keeps going rather than stopping at it.
TEST(ScoringForm, MalformedUtf8DropsOneByteNotTheTail) {
  const std::string bad = std::string("你好") + "\x80" + "我们。";
  EXPECT_EQ("你好我们。" + std::string(1, kEosCarrier), AlignToTrainingForm(bad));
}
