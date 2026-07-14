// Layer 1: unit tests for the socket-free ImeBridge state machine (ownership,
// context storage, ascii-mode ApplyAction). No socket, no thread.
//
// NOTE: <gtest/gtest.h> is included before "ime_bridge.h" (deviating from the
// brief's literal include order) because ime_bridge.h transitively includes
// <rime/processor.h> -> rime_api.h, which #defines a bare `Bool` macro; if
// that macro is active when gtest-param-test.h is parsed, it collides with
// gtest's `Bool()` function template. See test/commit_text_test.cc for the
// same ordering used to avoid this pre-existing macro collision.
#include <gtest/gtest.h>

#include "ime_bridge.h"

using rime::ImeBridgePendingAction;
using rime::ImeBridgeState;

TEST(ImeBridgeState, NoActiveClientReturnsNullopt) {
  ImeBridgeState s;
  EXPECT_FALSE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, ActiveClientContextIsReturned) {
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.HandleContext("nvim:1", "中", "文");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);
  EXPECT_EQ("文", ctx->after);
  EXPECT_EQ("nvim:1", ctx->client_key);
}

TEST(ImeBridgeState, ContextAutoActivatesSender) {
  // Pushing context makes that client the active owner (no explicit activate).
  ImeBridgeState s;
  s.HandleContext("nvim:1", "中", "文");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("nvim:1", ctx->client_key);
  EXPECT_EQ("中", ctx->before);
}

TEST(ImeBridgeState, ContextFromAnotherClientTakesOwnership) {
  // Two nvim instances: whoever last pushed context owns the context.
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.HandleContext("nvim:1", "A", "");
  s.HandleContext("nvim:2", "B", "");  // nvim:2 typed most recently
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("nvim:2", ctx->client_key);
  EXPECT_EQ("B", ctx->before);
}

TEST(ImeBridgeState, DeactivateClearsOwnership) {
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.HandleContext("nvim:1", "中", "");
  ASSERT_TRUE(s.GetActiveContext().has_value());
  s.HandleDeactivate("nvim:1");
  EXPECT_FALSE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, LastActivateWins) {
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.HandleContext("nvim:1", "A", "");
  s.HandleActivate("nvim:2");
  s.HandleContext("nvim:2", "B", "");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("B", ctx->before);
  EXPECT_EQ("nvim:2", ctx->client_key);
}

TEST(ImeBridgeState, ClearContextOnActiveClearsOwnership) {
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.HandleContext("nvim:1", "中", "");
  s.HandleClearContext("nvim:1");
  EXPECT_FALSE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, ProcessMessageParsesContext) {
  ImeBridgeState s;
  s.HandleActivate("nvim:1");
  s.ProcessMessage(R"({"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
                   R"("data":{"action":"context","before":"中","after":"文"}})");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);
  EXPECT_EQ("文", ctx->after);
}

TEST(ImeBridgeState, ApplyActionSetThenRestoreReturnsToBase) {
  ImeBridgeState s;
  ImeBridgePendingAction set;
  set.type = ImeBridgePendingAction::kSet;
  set.client_key = "nvim:1";
  set.ascii = true;
  set.stack = true;
  auto r1 = s.ApplyAction(set, /*current_ascii=*/false);  // base captured = false
  EXPECT_TRUE(r1.should_set);
  EXPECT_TRUE(r1.ascii_mode);

  ImeBridgePendingAction restore;
  restore.type = ImeBridgePendingAction::kRestore;
  restore.client_key = "nvim:1";
  auto r2 = s.ApplyAction(restore, /*current_ascii=*/true);
  EXPECT_TRUE(r2.should_set);
  EXPECT_FALSE(r2.ascii_mode);  // restored to base (false)
}
