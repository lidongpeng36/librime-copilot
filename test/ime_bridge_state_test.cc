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
  s.ProcessMessage(R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
                   R"("data":{"action":"context","before":"中","after":"文"}})");
  auto ctx = s.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);
  EXPECT_EQ("文", ctx->after);
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
                R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
                R"("data":{"action":"ping"}})"));
  EXPECT_EQ("", s.ProcessMessage("not json at all"));
  // The old version, now rejected outright by the same exact-equality check
  // RejectsTheOldVersionOutright pins below.
  EXPECT_EQ("", s.ProcessMessage(
                    R"({"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
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
  EXPECT_EQ(2, msg["v"]);
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
  s.HandleSet("nvim:1", true);
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
  s.HandleSet("nvim:1", true);
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
  s.HandleSet("nvim:1", true);
  for (auto q = s.TakePendingActions(); !q.empty(); q.pop()) {
    s.ApplyAction(q.front(), /*current_ascii=*/false);  // initial_state = false
  }

  s.ReleaseClientConnection("nvim:1");  // tunnel dropped
  auto pending = s.TakePendingActions();
  ASSERT_EQ(1u, pending.size());

  s.RetainClientConnection("nvim:1");  // ...and it is already back

  auto result = s.ApplyAction(pending.front(), /*current_ascii=*/false);
  EXPECT_FALSE(result.should_set);

  // The client state survived: a later *explicit* reset still knows the
  // initial state.
  ImeBridgePendingAction reset;
  reset.type = ImeBridgePendingAction::kReset;
  reset.client_key = "nvim:1";
  reset.restore = true;
  auto r2 = s.ApplyAction(reset, /*current_ascii=*/true);
  EXPECT_TRUE(r2.should_set);
  EXPECT_FALSE(r2.ascii_mode);  // back to initial_state (false, captured above)
}

TEST(ImeBridgeState, SynthesizedResetStillAppliesWhenTheClientIsGone) {
  // Same setup, but nothing reconnected: the client really died and ascii_mode
  // must go back to where we found it.
  ImeBridgeState s;
  s.RetainClientConnection("nvim:1");
  s.HandleSet("nvim:1", true);
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
  s.HandleSet("nvim:1", true);
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

// The other half of a cross-repo contract. The reporter that emits this exact
// message lives in rime-copilot-clients (scripts/report.sh), and its
// test/report_test.py pins the same string from the other side. Neither can
// test the other -- this handler lives behind a real Rime engine -- so the two
// goldens are kept in step by hand, the way MakeKey and MakeClientKey are
// (src/context_memory.h:45-59).
//
// Bumping kProtocolVersion is a coordinated change across both repos. A
// mismatch is inert, never wrong: ProcessMessage logs a warning and ignores
// the message.
namespace {
std::string IdentityMessage(const char* pane, const char* command,
                            const char* host = nullptr) {
  std::string data =
      std::string(R"({"socket":"default","pane":")") + pane +
      R"(","command":")" + command + R"(")";
  if (host) {
    data += std::string(R"(,"host":")") + host + R"(")";
  }
  return R"({"v":2,"ns":"rime.ime","type":"identity","data":)" + data + "}}";
}
}  // namespace

// The cross-repo golden. These two strings are exactly what
// rime-copilot-clients' scripts/report.sh emits -- the first in local mode,
// the second in remote mode -- and its test/report_test.py pins the same two
// from the other side. Neither repo can test the other: this handler lives
// behind a real Rime engine.
//
// Written out in full rather than built by IdentityMessage() ON PURPOSE.
// Every other test in this file uses that helper, so a change to the wire
// format changes the helper and leaves all of them green. These two cannot be
// satisfied that way. If one fails, either the format changed -- in which case
// the other repo needs the same change and kProtocolVersion probably needs a
// coordinated bump -- or somebody reformatted the literal, which is the same
// thing wearing a different hat.
//
// The local string is not invented: it was captured from the installed
// reporter on 2026-08-31.
TEST(ImeBridgeIdentity, TheLocalReportersExactMessageStillParses) {
  rime::ImeBridgeState state;
  state.ProcessMessage(
      R"({"v":2,"ns":"rime.ime","type":"identity","data":{"socket":"/private/tmp/tmux-501/default","pane":"%2","command":"zsh"}})");
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->socket, "/private/tmp/tmux-501/default");
  EXPECT_EQ(id->pane_id, "%2");
  EXPECT_EQ(id->command, "zsh");
  EXPECT_EQ(id->host, "");
}

TEST(ImeBridgeIdentity, TheRemoteReportersExactMessageStillParses) {
  rime::ImeBridgeState state;
  state.SetHostIdForTest("Mac-Mini");
  state.ProcessMessage(
      R"({"v":2,"ns":"rime.ime","type":"identity","data":{"expect":"Mac-Mini","host":"devbox","socket":"/tmp/tmux-1000/default","pane":"%2","command":"zsh"}})");
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->host, "devbox");
  EXPECT_EQ(id->pane_id, "%2");
  EXPECT_EQ(id->command, "zsh");
}

TEST(ImeBridgeIdentity, UpdatesTheIdentityCell) {
  rime::ImeBridgeState state;
  state.ProcessMessage(IdentityMessage("%7", "claude"));
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->socket, "default");
  EXPECT_EQ(id->pane_id, "%7");
  EXPECT_EQ(id->command, "claude");
}

TEST(ImeBridgeIdentity, LastPushWins) {
  rime::ImeBridgeState state;
  state.ProcessMessage(IdentityMessage("%7", "claude"));
  state.ProcessMessage(IdentityMessage("%3", "zsh"));
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->pane_id, "%3");
}

// The load-bearing one. The reporter connects and disconnects on EVERY pane
// switch. If an identity message registered a client, every one of those
// disconnects would synthesize a reset and flip ascii_mode on a machine nobody
// is typing on. ProcessMessage returning "" is exactly what stops
// HandleConnection from taking a connection refcount (ime_bridge.cc:237-241).
TEST(ImeBridgeIdentity, RegistersNoClient) {
  rime::ImeBridgeState state;
  const std::string key = state.ProcessMessage(IdentityMessage("%7", "claude"));
  EXPECT_EQ(key, "");
  auto pending = state.TakePendingActions();
  EXPECT_TRUE(pending.empty());
}

TEST(ImeBridgeIdentity, MalformedIdentityLeavesTheCellAlone) {
  rime::ImeBridgeState state;
  state.ProcessMessage(IdentityMessage("%7", "claude"));
  state.ProcessMessage(R"({"v":2,"ns":"rime.ime","type":"identity","data":{}})");
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->pane_id, "%7");
}

// An ascii message must keep behaving exactly as before.
TEST(ImeBridgeIdentity, AsciiMessagesStillRegisterTheirClient) {
  rime::ImeBridgeState state;
  const std::string key = state.ProcessMessage(
      R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
      R"("data":{"action":"set","ascii":true}})");
  EXPECT_EQ(key, "nvim:1");
}

TEST(ImeBridgeIdentity, CarriesTheHostFieldWhenPresent) {
  rime::ImeBridgeState state;
  state.ProcessMessage(IdentityMessage("%2", "zsh", "devbox"));
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->host, "devbox");
  EXPECT_EQ(id->pane_id, "%2");
}

// 本机模式下的上报器（rime-copilot-clients 的 scripts/report.sh）本就不发
// host 这个字段——它没有远端身份要区分。空字符串必须保持「本机」的含义，
// 这是 spec 的 Degradation 表里允许发生的一格。
TEST(ImeBridgeIdentity, AbsentHostFieldMeansThisMachine) {
  rime::ImeBridgeState state;
  state.ProcessMessage(IdentityMessage("%2", "zsh"));
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->host, "");
}

namespace {
std::string IdentityMessageWithExpect(const char* pane, const char* host,
                                      const char* expect) {
  return std::string(R"({"v":2,"ns":"rime.ime","type":"identity","data":{)") +
         R"("socket":"default","pane":")" + pane + R"(","command":"zsh")" +
         R"(,"host":")" + host + R"(","expect":")" + expect + R"("}})";
}
}  // namespace

// 远端的 endpoint 缓存按 uid 存，两台笔记本共用一个文件；而
// RemoteForward 127.0.0.1:0 每次重连拿的是临时端口，重连后那个端口
// 可能已经属于另一条隧道。让消息自带收件人，收错的一方直接丢，
// 而不是把别人的 pane 记进自己的表。
TEST(ImeBridgeIdentity, DropsAMessageAddressedToAnotherMachine) {
  rime::ImeBridgeState state;
  state.SetHostIdForTest("Mac-Mini");
  state.ProcessMessage(IdentityMessageWithExpect("%2", "devbox", "SomeOtherLaptop"));
  EXPECT_FALSE(state.GetPushedIdentity().has_value());
}

TEST(ImeBridgeIdentity, AcceptsAMessageAddressedToThisMachine) {
  rime::ImeBridgeState state;
  state.SetHostIdForTest("Mac-Mini");
  state.ProcessMessage(IdentityMessageWithExpect("%2", "devbox", "Mac-Mini"));
  auto id = state.GetPushedIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->host, "devbox");
}

// 本机上报器不发这个字段，它必须继续工作。
TEST(ImeBridgeIdentity, AnAbsentExpectFieldIsNotAMismatch) {
  rime::ImeBridgeState state;
  state.SetHostIdForTest("Mac-Mini");
  state.ProcessMessage(IdentityMessage("%2", "zsh"));
  EXPECT_TRUE(state.GetPushedIdentity().has_value());
}

// ---------------------------------------------------------------------------
// applied_mode_writes(): the seam per-context memory uses to tell "the user's
// mode in this pane" from "a mode this queue applied here by accident".
// ---------------------------------------------------------------------------

TEST(ImeBridgeModeWrites, CountsEveryAppliedWriteAndNothingElse) {
  ImeBridgeState s;
  EXPECT_EQ(s.applied_mode_writes(), 0u);

  ImeBridgePendingAction set;
  set.type = ImeBridgePendingAction::kSet;
  set.client_key = "nvim:1";
  set.ascii = true;
  ASSERT_TRUE(s.ApplyAction(set, /*current_ascii=*/false).should_set);
  EXPECT_EQ(s.applied_mode_writes(), 1u);

  // A reset that finds the client's captured initial state writes the mode
  // too, and lands on whatever pane the keystroke is in just as a set does --
  // so it must count.
  ImeBridgePendingAction reset;
  reset.type = ImeBridgePendingAction::kReset;
  reset.client_key = "nvim:1";
  reset.restore = true;
  ASSERT_TRUE(s.ApplyAction(reset, /*current_ascii=*/true).should_set);
  EXPECT_EQ(s.applied_mode_writes(), 2u);

  // An action that resolves to no write must NOT count: a tail that saw the
  // counter move refuses to record, so an over-count silently costs the
  // feature the keystrokes it exists to remember. The reset above already
  // erased the client's state, so re-applying the same action finds nothing.
  ASSERT_FALSE(s.ApplyAction(reset, /*current_ascii=*/true).should_set);
  EXPECT_EQ(s.applied_mode_writes(), 2u);

  ImeBridgePendingAction unknown_unregister;
  unknown_unregister.type = ImeBridgePendingAction::kUnregister;
  unknown_unregister.client_key = "nobody:0";
  ASSERT_FALSE(s.ApplyAction(unknown_unregister, /*current_ascii=*/true).should_set);
  EXPECT_EQ(s.applied_mode_writes(), 2u);
}

// Monotonic and never reset: the memory feature samples it at the head of a
// key event and compares at the tail, so any wrap-to-zero or per-event reset
// would read as "the bridge did nothing" on exactly the event it did something.
TEST(ImeBridgeModeWrites, IsMonotonicAcrossClientsAndResets) {
  ImeBridgeState s;
  ImeBridgePendingAction a;
  a.type = ImeBridgePendingAction::kSet;
  a.client_key = "nvim:1";
  a.ascii = true;
  s.ApplyAction(a, /*current_ascii=*/false);

  ImeBridgePendingAction reset;
  reset.type = ImeBridgePendingAction::kReset;
  reset.client_key = "nvim:1";
  reset.restore = true;
  ASSERT_TRUE(s.ApplyAction(reset, /*current_ascii=*/true).should_set);

  ImeBridgePendingAction b;
  b.type = ImeBridgePendingAction::kSet;
  b.client_key = "nvim:2";
  b.ascii = false;
  s.ApplyAction(b, /*current_ascii=*/true);

  EXPECT_EQ(s.applied_mode_writes(), 3u);
}

// The insert-mode bit, and the other half of a second cross-repo golden.
//
// Everything below pins ApplyAction's kSet / kReset / kEnterInsert /
// kLeaveInsert / kUnregister semantics from this side. The other side is
// rime-copilot-clients' test/verify_spec.lua -- its `new_model` /
// `model_apply` pair is a hand-written state machine mirroring exactly these
// transitions, and its docstring names this test group back. That is the same
// arrangement as the identity-message golden further up this file, and it
// exists for the same reason: neither repo can test the other, because this
// handler lives behind a real Rime engine that the Lua side has no way to
// stand up.
//
// So a change to any transition here is a change over there too. If the two
// drift, the failure is silent in the worst possible direction: verify_spec
// goes on passing against a bridge that no longer exists, and it is the only
// thing on the client side that checks state rather than bytes.
//
// The Lua model keeps one state rather than a map keyed by `app:instance`,
// because each scenario runs a single client. That is a simplification of the
// per-client keying below, not a disagreement with it.

// 离开 insert：把当时的真实状态存成 bit，然后强制英文。
TEST(ImeBridgeInsertBit, LeaveInsertSavesCurrentAndForcesAscii) {
  ImeBridgeState s;
  ImeBridgePendingAction leave;
  leave.type = ImeBridgePendingAction::kLeaveInsert;
  leave.client_key = "nvim:1";
  auto r = s.ApplyAction(leave, /*current_ascii=*/false);  // 用户当时在中文
  EXPECT_TRUE(r.should_set);
  EXPECT_TRUE(r.ascii_mode);  // 普通模式恒为英文
}

// 没有 bit 时什么都不做 —— 这是「第一次进 insert 沿用这台机器原有状态」的实现依据。
// 注意：这里 client_states_ 是空的，所以只覆盖 guard 里 `it == end()` 这一支 ——
// 生产路径上 HandleEnterInsert 会先 operator[] 自动建出条目，那一支永远不会命中；
// 真正在生产路径上生效的是 `!has_insert_state`，那一支由下面
// EnterInsertWritesNothingWhenTheEntryExistsWithoutABit 覆盖。
TEST(ImeBridgeInsertBit, EnterInsertWithNoSavedBitDoesNothing) {
  ImeBridgeState s;
  ImeBridgePendingAction enter;
  enter.type = ImeBridgePendingAction::kEnterInsert;
  enter.client_key = "nvim:1";
  auto r = s.ApplyAction(enter, /*current_ascii=*/true);
  EXPECT_FALSE(r.should_set);  // 不是「设成某值」，是「一个字都不写」
}

// The guard's live clause. HandleEnterInsert auto-vivifies the client entry,
// so on the real path `find()` never returns end() -- what actually stops the
// write is has_insert_state being false. The sibling test above cannot reach
// this clause because it calls ApplyAction with an empty map.
TEST(ImeBridgeInsertBit, EnterInsertWritesNothingWhenTheEntryExistsWithoutABit) {
  ImeBridgeState s;
  s.HandleEnterInsert("nvim:1");
  auto actions = s.TakePendingActions();
  ASSERT_EQ(actions.size(), 1u);
  auto r = s.ApplyAction(actions.front(), /*current_ascii=*/false);
  EXPECT_FALSE(r.should_set) << "an entry with no bit must still write nothing";
}

// 存了什么就还什么。
TEST(ImeBridgeInsertBit, EnterInsertRestoresTheSavedBit) {
  ImeBridgeState s;
  ImeBridgePendingAction leave;
  leave.type = ImeBridgePendingAction::kLeaveInsert;
  leave.client_key = "nvim:1";
  s.ApplyAction(leave, /*current_ascii=*/false);

  ImeBridgePendingAction enter;
  enter.type = ImeBridgePendingAction::kEnterInsert;
  enter.client_key = "nvim:1";
  auto r = s.ApplyAction(enter, /*current_ascii=*/true);
  EXPECT_TRUE(r.should_set);
  EXPECT_FALSE(r.ascii_mode);  // 回到中文
}

// 手动切成英文之后离开再回来，仍是英文 —— spec 的 G2。
TEST(ImeBridgeInsertBit, AManualSwitchInsideInsertSurvivesTheRoundTrip) {
  ImeBridgeState s;
  ImeBridgePendingAction leave;
  leave.type = ImeBridgePendingAction::kLeaveInsert;
  leave.client_key = "nvim:1";
  s.ApplyAction(leave, /*current_ascii=*/true);  // 用户在 insert 里手动切了英文

  ImeBridgePendingAction enter;
  enter.type = ImeBridgePendingAction::kEnterInsert;
  enter.client_key = "nvim:1";
  auto r = s.ApplyAction(enter, /*current_ascii=*/true);
  EXPECT_TRUE(r.should_set);
  EXPECT_TRUE(r.ascii_mode);
}

// F4：由 ApplyAction 建出来的状态，last_active 曾是零值，于是「出生即过期」。
TEST(ImeBridgeInsertBit, StateCreatedByApplyActionIsNotBornStale) {
  ImeBridgeState s;  // 默认超时 30 分钟
  ImeBridgePendingAction leave;
  leave.type = ImeBridgePendingAction::kLeaveInsert;
  leave.client_key = "nvim:1";
  ASSERT_TRUE(s.ApplyAction(leave, /*current_ascii=*/false).should_set);

  s.CleanupStaleClients();  // 必须留下它

  ImeBridgePendingAction enter;
  enter.type = ImeBridgePendingAction::kEnterInsert;
  enter.client_key = "nvim:1";
  auto r = s.ApplyAction(enter, /*current_ascii=*/true);
  EXPECT_TRUE(r.should_set) << "the bit did not survive a cleanup pass";
  EXPECT_FALSE(r.ascii_mode);
}

// 退出 nvim 仍要能恢复：initial_state 的抓取不依赖旧的 stack 逻辑。
TEST(ImeBridgeInsertBit, LeaveInsertAlsoCapturesInitialStateOnce) {
  ImeBridgeState s;
  ImeBridgePendingAction leave;
  leave.type = ImeBridgePendingAction::kLeaveInsert;
  leave.client_key = "nvim:1";
  s.ApplyAction(leave, /*current_ascii=*/false);  // 进 nvim 前是中文
  s.ApplyAction(leave, /*current_ascii=*/true);   // 后续的 leave 不得改写它

  ImeBridgePendingAction reset;
  reset.type = ImeBridgePendingAction::kReset;
  reset.client_key = "nvim:1";
  reset.restore = true;
  auto r = s.ApplyAction(reset, /*current_ascii=*/true);
  ASSERT_TRUE(r.should_set);
  EXPECT_FALSE(r.ascii_mode) << "initial_state should still be the pre-nvim value";
}

// 线格式：这两个动作要能从一条真实消息走通。
TEST(ImeBridgeInsertBit, ProcessMessageDispatchesTheTwoActions) {
  ImeBridgeState s;
  s.ProcessMessage(
      R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
      R"("data":{"action":"leave_insert"}})");
  s.ProcessMessage(
      R"({"v":2,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
      R"("data":{"action":"enter_insert"}})");
  auto actions = s.TakePendingActions();
  ASSERT_EQ(actions.size(), 2u);
  EXPECT_EQ(actions.front().type, ImeBridgePendingAction::kLeaveInsert);
  actions.pop();
  EXPECT_EQ(actions.front().type, ImeBridgePendingAction::kEnterInsert);
}

// 一次 flag day：精确相等的检查意味着新 dylib 会完全忽略仍说 v1 的客户端。
// 这条用例存在的意义是让「顺手把版本改回去」当场失败。
TEST(ImeBridgeProtocol, RejectsTheOldVersionOutright) {
  ImeBridgeState s;
  s.ProcessMessage(
      R"({"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"1"},)"
      R"("data":{"action":"leave_insert"}})");
  EXPECT_TRUE(s.TakePendingActions().empty());
}

TEST(ImeBridgeProtocol, HelloAdvertisesVersionTwo) {
  ImeBridgeState s;
  auto hello = nlohmann::json::parse(s.BuildHello());
  EXPECT_EQ(hello["v"], 2);
}
