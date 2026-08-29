// Layer 2: real Unix-socket round-trip. Starts a test ImeBridgeServer, connects
// a client, sends JSON-Lines protocol messages, and asserts GetActiveContext
// reflects them. The server processes on its own thread, so poll with a timeout.
//
// NOTE: <gtest/gtest.h> is included before "ime_bridge.h" (deviating from the
// brief's literal include order) because ime_bridge.h transitively includes
// <rime/processor.h> -> rime_api.h, which #defines a bare `Bool` macro; if
// that macro is active when gtest-param-test.h is parsed, it collides with
// gtest's `Bool()` function template. See test/ime_bridge_state_test.cc and
// test/commit_text_test.cc for the same ordering used to avoid this
// pre-existing macro collision.
#include <gtest/gtest.h>

#include "ime_bridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <string>
#include <thread>

using rime::ImeBridgePendingAction;
using rime::ImeBridgeServer;

namespace {

int ConnectClient(const std::string& path) {
  int fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (fd < 0) return -1;
  sockaddr_un addr{};
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, path.c_str(), sizeof(addr.sun_path) - 1);
  if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
    close(fd);
    return -1;
  }
  return fd;
}

void SendLine(int fd, const std::string& line) {
  std::string msg = line + "\n";
  ASSERT_GT(write(fd, msg.data(), msg.size()), 0);
}

// Poll GetActiveContext until it has a value with the expected before, or timeout.
bool WaitForBefore(ImeBridgeServer& server, const std::string& expected_before) {
  for (int i = 0; i < 200; ++i) {  // up to ~2s
    auto ctx = server.GetActiveContext();
    if (ctx && ctx->before == expected_before) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

// Drain pending actions until one of the given type shows up, or time out.
bool WaitForAction(ImeBridgeServer& server, ImeBridgePendingAction::Type type,
                   ImeBridgePendingAction* out) {
  for (int i = 0; i < 200; ++i) {  // up to ~2s
    auto q = server.TakePendingActions();
    while (!q.empty()) {
      if (q.front().type == type) {
        if (out) *out = q.front();
        return true;
      }
      q.pop();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

bool WaitForLiveConnections(ImeBridgeServer& server, int expected) {
  for (int i = 0; i < 200; ++i) {  // up to ~2s
    if (server.live_connections() == expected) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return false;
}

std::string Msg(const std::string& app, const std::string& instance, const std::string& data) {
  return std::string(R"({"v":1,"ns":"rime.ime","type":"ascii","src":{"app":")") + app +
         R"(","instance":")" + instance + R"("},"data":)" + data + "}";
}

}  // namespace

TEST(ImeBridgeSocket, ContextOverSocketBecomesActive) {
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  int fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(fd, 0);

  SendLine(fd, Msg("nvim", "1", R"({"action":"context","before":"中","after":"文"})"));
  EXPECT_TRUE(WaitForBefore(server, "中"));
  auto ctx = server.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("文", ctx->after);

  close(fd);
  server.Stop();
}

// A remote client dials every candidate endpoint it can find and keeps only the
// one whose greeting names the machine its ssh session came from. Two things
// have to hold for that to be safe, and both are tested here.

TEST(ImeBridgeSocket, GreetingArrivesBeforeTheClientSaysAnything) {
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test_hello_" + std::to_string(getpid()) + ".sock";
  cfg.host_id = "test-laptop";
  server.Start(cfg);

  int fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(fd, 0);

  // Read without having written: the greeting must be unprompted, or a client
  // would have to leak keystrokes to the wrong machine to discover it is wrong.
  std::string got;
  char buf[512];
  for (int i = 0; i < 200 && got.find('\n') == std::string::npos; ++i) {
    ssize_t n = read(fd, buf, sizeof(buf));
    if (n > 0) {
      got.append(buf, static_cast<size_t>(n));
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  ASSERT_NE(std::string::npos, got.find('\n')) << "no greeting arrived";
  EXPECT_NE(std::string::npos, got.find(R"("type":"hello")")) << got;
  EXPECT_NE(std::string::npos, got.find(R"("host":"test-laptop")")) << got;

  close(fd);
  server.Stop();
}

TEST(ImeBridgeSocket, ProbeThatOnlyReadsTheGreetingLeavesNoState) {
  // This is what happens to the *other* laptop's tunnel on every discovery
  // sweep: we connect, read the greeting, see the wrong host, and hang up
  // without sending a byte. That must not register a client -- if it did, the
  // disconnect would synthesize a reset and flip the other laptop's ascii_mode.
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test_probe_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  // A real client first, so the server has state that a stray reset would show
  // up against.
  int owner = ConnectClient(cfg.socket_path);
  ASSERT_GE(owner, 0);
  SendLine(owner, Msg("nvim", "owner", R"({"action":"context","before":"中","after":"文"})"));
  ASSERT_TRUE(WaitForBefore(server, "中"));
  server.TakePendingActions();  // drain

  for (int i = 0; i < 3; ++i) {
    int probe = ConnectClient(cfg.socket_path);
    ASSERT_GE(probe, 0);
    char buf[512];
    (void)read(probe, buf, sizeof(buf));  // greeting only
    close(probe);
  }
  ASSERT_TRUE(WaitForLiveConnections(server, 1)) << "probes should have drained";

  auto q = server.TakePendingActions();
  while (!q.empty()) {
    EXPECT_NE(ImeBridgePendingAction::kReset, q.front().type)
        << "a silent probe must not synthesize a reset";
    q.pop();
  }
  // And the real client still owns the context.
  auto ctx = server.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("中", ctx->before);

  close(owner);
  server.Stop();
}

TEST(ImeBridgeSocket, DroppedConnectionSynthesizesReset) {
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test3_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  int fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(fd, 0);
  SendLine(fd, Msg("nvim", "1", R"({"action":"set","ascii":true})"));
  ImeBridgePendingAction set_action;
  ASSERT_TRUE(WaitForAction(server, ImeBridgePendingAction::kSet, &set_action));

  close(fd);  // no reset, no unregister: exactly what kill -9 looks like

  ImeBridgePendingAction reset_action;
  ASSERT_TRUE(WaitForAction(server, ImeBridgePendingAction::kReset, &reset_action));
  EXPECT_EQ("nvim:1", reset_action.client_key);
  EXPECT_TRUE(reset_action.restore);

  server.Stop();
}

TEST(ImeBridgeSocket, SecondClientTakesOwnershipOverSocket) {
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test2_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  int fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(fd, 0);
  SendLine(fd, Msg("nvim", "1", R"({"action":"context","before":"A","after":""})"));
  ASSERT_TRUE(WaitForBefore(server, "A"));
  SendLine(fd, Msg("nvim", "2", R"({"action":"context","before":"B","after":""})"));
  EXPECT_TRUE(WaitForBefore(server, "B"));
  auto ctx = server.GetActiveContext();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ("nvim:2", ctx->client_key);

  close(fd);
  server.Stop();
}

TEST(ImeBridgeSocket, StopDrainsConnectionThreads) {
  // Stop() used to only join the accept thread. Connection threads stayed
  // parked on read() holding a reference to state_, so a schema redeploy piled
  // them up and process exit could destroy the singleton out from under them.
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path = "/tmp/rime_copilot_test4_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  int fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(fd, 0);
  SendLine(fd, Msg("nvim", "1", R"({"action":"ping"})"));
  ASSERT_TRUE(WaitForLiveConnections(server, 1));

  auto started = std::chrono::steady_clock::now();
  server.Stop();
  auto elapsed = std::chrono::steady_clock::now() - started;

  EXPECT_EQ(0, server.live_connections());
  EXPECT_LT(elapsed, std::chrono::seconds(2));

  close(fd);
}

// The load-bearing contrast: an `ascii` client's disconnect DOES synthesize a
// reset (mirrors DroppedConnectionSynthesizesReset above), but an
// identity-only connection's disconnect does not, because ProcessMessage
// returns "" for it and HandleConnection's `note` lambda only retains a
// connection ref for a non-empty key (ime_bridge.cc:237-241). Without the
// second half this would just be DroppedConnectionSynthesizesReset again;
// without the GetPushedIdentity() check it could pass by the identity message
// having been silently dropped rather than by the no-registration path.
TEST(ImeBridgeSocket, IdentityDisconnectDoesNotResetUnlikeAscii) {
  ImeBridgeServer server;
  ImeBridgeServer::Config cfg;
  cfg.socket_path =
      "/tmp/rime_copilot_test_identity_contrast_" + std::to_string(getpid()) + ".sock";
  server.Start(cfg);

  // Baseline: a real client's disconnect synthesizes a reset.
  int ascii_fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(ascii_fd, 0);
  SendLine(ascii_fd, Msg("nvim", "contrast", R"({"action":"set","ascii":true})"));
  ImeBridgePendingAction set_action;
  ASSERT_TRUE(WaitForAction(server, ImeBridgePendingAction::kSet, &set_action));
  close(ascii_fd);
  ImeBridgePendingAction reset_action;
  ASSERT_TRUE(WaitForAction(server, ImeBridgePendingAction::kReset, &reset_action));
  EXPECT_EQ("nvim:contrast", reset_action.client_key);

  // An identity connection registers no client at all, so its disconnect
  // must produce no reset.
  int id_fd = ConnectClient(cfg.socket_path);
  ASSERT_GE(id_fd, 0);
  SendLine(id_fd, R"({"v":1,"ns":"rime.ime","type":"identity",)"
                  R"("data":{"socket":"default","pane":"%7","command":"claude"}})");

  bool got_identity = false;
  for (int i = 0; i < 200; ++i) {  // up to ~2s
    auto id = server.GetPushedIdentity();
    if (id && id->pane_id == "%7") {
      got_identity = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  // Proves the message actually arrived, so the reset-free assertion below
  // cannot pass simply because nothing happened.
  ASSERT_TRUE(got_identity) << "identity never arrived";

  close(id_fd);
  ASSERT_TRUE(WaitForLiveConnections(server, 0));

  auto q = server.TakePendingActions();
  while (!q.empty()) {
    EXPECT_NE(ImeBridgePendingAction::kReset, q.front().type)
        << "an identity-only connection must not synthesize a reset on disconnect";
    q.pop();
  }

  server.Stop();
}
