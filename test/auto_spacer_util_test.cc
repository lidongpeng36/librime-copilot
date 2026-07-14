// Unit tests for AutoSpacer's pure spacing helpers (auto_spacer_util.h).
//
// These are dependency-free string functions, so no Rime engine is needed.
// The tests characterize the current (intended) spacing behavior around the
// CJK / Latin boundary so future refactors don't silently change it.

#include "auto_spacer_util.h"

#include <gtest/gtest.h>

using namespace rime::auto_spacer_detail;

TEST(Utf8ToCodepoint, DecodesByLength) {
  EXPECT_EQ(0x41u, Utf8ToCodepoint("A"));     // 1-byte ASCII
  EXPECT_EQ(0x3002u, Utf8ToCodepoint("。"));  // 。 3-byte
  EXPECT_EQ(0x4E2Du, Utf8ToCodepoint("中"));  // 中 3-byte
  EXPECT_EQ(0xFF01u, Utf8ToCodepoint("！"));  // ！ full-width
}

TEST(IsChinesePunctuation, RecognizesCjkAndFullWidth) {
  EXPECT_TRUE(IsChinesePunctuation("。"));   // 。
  EXPECT_TRUE(IsChinesePunctuation("，"));   // ，
  EXPECT_TRUE(IsChinesePunctuation("！"));   // ！
  EXPECT_FALSE(IsChinesePunctuation("."));   // ASCII period
  EXPECT_FALSE(IsChinesePunctuation("中"));  // 中 is not punctuation
  EXPECT_FALSE(IsChinesePunctuation("a"));
  EXPECT_FALSE(IsChinesePunctuation(""));
}

TEST(LastAsciiCharCode, ReturnsTrailingAsciiOrNegative) {
  EXPECT_EQ('c', LastAsciiCharCode("abc"));
  EXPECT_EQ('a', LastAsciiCharCode("中a"));  // 中a -> 'a'
  EXPECT_EQ(-1, LastAsciiCharCode("中"));    // 中 (non-ascii tail)
  EXPECT_EQ(-1, LastAsciiCharCode("a中"));   // a中 -> tail is CJK
  EXPECT_EQ(-1, LastAsciiCharCode(""));
}

TEST(GetLastUtf8Char, ReturnsFinalCodepoint) {
  EXPECT_EQ("c", GetLastUtf8Char("abc"));
  EXPECT_EQ("文", GetLastUtf8Char("中文"));  // 中文 -> 文
  EXPECT_EQ("", GetLastUtf8Char(""));
}

TEST(GetFirstUtf8Char, ReturnsLeadingCodepoint) {
  EXPECT_EQ("a", GetFirstUtf8Char("abc"));
  EXPECT_EQ("中", GetFirstUtf8Char("中文"));  // 中文 -> 中
  EXPECT_EQ("", GetFirstUtf8Char(""));
}

TEST(AsciiPunctClassification, RightPunctVariants) {
  EXPECT_TRUE(IsAsciiRightPunctCode('.'));
  EXPECT_TRUE(IsAsciiRightPunctCode(','));
  EXPECT_TRUE(IsAsciiRightPunctCode('?'));
  // The ASCII-input variant excludes '.'
  EXPECT_FALSE(IsAsciiRightPunctCodeForAsciiInput('.'));
  EXPECT_TRUE(IsAsciiRightPunctCodeForAsciiInput(','));
  EXPECT_TRUE(IsAsciiRightPunctCodeForAsciiInput('?'));
}

TEST(CharClassification, CjkAndPureAscii) {
  EXPECT_TRUE(IsCjkNonPunctuationChar("中"));   // 中
  EXPECT_FALSE(IsCjkNonPunctuationChar("。"));  // 。 is punctuation
  EXPECT_FALSE(IsCjkNonPunctuationChar("a"));
  EXPECT_FALSE(IsCjkNonPunctuationChar(""));

  EXPECT_TRUE(IsPureAsciiText("abc123"));
  EXPECT_FALSE(IsPureAsciiText("a中"));
  EXPECT_FALSE(IsPureAsciiText(""));
}

TEST(NeedSpaceBefore, AsciiContent) {
  // Inserting ASCII after these boundaries:
  EXPECT_TRUE(NeedSpaceBefore("中", true));   // 中|ascii -> space
  EXPECT_FALSE(NeedSpaceBefore("a", true));   // english|english -> no space
  EXPECT_TRUE(NeedSpaceBefore(",", true));    // ,|ascii -> space
  EXPECT_FALSE(NeedSpaceBefore(".", true));   // '.' excluded for ascii input
  EXPECT_FALSE(NeedSpaceBefore(" ", true));   // already a space
  EXPECT_FALSE(NeedSpaceBefore("。", true));  // 。 chinese punct -> no space
  EXPECT_FALSE(NeedSpaceBefore("", true));
}

TEST(NeedSpaceBefore, CjkContent) {
  // Inserting CJK after these boundaries:
  EXPECT_TRUE(NeedSpaceBefore("a", false));    // english|中 -> space
  EXPECT_TRUE(NeedSpaceBefore("5", false));    // digit|中 -> space
  EXPECT_TRUE(NeedSpaceBefore(".", false));    // '.'|中 -> space (full variant)
  EXPECT_FALSE(NeedSpaceBefore("中", false));  // 中|中 -> no space
  EXPECT_FALSE(NeedSpaceBefore("", false));
}

TEST(NeedSpaceAfter, DependsOnContentMode) {
  // ASCII content, followed by:
  EXPECT_TRUE(NeedSpaceAfter("中", true));   // ascii|中 -> space
  EXPECT_FALSE(NeedSpaceAfter("a", true));   // ascii|english -> no space
  EXPECT_FALSE(NeedSpaceAfter("。", true));  // ascii|。 -> no space
  // CJK content, followed by:
  EXPECT_TRUE(NeedSpaceAfter("a", false));    // 中|english -> space
  EXPECT_FALSE(NeedSpaceAfter("中", false));  // 中|中 -> no space
  EXPECT_FALSE(NeedSpaceAfter("", false));
}

TEST(DecorateCommitText, AddsLeftSpaceForCjkBoundary) {
  // ASCII content committed after Chinese: leading space.
  EXPECT_EQ(" test", DecorateCommitText("test", "中", "", true, true));
  // CJK content committed after English: leading space.
  EXPECT_EQ(" 中文", DecorateCommitText("中文", "abc", "", false, true));
}

TEST(DecorateCommitText, RightSpaceHonorsFlag) {
  // CJK content before following English: trailing space when enabled.
  EXPECT_EQ("中文 ", DecorateCommitText("中文", "", "def", false, true));
  // ...and no trailing space when enable_space_after is false.
  EXPECT_EQ("中文", DecorateCommitText("中文", "", "def", false, false));
}

TEST(DecorateCommitText, BothSides) {
  // English -> [中文] -> 国: space on both sides.
  EXPECT_EQ(" 中文 ", DecorateCommitText("中文", "abc", "def", false, true));
}

TEST(DecorateCommitText, TrimsAndSkipsChinesePunct) {
  // Surrounding whitespace in the raw text is trimmed before decoration.
  EXPECT_EQ(" test", DecorateCommitText("  test  ", "中", "", true, true));
  // A Chinese-punctuation payload is returned untouched (no auto-spacing).
  EXPECT_EQ("。", DecorateCommitText("。", "a", "b", false, true));
  // Empty stays empty.
  EXPECT_EQ("", DecorateCommitText("", "中", "中", true, true));
}
