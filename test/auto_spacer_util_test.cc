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

TEST(IsChinesePunctuation, RejectsMultiCharacterStrings) {
  // The predicate decoded any string of <= 4 bytes as if it were a single
  // UTF-8 character, so short ASCII runs could land inside the CJK punctuation
  // range: "3@x" decoded to U+3020. A commit like that then skipped auto
  // spacing (DecorateCommitText / ProcessWithCommitHistory both gate on this).
  EXPECT_FALSE(IsChinesePunctuation("3@x"));
  EXPECT_FALSE(IsChinesePunctuation("c@ "));
  EXPECT_FALSE(IsChinesePunctuation("s@a"));
  EXPECT_FALSE(IsChinesePunctuation("ab"));
  EXPECT_FALSE(IsChinesePunctuation("。。"));  // two real puncts are not one
  EXPECT_FALSE(IsChinesePunctuation("。a"));
  // Truncated / malformed sequences are not punctuation either.
  EXPECT_FALSE(IsChinesePunctuation(std::string("\xE3\x80")));  // 2 of 3 bytes
  EXPECT_FALSE(IsChinesePunctuation(std::string("\x80")));      // lone continuation
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

// A newline before the caret is a boundary, exactly as a space is. No source
// produces this string yet -- nvim reads only the caret's line and tmux slices a
// single row -- but step (c) of the surrounding-context design makes `before`
// carry real newlines, and the predicate must not answer that question by
// falling through to IsAsciiRightPunctCode('\n').
TEST(AutoSpacerUtil, WhitespaceBeforeNeedsNoSpace) {
  using rime::auto_spacer_detail::NeedSpaceBefore;
  EXPECT_FALSE(NeedSpaceBefore("上一行\n", true));
  EXPECT_FALSE(NeedSpaceBefore("上一行\n", false));
  EXPECT_FALSE(NeedSpaceBefore("word\t", true));
  EXPECT_FALSE(NeedSpaceBefore("word\t", false));
  EXPECT_FALSE(NeedSpaceBefore("word\r", false));
}

TEST(AutoSpacerUtil, WhitespaceAfterNeedsNoSpace) {
  using rime::auto_spacer_detail::NeedSpaceAfter;
  EXPECT_FALSE(NeedSpaceAfter("\n下一行", true));
  EXPECT_FALSE(NeedSpaceAfter("\n下一行", false));
  EXPECT_FALSE(NeedSpaceAfter("\tword", true));
}

// Regression guard: the ordinary cases must be untouched by the change above.
TEST(AutoSpacerUtil, WhitespaceHardeningLeavesOrdinaryCasesAlone) {
  using rime::auto_spacer_detail::NeedSpaceAfter;
  using rime::auto_spacer_detail::NeedSpaceBefore;
  EXPECT_TRUE(NeedSpaceBefore("今天", true));      // CJK then ascii content
  EXPECT_TRUE(NeedSpaceBefore("word", false));     // ascii then CJK content
  EXPECT_FALSE(NeedSpaceBefore("今天，", false));  // Chinese punctuation
  EXPECT_TRUE(NeedSpaceAfter("今天", true));
}

// IsContinuingSelfCommittedRawAscii is NeedAddSpace's "don't re-space a raw
// ASCII run we are still typing" check, extracted as a single predicate.
TEST(IsContinuingSelfCommittedRawAscii, TrueForRawOrThruEndingInAlnum) {
  EXPECT_TRUE(IsContinuingSelfCommittedRawAscii("raw", " h"));
  EXPECT_TRUE(IsContinuingSelfCommittedRawAscii("thru", "he"));
  EXPECT_TRUE(IsContinuingSelfCommittedRawAscii("raw", "9"));
}

TEST(IsContinuingSelfCommittedRawAscii, FalseForOtherRecordTypes) {
  // A selected CJK candidate is not a raw ASCII run, whatever it ends in.
  EXPECT_FALSE(IsContinuingSelfCommittedRawAscii("select", "he"));
  EXPECT_FALSE(IsContinuingSelfCommittedRawAscii("", "he"));
}

TEST(IsContinuingSelfCommittedRawAscii, FalseWhenLatestTextDoesNotEndInAlnum) {
  EXPECT_FALSE(IsContinuingSelfCommittedRawAscii("raw", "中"));
  EXPECT_FALSE(IsContinuingSelfCommittedRawAscii("raw", ""));
  EXPECT_FALSE(IsContinuingSelfCommittedRawAscii("raw", " h "));  // trailing space
}

// ResolveBoundaryBefore decides between the screen (`raw_before`, which over
// ssh lags the round trip) and our own last commit_history record. It is only
// consulted once the caller has established that record is still at this
// caret (LocalCommitStillAtCaret below).
TEST(ResolveBoundaryBefore, TrustsRawBeforeWhenItEndsWithTheLatestCommit) {
  // Caught up: keep raw_before, it carries more left context.
  EXPECT_EQ("对比下 vLLM", ResolveBoundaryBefore("对比下 vLLM", " vLLM"));
}

TEST(ResolveBoundaryBefore, FallsBackWhenRawBeforeHasNotCaughtUp) {
  // Live repro: raw_before was still the pre-"和" screen, so "和sglang".
  EXPECT_EQ("和", ResolveBoundaryBefore("对, 下完自动校验. 另外,我们对比下", "和"));
  EXPECT_EQ(" h", ResolveBoundaryBefore("中", " h"));
}

TEST(ResolveBoundaryBefore, KeepsRawBeforeWhenLatestTextIsEmpty) {
  // librime clears commit_history on an unhandled Backspace/Return.
  EXPECT_EQ("中文", ResolveBoundaryBefore("中文", ""));
}

// librime records an unhandled printable key as {"thru", ch} only when it has
// no modifier (commit_history.cc), so a Shift-typed "?" is never in
// commit_history while the screen shows it. Only such characters -- ASCII
// punctuation/whitespace -- may trail our commit on a caught-up screen.
TEST(ResolveBoundaryBefore, ToleratesTrailingAsciiPunctuationMissingFromHistory) {
  EXPECT_EQ("❯ 好的, 步骤?", ResolveBoundaryBefore("❯ 好的, 步骤?", " 步骤"));
  EXPECT_TRUE(NeedSpaceBefore(ResolveBoundaryBefore("❯ 好的, 步骤?", " 步骤"),
                              /*content_is_ascii=*/false));
}

TEST(ResolveBoundaryBefore, DoesNotMatchAnEarlierOccurrenceOfTheSameText) {
  // Live repro: "agent" committed in a sentence that already said "agent";
  // the screen still ended in "是", so the CJK commit after it lost its space.
  EXPECT_EQ(" agent",
            ResolveBoundaryBefore("我现在需要的结论是当前我们构建的 agent 到底表现怎么样, "
                                  "还有哪些问题, 这些问题是",
                                  " agent"));
}

TEST(ResolveBoundaryBefore, AManualSpaceIsItsOwnCommit) {
  // Live repro "请求.  百炼": the user's own Space is recorded as {"thru", " "}.
  // Before the screen shows it, a space elsewhere in the line must not count
  // as confirmation -- only the tail does.
  EXPECT_EQ(" ", ResolveBoundaryBefore("我们构建的 agent 请求.", " "));
  EXPECT_FALSE(NeedSpaceBefore(ResolveBoundaryBefore("我们构建的 agent 请求.", " "),
                               /*content_is_ascii=*/false));
  // Once rendered, the screen wins.
  EXPECT_EQ("请求. ", ResolveBoundaryBefore("请求. ", " "));
}

TEST(ResolveBoundaryBefore, ContinuingARawAsciiRunDoesNotReSpace) {
  // Screen still shows the pre-commit CJK tail; we just committed " h".
  EXPECT_FALSE(NeedSpaceBefore(ResolveBoundaryBefore("中", " h"), /*content_is_ascii=*/true));
}

// LocalCommitStillAtCaret gates the fallback above. A disagreement between the
// screen and commit_history means lag only if the commit is recent and was
// made at this caret; commit_history is per Rime session (one terminal
// window), so after a tmux pane switch or a caret move it describes somewhere
// else, and the screen -- accurate locally -- must win.
TEST(LocalCommitStillAtCaret, TrueForARecentCommitOnThisClient) {
  LocalCommitWitness w{"中文", "tmux:s:%1", 1000};
  EXPECT_TRUE(LocalCommitStillAtCaret(w, "中文", "tmux:s:%1", 1500, 3000));
}

TEST(LocalCommitStillAtCaret, FalseAfterAPaneSwitch) {
  // Zero-lag regression this gate exists for: "中文" committed in pane %1,
  // then `ls` typed at pane %2's "$ " prompt got a spurious leading space.
  LocalCommitWitness w{"中文", "tmux:s:%1", 1000};
  EXPECT_FALSE(LocalCommitStillAtCaret(w, "中文", "tmux:s:%2", 1100, 3000));
}

TEST(LocalCommitStillAtCaret, FalseOnceTheLagWindowHasPassed) {
  LocalCommitWitness w{"中文", "tmux:s:%1", 1000};
  EXPECT_FALSE(LocalCommitStillAtCaret(w, "中文", "tmux:s:%1", 4001, 3000));
}

TEST(LocalCommitStillAtCaret, FalseWhenTheWitnessIsForADifferentRecord) {
  LocalCommitWitness w{"中文", "tmux:s:%1", 1000};
  EXPECT_FALSE(LocalCommitStillAtCaret(w, "测试", "tmux:s:%1", 1100, 3000));
}

TEST(LocalCommitStillAtCaret, FalseWhenNothingWasWitnessed) {
  // Default-constructed: no commit seen, or invalidated by a navigation key.
  EXPECT_FALSE(LocalCommitStillAtCaret(LocalCommitWitness{}, "中文", "tmux:s:%1", 1100, 3000));
  EXPECT_FALSE(LocalCommitStillAtCaret(LocalCommitWitness{}, "", "", 1100, 3000));
}

TEST(DecorateCommitText, TrimsAndSkipsChinesePunct) {
  // Surrounding whitespace in the raw text is trimmed before decoration.
  EXPECT_EQ(" test", DecorateCommitText("  test  ", "中", "", true, true));
  // A Chinese-punctuation payload is returned untouched (no auto-spacing).
  EXPECT_EQ("。", DecorateCommitText("。", "a", "b", false, true));
  // Empty stays empty.
  EXPECT_EQ("", DecorateCommitText("", "中", "中", true, true));
}

// A bare digit typed while nothing is composing never reaches Rime's composer
// (digits are not in the speller alphabet), so AutoSpacer is the only place
// that can space it. It is ASCII content whichever mode typed it: `中文 1`.
TEST(DecideBareDigitSpacing, SpacesAfterCjk) {
  EXPECT_TRUE(ShouldSpaceBareDigit("中文", /*composing=*/false));
  EXPECT_TRUE(ShouldSpaceBareDigit("第", false));
}
TEST(DecideBareDigitSpacing, NoSpaceWhereNoneBelongs) {
  EXPECT_FALSE(ShouldSpaceBareDigit("中文 ", false));   // already spaced
  EXPECT_FALSE(ShouldSpaceBareDigit("中文 1", false));  // continuing a number
  EXPECT_FALSE(ShouldSpaceBareDigit("abc", false));     // ASCII word
  EXPECT_FALSE(ShouldSpaceBareDigit("3.", false));      // decimal point
  EXPECT_FALSE(ShouldSpaceBareDigit("中文，", false));  // Chinese punctuation
  EXPECT_FALSE(ShouldSpaceBareDigit("", false));        // start of text
}
// A digit while composing -- including the empty-input copilot prediction
// menu -- selects a candidate; that is HandleNumberKey's business, not this.
TEST(DecideBareDigitSpacing, NeverWhileComposing) {
  EXPECT_FALSE(ShouldSpaceBareDigit("中文", /*composing=*/true));
}
