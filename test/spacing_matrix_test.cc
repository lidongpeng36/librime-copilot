// Scenario-oriented characterization matrix for AutoSpacer's surrounding-context
// spacing. Each case mirrors a real typing situation that used to be checked by
// hand. DecorateCommitText is the shared decision used by both the IMK and the
// ImeBridge paths, so pinning it here characterizes the IMK path's decisions too.
//
// These document CURRENT behavior (expected to pass). If one fails, current
// behavior differs from expectation — investigate before changing anything.

#include "auto_spacer_util.h"

#include <gtest/gtest.h>

using rime::auto_spacer_detail::DecorateCommitText;

namespace {
// content_is_ascii=true  -> committing ASCII (e.g. typing English) around CJK.
// content_is_ascii=false -> committing CJK (e.g. a Chinese candidate).
std::string CommitAscii(const std::string& text, const std::string& before,
                        const std::string& after, bool enable_right = true) {
  return DecorateCommitText(text, before, after, /*content_is_ascii=*/true, enable_right);
}
std::string CommitCjk(const std::string& text, const std::string& before, const std::string& after,
                      bool enable_right = true) {
  return DecorateCommitText(text, before, after, /*content_is_ascii=*/false, enable_right);
}
}  // namespace

// --- Committing ASCII around CJK (content_is_ascii = true) ---
TEST(SpacingMatrix, AsciiAfterCjkGetsLeadingSpace) {
  EXPECT_EQ(" test", CommitAscii("test", "中", ""));  // 中|test -> space
}
TEST(SpacingMatrix, AsciiAfterAsciiNoSpace) {
  EXPECT_EQ("test", CommitAscii("test", "o", ""));  // o|test -> none
  EXPECT_EQ("test", CommitAscii("test", "5", ""));  // 5|test -> none
}
TEST(SpacingMatrix, AsciiAfterPunct) {
  EXPECT_EQ(" test", CommitAscii("test", ",", ""));  // ,|test -> space
  EXPECT_EQ("test", CommitAscii("test", ".", ""));   // '.' excluded for ascii
  EXPECT_EQ("test", CommitAscii("test", " ", ""));   // already a space
}
TEST(SpacingMatrix, AsciiBeforeCjkHonorsRightSpace) {
  EXPECT_EQ("test ", CommitAscii("test", "", "中", true));  // test|中 -> trailing
  EXPECT_EQ("test", CommitAscii("test", "", "中", false));  // disabled
}

// --- Committing CJK around ASCII (content_is_ascii = false) ---
TEST(SpacingMatrix, CjkAfterAsciiGetsLeadingSpace) {
  EXPECT_EQ(" 中文", CommitCjk("中文", "o", ""));  // o|中文 -> space
  EXPECT_EQ(" 中文", CommitCjk("中文", "5", ""));  // 5|中文 -> space
  EXPECT_EQ(" 中文", CommitCjk("中文", ".", ""));  // .|中文 -> space (full variant)
}
TEST(SpacingMatrix, CjkAfterCjkNoSpace) {
  EXPECT_EQ("中文", CommitCjk("中文", "文", ""));  // 文|中文 -> none
  EXPECT_EQ("中文", CommitCjk("中文", " ", ""));   // space -> none
}
TEST(SpacingMatrix, CjkBeforeAsciiHonorsRightSpace) {
  EXPECT_EQ("中文 ", CommitCjk("中文", "", "a", true));  // 中文|a -> trailing
  EXPECT_EQ("中文", CommitCjk("中文", "", "a", false));  // disabled
  EXPECT_EQ("中文", CommitCjk("中文", "", "国", true));  // 中文|国 -> none
}
TEST(SpacingMatrix, ChinesePunctuationNeverSpaced) {
  EXPECT_EQ("中文", CommitCjk("中文", "，", "。", true));  // punct on both sides
  EXPECT_EQ("。", CommitCjk("。", "a", "b", true));        // punct payload untouched
}
TEST(SpacingMatrix, WhitespaceTrimmedBeforeDecoration) {
  EXPECT_EQ(" 中文", CommitCjk("  中文  ", "o", ""));  // trimmed then decorated
}

// --- Retained-space scenario: inserting CJK into a `CJK <space> ASCII` gap ---
// Document `中文 |a` + 测试. before is the pre-existing space, after is `a`.
// CURRENT behavior: no left space (space guard) + right space (before `a`) -> "测试 ",
// leaving the app's pre-existing space stranded between 中文 and 测试
// (net `中文 测试 a`). IMK point 2 will later change this; pin it now so that
// fix is a deliberate, visible diff.
TEST(SpacingMatrix, RetainedSpaceScenario_CurrentBehavior) {
  EXPECT_EQ("测试 ", CommitCjk("测试", " ", "a", true));
}
