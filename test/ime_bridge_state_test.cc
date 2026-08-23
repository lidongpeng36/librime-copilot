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

#include <unistd.h>

#include <chrono>
#include <thread>

#include <nlohmann/json.hpp>

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

// The greeting is what lets a client on a remote host tell whose IME sits at
// the far end of the tunnel it just dialled. Two laptops sharing one remote
// account is the case that needs it; without it the second one silently drives
// the first one's input method.

TEST(ImeBridgeState, HelloCarriesTheConfiguredHostId) {
  ImeBridgeState s;
  s.config_.host_id = "my-laptop";

  auto hello = s.BuildHello();
  ASSERT_FALSE(hello.empty());
  EXPECT_EQ('\n', hello.back()) << "the greeting must be one JSON Lines record";
  EXPECT_EQ(std::string::npos, hello.substr(0, hello.size() - 1).find('\n'))
      << "and must not contain an embedded newline";

  auto msg = nlohmann::json::parse(hello);
  EXPECT_EQ(1, msg["v"]);
  EXPECT_EQ("rime.ime", msg["ns"]);
  EXPECT_EQ("hello", msg["type"]);
  EXPECT_EQ("my-laptop", msg["data"]["host"]);
}

TEST(ImeBridgeState, HostIdDefaultsToTheShortHostname) {
  ImeBridgeState s;  // host_id left empty -> derive from gethostname()
  auto host = nlohmann::json::parse(s.BuildHello())["data"]["host"].get<std::string>();

  // ssh's %L -- what the remote client compares against -- is the hostname
  // truncated at the first dot. macOS reports "name.local" from gethostname(),
  // so a dot surviving here would make every comparison fail.
  EXPECT_EQ(std::string::npos, host.find('.'))
      << "derived host id must be the short name, got: " << host;

  char expected[256] = {0};
  ASSERT_EQ(0, gethostname(expected, sizeof(expected) - 1));
  std::string want(expected);
  auto dot = want.find('.');
  if (dot != std::string::npos) {
    want.erase(dot);
  }
  EXPECT_EQ(want, host);
}

TEST(ImeBridgeState, HostIdIsStableAcrossCalls) {
  // Cached behind a mutex and read once per connection; a second call must not
  // observe a half-populated cache.
  ImeBridgeState s;
  EXPECT_EQ(s.BuildHello(), s.BuildHello());
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

TEST(ImeBridgeState, SynthesizedResetIsMarkedAsSuch) {
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.ReleaseClientConnection("nvim:1");

  auto q = s.TakePendingActions();
  ASSERT_EQ(1u, q.size());
  EXPECT_TRUE(q.front().synthesized);

  // What the client itself asks for is never marked.
  s.HandleReset("nvim:1", true);
  auto q2 = s.TakePendingActions();
  ASSERT_EQ(1u, q2.size());
  EXPECT_FALSE(q2.front().synthesized);
}

TEST(ImeBridgeState, SynthesizedResetIsSkippedWhenTheClientIsBack) {
  // The tunnel blipped mid-composition: the release synthesized a reset, then
  // the client reconnected before Rime's next key event. Applying the reset now
  // would switch the user to English and erase has_initial for good.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true, true);
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/false);  // initial_state = false
  }

  s.ReleaseClientConnection("nvim:1");  // tunnel dropped
  auto pending = s.TakePendingActions();
  ASSERT_EQ(1u, pending.size());

  s.RetainClientConnection("nvim:1");  // ...and it is already back

  auto result = s.ApplyAction(pending.front(), /*current_ascii=*/false);
  EXPECT_FALSE(result.should_set);

  // The client state survived: a later restore still knows the base, and a
  // later *explicit* reset still knows the initial state.
  ImeBridgePendingAction restore;
  restore.type = ImeBridgePendingAction::kRestore;
  restore.client_key = "nvim:1";
  auto r2 = s.ApplyAction(restore, /*current_ascii=*/true);
  EXPECT_TRUE(r2.should_set);
  EXPECT_FALSE(r2.ascii_mode);  // back to the base captured above
}

TEST(ImeBridgeState, SynthesizedResetStillAppliesWhenTheClientIsGone) {
  // Same setup, but nothing reconnected: the client really died and ascii_mode
  // must go back to where we found it.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true, true);
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/false);  // initial_state = false
  }

  s.ReleaseClientConnection("nvim:1");
  auto pending = s.TakePendingActions();
  ASSERT_EQ(1u, pending.size());

  auto result = s.ApplyAction(pending.front(), /*current_ascii=*/true);
  EXPECT_TRUE(result.should_set);
  EXPECT_FALSE(result.ascii_mode);  // restored to initial_state

  // ...and the state was erased, so a second apply finds nothing.
  auto again = s.ApplyAction(pending.front(), /*current_ascii=*/true);
  EXPECT_FALSE(again.should_set);
}

TEST(ImeBridgeState, ExplicitResetAppliesEvenWithALiveConnection) {
  // A client that sends reset and then reconnects still means it, so the live
  // connection must not suppress a reset the client asked for.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true, true);
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/false);  // initial_state = false
  }

  s.HandleReset("nvim:1", true);
  auto pending = s.TakePendingActions();
  ASSERT_EQ(1u, pending.size());
  ASSERT_FALSE(pending.front().synthesized);

  auto result = s.ApplyAction(pending.front(), /*current_ascii=*/true);
  EXPECT_TRUE(result.should_set);
  EXPECT_FALSE(result.ascii_mode);  // restored to initial_state despite conn_refs_

  auto again = s.ApplyAction(pending.front(), /*current_ascii=*/true);
  EXPECT_FALSE(again.should_set);  // state really was erased
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

// The bridge receives a string and cannot know whether the client had more.
// Reporting any of the four real values would be inventing evidence; kUnknown
// is the honest answer until step (c) puts `want_before` in the greeting.
//
// There is no HandleRegister: HandleContext creates the client's state entry
// and activates it (ime_bridge.cc:501-510), which is what the file's existing
// ContextAutoActivatesSender test already pins.
TEST(ImeBridgeState, ContextDepthIsCountedAndTruncationIsUnknown) {
  ImeBridgeState s;
  s.HandleContext("nvim:1", "今天天气", "好");

  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(4, ctx->before_depth);
  EXPECT_EQ(1, ctx->after_depth);
  EXPECT_EQ(rime::Truncation::kUnknown, ctx->truncation);
}
