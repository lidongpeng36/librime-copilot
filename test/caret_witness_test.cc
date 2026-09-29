// Which keys end the assumption that our own last commit still sits
// immediately left of the caret.
//
// That assumption is what `LocalCommitStillAtCaret` vouches for and what
// `ResolveBoundaryBefore` acts on: when the screen disagrees with
// commit_history, the screen is treated as lagging and the history wins
// (auto_spacer_util.h). It is only sound while the caret has not moved for
// some other reason, so every key that moves it has to clear the witness.
//
// The set used to be arrows + Tab + the readline bindings + the delete keys,
// and the miss was reachable: press Enter in codex's composer and the caret
// lands on a fresh, indented row, so the scrape correctly reports blanks
// before it -- which reads as "the screen has not caught up", and the previous
// line's Chinese earns the digit at the start of the new line a leading space.
// Reproduced 2026-09-29 in WezTerm; invisible in Alacritty only because winit
// drops the commit (see CLAUDE.md).

#include <gtest/gtest.h>
#include <rime/key_event.h>

#include "auto_spacer.h"

using rime::CaretLeavesLastCommit;
using rime::KeyEvent;

namespace {

KeyEvent Key(int keycode, int modifier = 0) { return KeyEvent(keycode, modifier); }

}  // namespace

TEST(CaretLeavesLastCommit, NewlineKeys) {
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Return)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_KP_Enter)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_ISO_Enter)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Linefeed)));
  // Shift+Enter is how a TUI composer usually takes a newline, and Alt+Enter
  // is how some others do: the modifier must not exempt the key.
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Return, kShiftMask)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Return, kAltMask)));
  // Ctrl+J is the same character a terminal sends for a newline.
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_j, kControlMask)));
}

TEST(CaretLeavesLastCommit, CaretJumpsTheArrowRangeMisses) {
  // XK_Home (0xff50) sits just below XK_Left (0xff51) and End/Page_Up/Page_Down
  // just above XK_Down (0xff54), so the `Left <= k <= Down` range never covered
  // any of them.
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Home)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_End)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Page_Up)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Page_Down)));
}

TEST(CaretLeavesLastCommit, WhatWasAlreadyCovered) {
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Left)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Right)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Up)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Down)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Tab)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_BackSpace)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_Delete)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_a, kControlMask)));
  EXPECT_TRUE(CaretLeavesLastCommit(Key(XK_e, kControlMask)));
}

TEST(CaretLeavesLastCommit, OrdinaryTypingKeepsTheWitness) {
  // The witness exists for exactly these: the next keystroke after a commit,
  // typed where the commit landed.
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_a)));
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_z)));
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_1)));
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_space)));
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_comma)));
  // A bare letter with Shift is still typing.
  EXPECT_FALSE(CaretLeavesLastCommit(Key(XK_A, kShiftMask)));
}
