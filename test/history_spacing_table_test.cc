#include <gtest/gtest.h>

#include <cstddef>
#include <string>

#include "auto_spacer_util.h"

using rime::DecideHistorySpacing;
using rime::HistorySpaceAction;
using rime::HistorySpaceInput;

namespace {

// The record table carries the field the live input no longer has; the live
// table cannot, so they are two structs rather than one.
struct RecordRow {
  const char* name;
  const char* before;
  bool ascii_mode;
  bool has_input;
  bool previous_was_thru_or_raw;
  HistorySpaceAction expected;
};

struct Row {
  const char* name;
  const char* before;
  bool ascii_mode;
  bool has_input;
  HistorySpaceAction expected;
};

// HISTORY, NOT AN ASSERTION. Captured from the code as it stood before the
// commit `refactor(auto-spacer): one set of spacing predicates, with the diff
// recorded` (Task 6 of the caret-context-consolidation branch), which replaced
// the hand-rolled boundary tests in DecideHistorySpacing with the shared
// NeedSpaceBefore the surrounding path already used, and deleted
// `previous_was_thru_or_raw` along with the commit-history type-tag check that
// fed it.
//
// It is kept beside kTableAfterConsolidation because the PAIR is the record of
// what that substitution changed -- nine of these twenty-eight rows moved, and
// a row that moves is a decision. Nothing asserts these expectations any more:
// a table kept as a record must not be kept as a failing assertion. The one
// test that reads it (RecordsWhichRowsTheConsolidationMoved) pins that the two
// tables still describe the same twenty-eight inputs in the same order, and
// that exactly nine of them disagree.
const RecordRow kTableBeforeConsolidation[] = {
    {"english then chinese input adds a space", "hello", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"but not when the previous commit was continuous english", "hello", false, false, true,
     HistorySpaceAction::kNone},
    {"ascii punctuation also opens a boundary", "hello,", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"backtick is excluded from that", "hello`", false, false, false, HistorySpaceAction::kNone},
    {"a thru previous does not suppress the punctuation case", "hello,", false, false, true,
     HistorySpaceAction::kPrependSpaceToInput},
    {"chinese then ascii typing commits with a space", "你好", true, false, false,
     HistorySpaceAction::kCommitWithSpace},
    {"chinese then chinese does nothing", "你好", false, false, false, HistorySpaceAction::kNone},
    {"a composition in progress suppresses everything", "hello", false, true, false,
     HistorySpaceAction::kNone},
    {"a lone space in history suppresses everything", " ", false, false, false,
     HistorySpaceAction::kNone},
    {"empty history does nothing", "", false, false, false, HistorySpaceAction::kNone},
    {"chinese punctuation before does not open a boundary", "。", false, false, false,
     HistorySpaceAction::kNone},
    {"ascii mode after ascii does nothing", "hello", true, false, false, HistorySpaceAction::kNone},

    // --- the punctuation set, sampled on both sides of the Task 6 boundary ---
    //
    // The predicate was `IsAsciiPunctuationCode(c) && c != '`'` -- about 31
    // characters. The shared predicate that replaced it, NeedSpaceBefore(before,
    // false), accepts eight: . , ! ? ) ] } > . So `.` below agreed and `;` `:`
    // `(` `-` did not, and those four are the rows that moved. Sampling only
    // `,` (the row above) would have let that substitution land green.
    {"'.' opens a boundary, and is in the eight-char set too", "hello.", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"';' opens a boundary today, and is NOT in the eight-char set", "hello;", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"':' opens a boundary today, and is NOT in the eight-char set", "hello:", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"'(' opens a boundary today -- a LEFT bracket, never a right-punct", "hello(", false, false,
     false, HistorySpaceAction::kPrependSpaceToInput},
    {"'-' opens a boundary today, and is NOT in the eight-char set", "hello-", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},

    // --- digits, which IsAsciiAlphaNumCode covers and no row above reached ---
    {"a trailing digit is alphanumeric, so it opens a boundary", "abc1", false, false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"and a thru previous suppresses a trailing digit, as it does a letter", "abc1", false, false,
     true, HistorySpaceAction::kNone},

    // --- ascii_mode against everything the rows above only tested with it off ---
    {"ascii mode blocks the agreeing-punctuation branch", "hello,", true, false, false,
     HistorySpaceAction::kNone},
    {"ascii mode blocks the disagreeing-punctuation branch too", "hello;", true, false, false,
     HistorySpaceAction::kNone},
    {"ascii mode after a backtick still does nothing", "hello`", true, false, false,
     HistorySpaceAction::kNone},
    {"ascii mode after a digit does nothing", "abc1", true, false, false,
     HistorySpaceAction::kNone},
    {"a thru previous does NOT suppress the ascii commit", "你好", true, false, true,
     HistorySpaceAction::kCommitWithSpace},
    {"a composition in progress suppresses the ascii commit too", "你好", true, true, false,
     HistorySpaceAction::kNone},
    {"a lone space suppresses the ascii commit too", " ", true, false, false,
     HistorySpaceAction::kNone},
    // The two below reach the ascii branch for the same reason -- the old test
    // was `LastAsciiCharCode(before) < 0`, which is -1 for a non-ASCII last
    // character AND for the empty string -- but they are NOT alike in
    // reachability, and an earlier draft of this comment wrongly said they
    // were. See HistorySpacingLivePath below for the row that matters.
    {"chinese punctuation in ascii mode commits with a space", "。", true, false, false,
     HistorySpaceAction::kCommitWithSpace},
    {"empty history in ascii mode commits with a space", "", true, false, false,
     HistorySpaceAction::kCommitWithSpace},
};

// THE LIVE ASSERTION. Same twenty-eight inputs, in the same order, decided by
// DecideHistorySpacing as it stands now -- one set of spacing predicates,
// shared with the surrounding path. Rows marked MOVED disagree with the record
// above; each carries the reason it moved, because a row that changed silently
// is the failure this pair exists to prevent.
const Row kTableAfterConsolidation[] = {
    {"english then chinese input adds a space", "hello", false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    // MOVED (kNone): the type tag is gone, so the same `before` decides the
    // same way however the previous text was committed.
    {"a thru previous no longer suppresses it -- the text decides, not the tag", "hello", false,
     false, HistorySpaceAction::kPrependSpaceToInput},
    {"ascii punctuation also opens a boundary", "hello,", false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"backtick is excluded from that", "hello`", false, false, HistorySpaceAction::kNone},
    // Once the tag is gone this row's input is the one two above it. It is kept
    // so the two tables stay in lockstep, row for row.
    {"a thru previous does not suppress the punctuation case", "hello,", false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    {"chinese then ascii typing commits with a space", "你好", true, false,
     HistorySpaceAction::kCommitWithSpace},
    {"chinese then chinese does nothing", "你好", false, false, HistorySpaceAction::kNone},
    {"a composition in progress suppresses everything", "hello", false, true,
     HistorySpaceAction::kNone},
    {"a lone space in history suppresses everything", " ", false, false, HistorySpaceAction::kNone},
    {"empty history does nothing", "", false, false, HistorySpaceAction::kNone},
    {"chinese punctuation before does not open a boundary", "。", false, false,
     HistorySpaceAction::kNone},
    {"ascii mode after ascii does nothing", "hello", true, false, HistorySpaceAction::kNone},

    // --- the punctuation set: only NeedSpaceBefore's eight marks survive ---
    {"'.' opens a boundary -- it is a right-hand punctuation mark", "hello.", false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    // MOVED x4 (kPrependSpaceToInput): `;` `:` `(` `-` are not right-hand
    // punctuation, so no space belongs after them before Chinese.
    {"';' no longer opens a boundary", "hello;", false, false, HistorySpaceAction::kNone},
    {"':' no longer opens a boundary", "hello:", false, false, HistorySpaceAction::kNone},
    {"'(' no longer opens a boundary -- a LEFT bracket never should have", "hello(", false, false,
     HistorySpaceAction::kNone},
    {"'-' no longer opens a boundary -- a trailing hyphen joins, it does not end", "hello-", false,
     false, HistorySpaceAction::kNone},

    // --- digits, which IsAsciiAlphaNumCode covers ---
    {"a trailing digit is alphanumeric, so it opens a boundary", "abc1", false, false,
     HistorySpaceAction::kPrependSpaceToInput},
    // MOVED (kNone): same reason as the second row.
    {"a thru previous no longer suppresses a trailing digit either", "abc1", false, false,
     HistorySpaceAction::kPrependSpaceToInput},

    // --- ascii_mode, now decided by NeedSpaceBefore(before, true) ---
    // MOVED (kNone): ',' is in IsAsciiRightPunctCodeForAsciiInput, so Latin
    // typed after a comma gets the space English typography wants. This is the
    // rule the surrounding path already applied.
    {"ascii mode after a comma now commits with a space", "hello,", true, false,
     HistorySpaceAction::kCommitWithSpace},
    {"ascii mode after ';' still does nothing", "hello;", true, false, HistorySpaceAction::kNone},
    {"ascii mode after a backtick still does nothing", "hello`", true, false,
     HistorySpaceAction::kNone},
    {"ascii mode after a digit does nothing", "abc1", true, false, HistorySpaceAction::kNone},
    {"a thru previous does NOT suppress the ascii commit", "你好", true, false,
     HistorySpaceAction::kCommitWithSpace},
    {"a composition in progress suppresses the ascii commit too", "你好", true, true,
     HistorySpaceAction::kNone},
    {"a lone space suppresses the ascii commit too", " ", true, false, HistorySpaceAction::kNone},
    // MOVED x2 (kCommitWithSpace): the old test was `LastAsciiCharCode < 0`,
    // true of Chinese punctuation and of the empty string alike;
    // NeedSpaceBefore excludes both by name.
    //
    // These two rows differ in reachability, and the difference is easy to get
    // backwards -- it was, in review.
    //
    // THIS ROW'S MECHANISM IS LIVE. The guard above this block is
    // `IsChinesePunctuation(latest_text)` (auto_spacer.cc:644), and that tests
    // the WHOLE STRING: it starts with `if (!IsSingleUtf8Char(s)) return
    // false`. So only a `before` that is exactly one Chinese punctuation mark
    // is turned away. `你好。` is not, falls through, and reaches this branch
    // in ascii mode -- old kCommitWithSpace, new kNone, in production.
    // HistorySpacingLivePath.ChinesePunctuationAfterHanIsReachable pins that
    // form, because a row nobody can reach is a row someone will revert.
    {"chinese punctuation in ascii mode no longer commits a space", "。", true, false,
     HistorySpaceAction::kNone},
    // THIS ROW IS GENUINELY DEAD, by a different argument -- not "the guard
    // usually catches it" but "the guard is the same predicate". The guard is
    // `latest_text.empty()` (auto_spacer.cc:639) on the very string that
    // becomes `in.before`, so no input satisfies one and not the other. Unlike
    // IsChinesePunctuation, it is not a whole-string proxy for a last-character
    // test, so it has no longer-string escape.
    {"empty history in ascii mode no longer commits a space", "", true, false,
     HistorySpaceAction::kNone},
};

constexpr size_t kRowsThatMoved = 9;

}  // namespace

TEST(HistorySpacingTable, MatchesTheBehaviourAfterConsolidation) {
  for (const Row& row : kTableAfterConsolidation) {
    HistorySpaceInput in;
    in.before = row.before;
    in.ascii_mode = row.ascii_mode;
    in.has_input = row.has_input;
    EXPECT_EQ(DecideHistorySpacing(in), row.expected) << "row: " << row.name;
  }
}

// The record table's only reader. It asserts nothing about the OLD decisions --
// those are history -- but it does pin that the two tables cover the same
// inputs in the same order, and that the consolidation moved exactly nine rows.
// Without this the record could drift out of correspondence with the live
// table, and the pair would stop being a record of anything.
TEST(HistorySpacingTable, RecordsWhichRowsTheConsolidationMoved) {
  const size_t n = sizeof(kTableBeforeConsolidation) / sizeof(kTableBeforeConsolidation[0]);
  ASSERT_EQ(n, sizeof(kTableAfterConsolidation) / sizeof(kTableAfterConsolidation[0]));

  size_t moved = 0;
  for (size_t i = 0; i < n; ++i) {
    const RecordRow& was = kTableBeforeConsolidation[i];
    const Row& now = kTableAfterConsolidation[i];
    EXPECT_EQ(std::string(was.before), std::string(now.before)) << "row " << i << ": " << was.name;
    EXPECT_EQ(was.ascii_mode, now.ascii_mode) << "row " << i << ": " << was.name;
    EXPECT_EQ(was.has_input, now.has_input) << "row " << i << ": " << was.name;
    if (was.expected != now.expected) {
      ++moved;
    }
  }
  EXPECT_EQ(moved, kRowsThatMoved);
}

// The reachable form of the `。` row above.
//
// The paired tables sample `before == "。"`, and that exact one-character
// string never reaches DecideHistorySpacing in production: ProcessWithCommitHistory
// returns kNoop on it at auto_spacer.cc:644. Read carelessly that makes the row
// look like dead weight -- and a row believed dead is a row someone reverts.
// It is not dead. The guard tests the WHOLE string for being a single Chinese
// punctuation character, so any longer `before` that merely ENDS in one walks
// straight past it and into the branch this consolidation changed.
//
// Kept out of the two tables deliberately: those hold the same 28 inputs in
// lockstep and their nine-row diff is the record of this commit. This is an
// additional live case, not a 29th row of that record.
TEST(HistorySpacingLivePath, ChinesePunctuationAfterHanIsReachable) {
  // The mechanism, pinned first: this is what lets the input through.
  EXPECT_TRUE(rime::auto_spacer_detail::IsChinesePunctuation("。"));
  EXPECT_FALSE(rime::auto_spacer_detail::IsChinesePunctuation("你好。"));

  HistorySpaceInput in;
  in.before = "你好。";

  // The row that moved: kCommitWithSpace before this commit, in production.
  // `你好。 word` is what that produced; `你好。word` is what belongs.
  in.ascii_mode = true;
  EXPECT_EQ(DecideHistorySpacing(in), HistorySpaceAction::kNone);

  // The non-ascii direction did not move, and is asserted so the pair reads as
  // one decision rather than one measurement.
  in.ascii_mode = false;
  EXPECT_EQ(DecideHistorySpacing(in), HistorySpaceAction::kNone);
}
