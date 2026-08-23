#include "tmux_source_util.h"

#include <gtest/gtest.h>

using namespace rime::tmux_detail;

namespace {

// Mirrors the real one-exec output: client lines, then the cursor header,
// then the pane dump verbatim.
std::string Blob(const std::string& clients, const std::string& header, const std::string& rows) {
  return clients + header + rows;
}

}  // namespace

TEST(TmuxParse, ReadsHeaderAndRows) {
  auto snap = ParseTmuxOutput(Blob("CLI|1786506891\n", "CUR|%0|7|0|40\n",
                                   "\xE4\xB8\xAD\xE6\x96\x87"
                                   "abc\nsecond\n"));
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->pane_id, "%0");
  EXPECT_EQ(snap->cursor_x, 7);
  EXPECT_EQ(snap->cursor_y, 0);
  EXPECT_EQ(snap->pane_width, 40);
  ASSERT_EQ(snap->rows.size(), 2u);
  EXPECT_EQ(snap->rows[0],
            "\xE4\xB8\xAD\xE6\x96\x87"
            "abc");
  EXPECT_EQ(snap->client_activity.size(), 1u);
}

TEST(TmuxParse, MissingHeaderIsRejected) {
  EXPECT_FALSE(ParseTmuxOutput("CLI|123\nno header here\n").has_value());
}

TEST(TmuxParse, ReadsTheFocusEventsMarker) {
  auto on = ParseTmuxOutput("CLI|1\nFOC|1\nCUR|%0|1|0|40\nx\n");
  ASSERT_TRUE(on.has_value());
  EXPECT_TRUE(on->focus_events);

  auto off = ParseTmuxOutput("CLI|1\nFOC|0\nCUR|%0|1|0|40\nx\n");
  ASSERT_TRUE(off.has_value());
  EXPECT_FALSE(off->focus_events);
}

TEST(TmuxParse, FocusEventsDefaultsToOffWhenTmuxDoesNotReportIt) {
  // An older tmux that does not know #{focus-events} renders it empty; the
  // marker line may also be missing entirely. Either way we must not assume on.
  auto empty_value = ParseTmuxOutput("CLI|1\nFOC|\nCUR|%0|1|0|40\nx\n");
  ASSERT_TRUE(empty_value.has_value());
  EXPECT_FALSE(empty_value->focus_events);

  auto absent = ParseTmuxOutput("CLI|1\nCUR|%0|1|0|40\nx\n");
  ASSERT_TRUE(absent.has_value());
  EXPECT_FALSE(absent->focus_events);
}

TEST(TmuxParse, AcceptsBothSpellingsOfAnOnFlag) {
  EXPECT_TRUE(ParseTmuxFlag("1"));
  EXPECT_TRUE(ParseTmuxFlag("on"));
  EXPECT_FALSE(ParseTmuxFlag("0"));
  EXPECT_FALSE(ParseTmuxFlag("off"));
  EXPECT_FALSE(ParseTmuxFlag(""));
}

TEST(TmuxParse, APaneRowStartingWithTheFocusMarkerIsStillARow) {
  auto snap = ParseTmuxOutput("CUR|%1|0|0|80\nFOC|1\n");
  ASSERT_TRUE(snap.has_value());
  EXPECT_FALSE(snap->focus_events);  // the marker came after the header
  ASSERT_EQ(snap->rows.size(), 1u);
  EXPECT_EQ(snap->rows[0], "FOC|1");
}

TEST(TmuxParse, RowThatLooksLikeAHeaderIsStillARow) {
  auto snap = ParseTmuxOutput("CUR|%1|0|0|80\nCUR|%9|3|3|80\n");
  ASSERT_TRUE(snap.has_value());
  EXPECT_EQ(snap->pane_id, "%1");
  ASSERT_EQ(snap->rows.size(), 1u);
  EXPECT_EQ(snap->rows[0], "CUR|%9|3|3|80");
}

TEST(TmuxWidth, WideCharactersCountAsTwoColumns) {
  EXPECT_EQ(DisplayWidth(0x4E2D), 2);  // 中
  EXPECT_EQ(DisplayWidth(0xFF21), 2);  // fullwidth A
  EXPECT_EQ(DisplayWidth(0x0061), 1);  // a
  EXPECT_EQ(DisplayWidth(0x0301), 0);  // combining acute
  EXPECT_EQ(DisplayWidthOf("\xE4\xB8\xAD\xE6\x96\x87"
                           "abc"),
            7);
}

TEST(TmuxSlice, SplitsOnDisplayColumnNotCharacterIndex) {
  const std::string row =
      "\xE4\xB8\xAD\xE6\x96\x87"
      "abc";  // 中文abc, 7 cells
  EXPECT_EQ(SliceBeforeColumn(row, 7), row);
  EXPECT_EQ(SliceBeforeColumn(row, 4), "\xE4\xB8\xAD\xE6\x96\x87");  // 中文
  EXPECT_EQ(SliceAfterColumn(row, 4), "abc");
}

TEST(TmuxSlice, CaretInsideAWideGlyphStopsBeforeIt) {
  const std::string row = "\xE4\xB8\xAD\xE6\x96\x87";  // 中文
  EXPECT_EQ(SliceBeforeColumn(row, 3), "\xE4\xB8\xAD");
}

TEST(TmuxSlice, ColumnsPastTheTrimmedRowAreBlanks) {
  // Measured: tmux reports x=10 while the captured row is only 7 cells,
  // because it trims trailing blanks. Those columns really are spaces.
  const std::string row =
      "\xE4\xB8\xAD\xE6\x96\x87"
      "abc";
  EXPECT_EQ(SliceBeforeColumn(row, 10), row + "   ");
  EXPECT_EQ(SliceAfterColumn(row, 10), "");
}

TEST(TmuxContext, PlainCaseTakesTheCharacterLeftOfTheCaret) {
  Snapshot snap;
  snap.cursor_x = 7;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {
      "\xE4\xB8\xAD\xE6\x96\x87"
      "abc"};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "c");
  EXPECT_EQ(ctx->after, "");
}

TEST(TmuxContext, PrefixCharsReturnsMoreContextForPrediction) {
  Snapshot snap;
  snap.cursor_x = 7;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {
      "\xE4\xB8\xAD\xE6\x96\x87"
      "abc"};
  auto ctx = ExtractContext(snap, 8);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before,
            "\xE4\xB8\xAD\xE6\x96\x87"
            "abc");
}

TEST(TmuxContext, CaretAtColumnZeroFollowsAWrappedRowAbove) {
  // Row above is exactly pane_width, so the line wrapped and the previous
  // row's tail really is adjacent to the caret.
  Snapshot snap;
  snap.cursor_x = 0;
  snap.cursor_y = 1;
  snap.pane_width = 4;
  snap.rows = {"abcd", ""};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "d");
}

TEST(TmuxContext, CaretAtColumnZeroAfterAShortRowIsAFreshLine) {
  // Row above is shorter than pane_width, so it ended with a real newline.
  // Guessing "d" here would glue two unrelated lines together.
  Snapshot snap;
  snap.cursor_x = 0;
  snap.cursor_y = 1;
  snap.pane_width = 40;
  snap.rows = {"abcd", ""};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "");
}

TEST(TmuxContext, NonBreakingSpaceFromThePromptNormalizesToASpace) {
  // powerlevel10k-style prompts pad with U+00A0; AutoSpacer must see a blank.
  Snapshot snap;
  snap.cursor_x = 2;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {"\xE2\x9D\xAF\xC2\xA0"};  // ❯ + nbsp
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, " ");
}

TEST(TmuxContext, CursorRowOutOfRangeIsRefused) {
  Snapshot snap;
  snap.cursor_x = 0;
  snap.cursor_y = 5;
  snap.pane_width = 40;
  snap.rows = {"only one row"};
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());
}

TEST(TmuxContext, CursorColumnPastThePaneWidthIsRefused) {
  // `std::atoi` on a malformed header will return anything at all, and
  // SliceBeforeColumn materialises the blanks tmux trimmed -- so an unbounded
  // column is a multi-gigabyte allocation on the input thread.
  Snapshot snap;
  snap.cursor_x = 2000000000;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {"hello"};
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());

  snap.cursor_x = 41;
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());
}

TEST(TmuxContext, CursorColumnExactlyAtThePaneWidthIsAccepted) {
  // The caret legally sits one past the last cell just before a wrap.
  Snapshot snap;
  snap.cursor_x = 5;
  snap.cursor_y = 0;
  snap.pane_width = 5;
  snap.rows = {"hello"};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "o");
}

TEST(TmuxContext, NegativeCursorColumnIsRefused) {
  Snapshot snap;
  snap.cursor_x = -1;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {"hello"};
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());
}

TEST(TmuxContext, NonPositivePaneWidthIsRefused) {
  // A zero width is what an unparseable header field decays to; without a
  // width there is nothing to bound the column against.
  Snapshot snap;
  snap.cursor_x = 3;
  snap.cursor_y = 0;
  snap.pane_width = 0;
  snap.rows = {"hello"};
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());
}

TEST(TmuxContext, ImplausiblePaneWidthIsRefused) {
  // The exact repro that motivated kMaxPaneWidth: a header of
  // CUR|%0|2000000000|0|2000000000 passes every *other* guard --
  // pane_width > 0, cursor_x >= 0, cursor_x <= pane_width -- because both
  // fields are equally huge std::atoi output. Without an upper bound on
  // pane_width alone, this reaches SliceBeforeColumn and tries to append
  // ~2e9 blank characters on the input thread.
  Snapshot snap;
  snap.cursor_x = 2000000000;
  snap.cursor_y = 0;
  snap.pane_width = 2000000000;
  snap.rows = {"short row"};
  EXPECT_FALSE(ExtractContext(snap, 1).has_value());
}

TEST(TmuxContext, WidePaneWithinTheBoundIsStillAccepted) {
  // A real wide terminal -- an ultrawide or a 4K display at a small font --
  // can plausibly reach several hundred columns. The bound must not reject
  // ordinary use to guard against implausible ones.
  const int width = 400;
  Snapshot snap;
  snap.cursor_x = width;
  snap.cursor_y = 0;
  snap.pane_width = width;
  snap.rows = {std::string(static_cast<size_t>(width) - 1, 'x') + "y"};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "y");
}

TEST(TmuxAmbiguity, SingleClientIsNeverAmbiguous) {
  EXPECT_FALSE(ClientsAreAmbiguous({1786506891}));
}

TEST(TmuxAmbiguity, DistinctActivityPicksAWinner) {
  EXPECT_FALSE(ClientsAreAmbiguous({1786506891, 1786506800}));
}

TEST(TmuxAmbiguity, TiedActivityRefuses) {
  // client_activity has one-second granularity; a tie means we genuinely
  // cannot tell which terminal the keyboard is pointed at.
  EXPECT_TRUE(ClientsAreAmbiguous({1786506891, 1786506891}));
}

TEST(TmuxVerdict, OneAttachedClientIsAccepted) {
  // A single client is unambiguous whatever focus-events says -- there is
  // nothing for tmux to pick between.
  EXPECT_EQ(JudgeClients({1786506891}, true), ClientVerdict::kAccept);
  EXPECT_EQ(JudgeClients({1786506891}, false), ClientVerdict::kAccept);
}

TEST(TmuxVerdict, NoAttachedClientIsRefused) {
  // `display-message -p` with no target still answers by falling back to the
  // most-recently-used session, so an empty client list means we would be
  // reading a pane nobody is looking at: the user types at a bare shell while
  // a tmux session sits detached, and AutoSpacer gets the detached pane.
  EXPECT_EQ(JudgeClients({}, true), ClientVerdict::kNoClientAttached);
  EXPECT_EQ(JudgeClients({}, false), ClientVerdict::kNoClientAttached);
}

TEST(TmuxVerdict, MultipleClientsWithFocusEventsOffAreRefused) {
  // Distinct activity timestamps look decisive, but with focus-events off they
  // track "last typed into", not "focused" -- so tmux answers confidently from
  // the wrong pane. That is worse than a tie, not better.
  EXPECT_EQ(JudgeClients({1786506891, 1786506800}, false), ClientVerdict::kFocusEventsOff);
}

TEST(TmuxVerdict, MultipleClientsWithFocusEventsOnFollowActivity) {
  EXPECT_EQ(JudgeClients({1786506891, 1786506800}, true), ClientVerdict::kAccept);
  EXPECT_EQ(JudgeClients({1786506891, 1786506891}, true), ClientVerdict::kAmbiguousClients);
}

TEST(TmuxArgs, NoSocketOmitsDashS) {
  auto args = BuildTmuxArgs("");
  EXPECT_EQ(std::find(args.begin(), args.end(), "-S"), args.end());
}

TEST(TmuxArgs, SocketAddsDashSAsItsOwnPairOfElements) {
  auto args = BuildTmuxArgs("/tmp/mysock");
  ASSERT_GE(args.size(), 2u);
  EXPECT_EQ(args[0], "-S");
  EXPECT_EQ(args[1], "/tmp/mysock");
}

TEST(TmuxArgs, EachSemicolonIsItsOwnArgvElement) {
  // tmux's command separator must never be glued onto an adjacent token, or
  // it stops being recognized as a separator at all.
  auto args = BuildTmuxArgs("");
  int semicolons = 0;
  for (const auto& a : args) {
    if (a == ";") ++semicolons;
    EXPECT_EQ(a.find(';'), a == ";" ? 0u : std::string::npos);
  }
  EXPECT_EQ(semicolons, 3);
}

TEST(TmuxArgs, AsksForFocusEventsInTheSameExec) {
  // A second spawn to read one option would double the per-key cost.
  auto args = BuildTmuxArgs("");
  EXPECT_NE(std::find(args.begin(), args.end(), "FOC|#{focus-events}"), args.end());
}

TEST(TmuxArgs, EveryMarkerIsEmittedBeforeThePaneDump) {
  // ParseTmuxOutput only treats CLI|/FOC| specially before the CUR header, so
  // a pane row that starts with a marker stays a row. That only holds if the
  // argv really does put both markers ahead of capture-pane.
  auto args = BuildTmuxArgs("");
  auto cli = std::find(args.begin(), args.end(), "CLI|#{client_activity}");
  auto foc = std::find(args.begin(), args.end(), "FOC|#{focus-events}");
  auto cur =
      std::find(args.begin(), args.end(), "CUR|#{pane_id}|#{cursor_x}|#{cursor_y}|#{pane_width}");
  ASSERT_NE(cur, args.end());
  EXPECT_LT(cli, cur);
  EXPECT_LT(foc, cur);
}

TEST(TmuxArgs, NoDashTAnywhere) {
  // display-message and capture-pane must resolve the *same* current pane;
  // an explicit -t would defeat that.
  auto args = BuildTmuxArgs("/tmp/mysock");
  EXPECT_EQ(std::find(args.begin(), args.end(), "-t"), args.end());
}

TEST(TmuxArgs, ContainsTheThreeSubcommandsInOrder) {
  auto args = BuildTmuxArgs("");
  auto lc = std::find(args.begin(), args.end(), "list-clients");
  auto dm = std::find(args.begin(), args.end(), "display-message");
  auto cp = std::find(args.begin(), args.end(), "capture-pane");
  ASSERT_NE(lc, args.end());
  ASSERT_NE(dm, args.end());
  ASSERT_NE(cp, args.end());
  EXPECT_LT(lc, dm);
  EXPECT_LT(dm, cp);
}

TEST(TmuxClientKey, EmptySocketUsesDefaultTag) {
  EXPECT_EQ(MakeClientKey("", "%3"), "tmux:default:%3");
}

TEST(TmuxClientKey, EmbedsTheConfiguredSocket) {
  EXPECT_EQ(MakeClientKey("/tmp/mysock", "%3"), "tmux:/tmp/mysock:%3");
}

TEST(TmuxClientKey, DifferentPaneIdsProduceDifferentKeys) {
  // The pane id must be load-bearing: AutoSpacer indexes per-client state by
  // this key, so two panes sharing a key would bleed spacing state together.
  EXPECT_NE(MakeClientKey("", "%1"), MakeClientKey("", "%2"));
}

// ---------------------------------------------------------------------------
// Cell styling: telling the buffer apart from the app's own ghost text.
//
// The rows below are the real, measured `capture-pane -p -e` output of codex
// running under tmux (v0.147.0, 2026-08-15). They are what the bug report
// reduces to.
// ---------------------------------------------------------------------------

namespace {

// codex with an EMPTY composer: the prompt marker is bold, then a dim
// placeholder suggestion sits exactly where the caret is (cursor_x = 2).
const std::string kCodexGhostRow = "\x1b[1m\xE2\x80\xBA\x1b[0m \x1b[2mExplain this codebase\x1b[0m";

// codex with REAL text typed and the caret moved back into it (cursor_x = 8).
// Nothing after the prompt marker is styled at all.
const std::string kCodexTypedRow = "\x1b[1m\xE2\x80\xBA\x1b[0m  hello world";

}  // namespace

TEST(TmuxStyle, StripsSgrAndKeepsOneStylePerCharacter) {
  auto row = SplitStyledRow(kCodexGhostRow);
  EXPECT_EQ(row.plain, "\xE2\x80\xBA Explain this codebase");
  // 1 (›) + 1 (space) + 21 ("Explain this codebase")
  ASSERT_EQ(row.styles.size(), 23u);
  EXPECT_EQ(row.styles[0].attrs, kAttrBold);
  EXPECT_EQ(row.styles[1], CellStyle{});
  EXPECT_EQ(row.styles[2].attrs, kAttrDim);
}

TEST(TmuxStyle, AnUnstyledRowIsAllDefaultAndUnchanged) {
  auto row = SplitStyledRow("hello world");
  EXPECT_EQ(row.plain, "hello world");
  ASSERT_EQ(row.styles.size(), 11u);
  for (const auto& s : row.styles) EXPECT_EQ(s, CellStyle{});
}

TEST(TmuxStyle, ReadsIndexedAndTrueColorForegrounds) {
  auto indexed = SplitStyledRow("\x1b[38;5;6mA");
  auto truecolor = SplitStyledRow("\x1b[38;2;246;226;183mA");
  auto basic = SplitStyledRow("\x1b[31mA");
  EXPECT_NE(indexed.styles[0], CellStyle{});
  EXPECT_NE(truecolor.styles[0], CellStyle{});
  EXPECT_NE(basic.styles[0], CellStyle{});
  // Three different ways of saying "not the default colour" must not collide,
  // or a style change would read as "no change" and the ghost text would pass.
  EXPECT_NE(indexed.styles[0], truecolor.styles[0]);
  EXPECT_NE(indexed.styles[0], basic.styles[0]);
}

TEST(TmuxStyle, AttributeAndColourResetsReturnToTheDefault) {
  // A cell reached by "turn dim on, turn dim off" renders exactly like a cell
  // that was never touched; comparing raw escape state instead of decoded
  // state would call those two different and refuse for no reason.
  auto row = SplitStyledRow("\x1b[2ma\x1b[22mb\x1b[31mc\x1b[39md");
  ASSERT_EQ(row.styles.size(), 4u);
  EXPECT_EQ(row.styles[0].attrs, kAttrDim);
  EXPECT_EQ(row.styles[1], CellStyle{});
  EXPECT_NE(row.styles[2], CellStyle{});
  EXPECT_EQ(row.styles[3], CellStyle{});
}

TEST(TmuxStyle, StyleAtColumnCountsDisplayColumnsNotCharacters) {
  auto row = SplitStyledRow("\x1b[1m\xE4\xB8\xAD\x1b[0mab");  // 中(bold, 2 cells) a b
  EXPECT_EQ(StyleAtColumn(row.plain, row.styles, 0).attrs, kAttrBold);
  EXPECT_EQ(StyleAtColumn(row.plain, row.styles, 1).attrs, kAttrBold);  // 中's 2nd cell
  EXPECT_EQ(StyleAtColumn(row.plain, row.styles, 2), CellStyle{});
}

TEST(TmuxStyle, StyleAtColumnOutsideTheRowIsTheDefault) {
  auto row = SplitStyledRow("\x1b[2mab");
  EXPECT_EQ(StyleAtColumn(row.plain, row.styles, 9), CellStyle{});
  EXPECT_EQ(StyleAtColumn(row.plain, row.styles, -1), CellStyle{});
  // A snapshot carrying no style information at all (every hand-built one, and
  // any tmux that answered without escapes) must decode as "all default".
  EXPECT_EQ(StyleAtColumn("ab", {}, 0), CellStyle{});
}

TEST(TmuxParse, StripsEscapesFromRowsAndKeepsTheCaretRowStyles) {
  auto snap = ParseTmuxOutput("CLI|1\nFOC|1\nCUR|%0|2|1|80\nfirst\n" + kCodexGhostRow + "\n");
  ASSERT_TRUE(snap.has_value());
  ASSERT_EQ(snap->rows.size(), 2u);
  // Rows stay plain text, so every downstream slicing rule is unaffected.
  EXPECT_EQ(snap->rows[1], "\xE2\x80\xBA Explain this codebase");
  ASSERT_EQ(snap->caret_row_styles.size(), 23u);
  EXPECT_EQ(snap->caret_row_styles[2].attrs, kAttrDim);
  // The row above the caret is kept too: it is where `before` comes from when
  // the caret sits at column 0 of a wrapped line.
  ASSERT_EQ(snap->above_row_styles.size(), 5u);
}

TEST(TmuxContext, GhostTextRightOfTheCaretIsNotReportedAsAfter) {
  // The bug: codex draws a dim placeholder at the caret while the composer is
  // empty. capture-pane sees characters, so `after` became "E", NeedSpaceAfter
  // saw an ASCII letter, and every first CJK commit grew a trailing space.
  auto snap = ParseTmuxOutput("CLI|1\nFOC|1\nCUR|%0|2|0|80\n" + kCodexGhostRow + "\n");
  ASSERT_TRUE(snap.has_value());
  auto ctx = ExtractContext(*snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->after, "");
  // Only `after` is dropped. The left boundary is still read normally.
  EXPECT_EQ(ctx->before, " ");
}

TEST(TmuxContext, RealTextRightOfTheCaretIsStillReportedAsAfter) {
  // Same app, same prompt marker -- but the caret was moved back into text the
  // user actually typed, which renders in the same style as its neighbour.
  // Refusing here would be the regression: no more right-hand auto-spacing.
  auto snap = ParseTmuxOutput("CLI|1\nFOC|1\nCUR|%0|9|0|80\n" + kCodexTypedRow + "\n");
  ASSERT_TRUE(snap.has_value());
  ASSERT_EQ(snap->rows[0], "\xE2\x80\xBA  hello world");
  auto ctx = ExtractContext(*snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->after, "w");   // committing CJK here does want a space after
  EXPECT_EQ(ctx->before, " ");  // the caret sits between "hello" and "world"
}

TEST(TmuxContext, AGreyedRunAcrossTheCaretRefusesWithNoDimAnywhere) {
  // Not every ghost text is faint: zsh-autosuggestions defaults to `fg=8` and
  // fish to a 256-colour grey, which is SGR 90 here and carries no attribute.
  auto snap = ParseTmuxOutput("CLI|1\nFOC|1\nCUR|%0|2|0|80\ngi\x1b[90mt status\n");
  ASSERT_TRUE(snap.has_value());
  auto ctx = ExtractContext(*snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->after, "");
}

TEST(TmuxContext, SyntaxHighlightingUnderTheCaretIsStillTrusted) {
  // Measured from `vim -u NONE -c 'syntax on'` on this repo: the preprocessor
  // directive is 35 and the header name after it is 31. A hue boundary is not
  // evidence about whose text it is, and refusing here would cost a trailing
  // space in an editor for nothing.
  auto snap = ParseTmuxOutput(
      "CLI|1\nFOC|1\nCUR|%0|9|0|80\n\x1b[35m#include \x1b[31m<algorithm>\x1b[39m\n");
  ASSERT_TRUE(snap.has_value());
  ASSERT_EQ(snap->rows[0], "#include <algorithm>");
  auto ctx = ExtractContext(*snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, " ");
  EXPECT_EQ(ctx->after, "<");
}

TEST(TmuxStyle, MutedForegroundsAreTheGreysAndNothingElse) {
  EXPECT_TRUE(IsMutedColor(CellStyle::kBasic | 8));      // SGR 90, bright black
  EXPECT_TRUE(IsMutedColor(CellStyle::kIndexed | 8));    // 38;5;8
  EXPECT_TRUE(IsMutedColor(CellStyle::kIndexed | 240));  // greyscale ramp
  EXPECT_TRUE(IsMutedColor(CellStyle::kTrueColor | 0x808080u));
  EXPECT_FALSE(IsMutedColor(CellStyle::kDefault));
  EXPECT_FALSE(IsMutedColor(CellStyle::kBasic | 5));      // SGR 35, magenta
  EXPECT_FALSE(IsMutedColor(CellStyle::kIndexed | 130));  // 38;5;130, orange
  EXPECT_FALSE(IsMutedColor(CellStyle::kTrueColor | 0xF6E2B7u));
}

TEST(TmuxStyle, DeEmphasisIsRelativeToTheCellBesideIt) {
  CellStyle plain;
  CellStyle dim;
  dim.attrs = kAttrDim;
  CellStyle grey;
  grey.fg = CellStyle::kBasic | 8;
  CellStyle magenta;
  magenta.fg = CellStyle::kBasic | 5;

  EXPECT_TRUE(IsDeEmphasized(dim, plain));
  EXPECT_TRUE(IsDeEmphasized(grey, plain));
  EXPECT_FALSE(IsDeEmphasized(magenta, plain));
  EXPECT_FALSE(IsDeEmphasized(plain, dim));  // coming back OUT of a dim run
  EXPECT_FALSE(IsDeEmphasized(dim, dim));    // a wholly dim line is not ghosted
  EXPECT_FALSE(IsDeEmphasized(grey, grey));
}

TEST(TmuxContext, ASnapshotWithNoStyleInformationBehavesAsBefore) {
  // Hand-built snapshots (and any answer that carried no escapes) decode as
  // all-default, which is contiguous -- so this must not start refusing.
  Snapshot snap;
  snap.cursor_x = 2;
  snap.cursor_y = 0;
  snap.pane_width = 40;
  snap.rows = {
      "ab"
      "\xE4\xB8\xAD"};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->after, "\xE4\xB8\xAD");
}

TEST(TmuxContext, CaretAtColumnZeroComparesAgainstTheWrappedRowAbove) {
  // `before` comes from the row above when the line wrapped, so the "cell to
  // the left" the styles are compared against has to come from there too.
  Snapshot snap;
  snap.cursor_x = 0;
  snap.cursor_y = 1;
  snap.pane_width = 4;
  snap.rows = {"abcd", "efgh"};
  snap.above_row_styles = std::vector<CellStyle>(4, CellStyle{});
  snap.caret_row_styles = std::vector<CellStyle>(4, CellStyle{});
  snap.caret_row_styles[0].attrs = kAttrDim;
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "d");
  EXPECT_EQ(ctx->after, "");
}

TEST(TmuxContext, CaretAtColumnZeroOfAFreshLineHasNoLeftNeighbourToTrust) {
  // No wrapped row above means nothing is adjacent to the caret on the left,
  // so there is no evidence that what is drawn to the right continues it --
  // which is exactly the shape of a composer placeholder starting at column 0.
  Snapshot snap;
  snap.cursor_x = 0;
  snap.cursor_y = 1;
  snap.pane_width = 40;
  snap.rows = {"abcd", "efgh"};
  auto ctx = ExtractContext(snap, 1);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "");
  EXPECT_EQ(ctx->after, "");
}

TEST(TmuxArgs, AsksCapturePaneForEscapeSequences) {
  // Without -e the pane dump is characters only, and ghost text is
  // indistinguishable from the buffer.
  auto args = BuildTmuxArgs("");
  auto cp = std::find(args.begin(), args.end(), "capture-pane");
  ASSERT_NE(cp, args.end());
  EXPECT_NE(std::find(cp, args.end(), "-e"), args.end());
}

// A row longer than prefix_chars was cut by the config, not by the screen.
TEST(TmuxSourceUtil, TruncationByConfigWhenRowIsLongerThanAsked) {
  rime::tmux_detail::Snapshot snap;
  snap.pane_width = 80;
  snap.cursor_x = 16;  // 8 Han characters occupy 16 display columns
  snap.cursor_y = 0;
  snap.rows.push_back("今天天气真的很好");

  auto ctx = rime::tmux_detail::ExtractContext(snap, 4);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("真的很好", ctx->before);
  EXPECT_EQ(4, ctx->before_depth);
  EXPECT_EQ(rime::Truncation::kByConfig, ctx->truncation);
}

// The row began before we ran out of budget: tmux simply cannot see further.
TEST(TmuxSourceUtil, TruncationByScreenWhenRowIsShorterThanAsked) {
  rime::tmux_detail::Snapshot snap;
  snap.pane_width = 80;
  snap.cursor_x = 4;
  snap.cursor_y = 0;
  snap.rows.push_back("今天天气真的很好");

  auto ctx = rime::tmux_detail::ExtractContext(snap, 64);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("今天", ctx->before);
  EXPECT_EQ(2, ctx->before_depth);
  EXPECT_EQ(rime::Truncation::kByScreen, ctx->truncation);
}

// A region ending exactly on the budget is kByScreen for tmux, not kByConfig
// and not kFull: what ended it is the start of the row, which is a screen
// limit. kFull stays unreachable for tmux until step (c) teaches it where the
// user's own text begins.
TEST(TmuxSourceUtil, TruncationExactFitIsByScreen) {
  rime::tmux_detail::Snapshot snap;
  snap.pane_width = 80;
  snap.cursor_x = 4;
  snap.cursor_y = 0;
  snap.rows.push_back("今天");

  auto ctx = rime::tmux_detail::ExtractContext(snap, 2);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("今天", ctx->before);
  EXPECT_EQ(2, ctx->before_depth);
  EXPECT_EQ(rime::Truncation::kByScreen, ctx->truncation);
}

TEST(TmuxSourceUtil, AfterDepthCountsCharactersNotBytes) {
  rime::tmux_detail::Snapshot snap;
  snap.pane_width = 80;
  snap.cursor_x = 2;
  snap.cursor_y = 0;
  snap.rows.push_back("今天");

  auto ctx = rime::tmux_detail::ExtractContext(snap, 8);
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("天", ctx->after);
  EXPECT_EQ(1, ctx->after_depth);
}
