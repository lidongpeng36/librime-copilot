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

#include <chrono>
#include <thread>

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

TEST(ImeBridgeState, CleanupRemovesTimedOutClients) {
  // The cleanup pass had no call site at all, so client_timeout_minutes was a
  // dead config knob and per-client state lived forever.
  ImeBridgeState s;
  s.config_.client_timeout_minutes = 0;  // everything is instantly stale
  s.HandleContext("nvim:1", "中", "文");
  ASSERT_TRUE(s.GetActiveContext().has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(2));
  s.CleanupStaleClients();

  EXPECT_FALSE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, CleanupKeepsFreshClients) {
  ImeBridgeState s;  // default timeout: 30 minutes
  s.HandleContext("nvim:1", "中", "文");
  s.CleanupStaleClients();

  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);
}

TEST(ImeBridgeState, ProcessMessageReturnsClientKey) {
  ImeBridgeState s;
  EXPECT_EQ("nvim:1",
            s.ProcessMessage(
                R"({"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
                R"("data":{"action":"ping"}})"));
  EXPECT_EQ("", s.ProcessMessage("not json at all"));
  EXPECT_EQ("", s.ProcessMessage(
                    R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
                    R"("data":{"action":"ping"}})"));
}

TEST(ImeBridgeState, LastConnectionCloseSynthesizesReset) {
  // kill -9 / ssh tunnel drop: the client never sent reset, so the server has
  // to put ascii_mode back itself.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true, true);
  s.TakePendingActions();  // drain the set

  s.ReleaseClientConnection("nvim:1");

  auto q = s.TakePendingActions();
  ASSERT_EQ(1u, q.size());
  EXPECT_EQ(ImeBridgePendingAction::kReset, q.front().type);
  EXPECT_EQ("nvim:1", q.front().client_key);
  EXPECT_TRUE(q.front().restore);
}

TEST(ImeBridgeState, OverlappingConnectionsDoNotSynthesizeReset) {
  // A reconnecting client briefly has two live connections. Losing the old one
  // must not be mistaken for the client going away.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.RetainClientConnection("nvim:1");
  s.TakePendingActions();

  s.ReleaseClientConnection("nvim:1");
  EXPECT_TRUE(s.TakePendingActions().empty());

  s.ReleaseClientConnection("nvim:1");
  EXPECT_EQ(1u, s.TakePendingActions().size());
}

TEST(ImeBridgeState, ReleaseWithoutRetainIsIgnored) {
  ImeBridgeState s;
  s.ReleaseClientConnection("nvim:never-seen");
  EXPECT_TRUE(s.TakePendingActions().empty());
}

TEST(ImeBridgeState, SynthesizedResetIsANoOpAfterACleanExit) {
  // A clean exit already sent reset+unregister, which erased the client state.
  // The synthesized reset then finds nothing and asks for no ascii_mode change.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true, true);
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/false);
  }
  s.HandleUnregister("nvim:1");
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/true);
  }

  s.ReleaseClientConnection("nvim:1");

  auto q = s.TakePendingActions();
  ASSERT_EQ(1u, q.size());
  auto result = s.ApplyAction(q.front(), /*current_ascii=*/true);
  EXPECT_FALSE(result.should_set);
}

TEST(ImeBridgeState, FreshContextIsReturned) {
  ImeBridgeState s;  // default ttl: 60s
  s.HandleContext("nvim:1", "中", "文");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);
}

// The three tests below are wall-clock based because the knob is in seconds.
// Margins are deliberately generous (>= 800ms of slack on every assertion) so
// they stay reliable under ASAN and CI load. Sleeping *longer* than intended
// can only push a context further past its deadline, so the expiry assertions
// cannot flake in the false-failure direction; only the "still fresh"
// assertions need the slack, and they have it.

TEST(ImeBridgeState, StaleContextIsNotReturned) {
  // Wrong context is worse than no context: a stale owner would make the
  // re-ranking filter promote candidates for text the user is not typing into.
  ImeBridgeState s;
  s.config_.context_ttl_seconds = 1;
  s.HandleContext("nvim:1", "中", "文");
  ASSERT_TRUE(s.GetActiveContext().has_value());

  std::this_thread::sleep_for(std::chrono::milliseconds(1200));

  EXPECT_FALSE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, TtlZeroDisablesExpiry) {
  ImeBridgeState s;
  s.config_.context_ttl_seconds = 0;
  s.HandleContext("nvim:1", "中", "文");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  EXPECT_TRUE(s.GetActiveContext().has_value());
}

TEST(ImeBridgeState, RepushingContextRefreshesTheDeadline) {
  // ttl=2s with a 1200ms re-push leaves 800ms of slack on the final check.
  ImeBridgeState s;
  s.config_.context_ttl_seconds = 2;
  s.HandleContext("nvim:1", "中", "文");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  s.HandleContext("nvim:1", "国", "文");
  std::this_thread::sleep_for(std::chrono::milliseconds(1200));
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("国", ctx->before);
}
