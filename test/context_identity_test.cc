#include "context_identity.h"

#include <gtest/gtest.h>

using rime::ContextIdentitySource;
using rime::context_memory::Identity;

namespace {

std::optional<Identity> g_bridge;
std::optional<Identity> g_tmux;
bool g_terminal = true;

std::optional<Identity> BridgeHook() { return g_bridge; }
std::optional<Identity> TmuxHook() { return g_tmux; }
bool TerminalHook() { return g_terminal; }

class ContextIdentityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_bridge.reset();
    g_tmux.reset();
    g_terminal = true;
    rime::SetContextIdentityTestHooks(&BridgeHook, &TmuxHook, &TerminalHook);
    rime::ResetRemoteBindingForTest();
    rime::SetRemoteBindingOptions({"ssh", "mosh", "et"}, 256);
  }
  void TearDown() override {
    rime::SetContextIdentityTestHooks(nullptr, nullptr, nullptr);
    rime::ResetRemoteBindingForTest();
  }
};

}  // namespace

TEST_F(ContextIdentityTest, NoneWhenNobodyAnswers) {
  EXPECT_FALSE(rime::GetContextIdentity().has_value());
}

TEST_F(ContextIdentityTest, BridgeOutranksTmux) {
  g_bridge = Identity{"default", "%1", "nvim"};
  g_tmux = Identity{"default", "%9", "zsh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.pane_id, "%1");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridge);
}

TEST_F(ContextIdentityTest, FallsBackToTmux) {
  g_tmux = Identity{"default", "%9", "zsh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.pane_id, "%9");
  EXPECT_EQ(r->source, ContextIdentitySource::kTmux);
}

// The pushed identity is a cell with no expiry: without this gate, switching
// from the terminal to WeChat would go on reporting the last tmux pane, and
// the memory table would attribute WeChat's mode to it.
TEST_F(ContextIdentityTest, PushedIdentityIsGatedOnTheFrontmostApp) {
  g_bridge = Identity{"default", "%1", "nvim"};
  g_terminal = false;
  EXPECT_FALSE(rime::GetContextIdentity().has_value());
}

// The spec's cross-rung invariant. Both rungs hand their Identity to the same
// MakeKey, so this holds structurally -- the test is here to catch someone
// later building a key by hand on one side. A machine that adds the tmux hook
// after having used polling must find the keys it already remembered.
TEST_F(ContextIdentityTest, BothRungsProduceTheSameKey) {
  const Identity same{"default", "%7", "claude"};
  g_tmux = same;
  auto polled = rime::GetContextIdentity();
  ASSERT_TRUE(polled.has_value());
  g_bridge = same;
  auto pushed = rime::GetContextIdentity();
  ASSERT_TRUE(pushed.has_value());
  EXPECT_NE(polled->source, pushed->source);
  EXPECT_EQ(rime::context_memory::MakeKey(polled->id, true),
            rime::context_memory::MakeKey(pushed->id, true));
  EXPECT_EQ(rime::context_memory::MakeKey(polled->id, false),
            rime::context_memory::MakeKey(pushed->id, false));
}

// BothRungsProduceTheSameKey above feeds ONE Identity to both hooks, so it can
// only show MakeKey is deterministic -- the real divergence was in how each
// rung DERIVES `socket`, and that lives half in C++ and half in
// rime-copilot-clients' tmux reporter (scripts/report.sh). This pins the C++
// half against a literal, and that repo's test/report_test.py pins the shell
// half against the same one: the reporter's `data.socket` for
// TMUX=/tmp/tmux-501/work,... must be exactly the string fed here. Change one
// and the other fails.
//
// A NON-default socket on purpose. With the default socket the two used to
// agree by coincidence (the polled rung had "", MakeKey renders that as
// "default", which is also the default socket's basename), which is why the
// bug survived: every test that could have caught it used the default.
TEST_F(ContextIdentityTest, PolledKeyForANonDefaultSocketIsTheReportersString) {
  g_tmux = Identity{"/tmp/tmux-501/work", "%7", "claude"};
  auto polled = rime::GetContextIdentity();
  ASSERT_TRUE(polled.has_value());
  EXPECT_EQ(rime::context_memory::MakeKey(polled->id, true), "tmux:/tmp/tmux-501/work:%7|claude");
}

TEST_F(ContextIdentityTest, EmptyPaneIdIsNotAnIdentity) {
  g_bridge = Identity{"default", "", "nvim"};
  EXPECT_FALSE(rime::GetContextIdentity().has_value());
}

namespace {
rime::context_memory::Identity RemoteId(const char* host, const char* pane) {
  rime::context_memory::Identity id;
  id.socket = "/tmp/tmux-1000/default";
  id.pane_id = pane;
  id.command = "zsh";
  id.host = host;
  return id;
}
}  // namespace

TEST_F(ContextIdentityTest, RemotePushIsReportedAsBridgeRemote) {
  g_bridge = RemoteId("devbox", "%2");
  g_tmux = Identity{"default", "%4", "ssh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "devbox");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridgeRemote);
}

// 核心行为：远端推送时顺手把它绑到当前的本地 pane 上，之后本机推送
// 该 pane 就能拿回远端身份。
TEST_F(ContextIdentityTest, ALocalSshPaneResolvesToTheRemoteBehindIt) {
  g_bridge = RemoteId("devbox", "%2");
  g_tmux = Identity{"default", "%4", "ssh"};
  ASSERT_TRUE(rime::GetContextIdentity().has_value());  // 绑定发生在这一次

  g_bridge = Identity{"default", "%4", "ssh"};  // 本机钩子推送同一个 pane
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "devbox");
  EXPECT_EQ(r->id.pane_id, "%2");
  EXPECT_EQ(r->source, ContextIdentitySource::kBoundRemote);
}

// 守卫。用户切完远端 pane、还没打字就切到本地 shell pane —— 不拦的话
// 远端身份会绑到错的本地 pane 上，那个 pane 从此答另一台机器的模式。
TEST_F(ContextIdentityTest, RefusesToBindBehindANonRemoteCommand) {
  g_bridge = RemoteId("devbox", "%2");
  g_tmux = Identity{"default", "%4", "zsh"};  // 不是 ssh
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Identity{"default", "%4", "zsh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridge);
}

TEST_F(ContextIdentityTest, RefusesToBindWhenNoLocalIdentityAnswers) {
  g_bridge = RemoteId("devbox", "%2");
  g_tmux.reset();  // 轮询答不出来
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Identity{"default", "%4", "ssh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->source, ContextIdentitySource::kBridge);
}

// 本机推送在没有任何绑定时必须原样通过 —— 这是绝大多数机器的常态，
// 一次回归就会让本机 pane 记忆失效。
TEST_F(ContextIdentityTest, PlainLocalPushIsUnaffected) {
  g_bridge = Identity{"default", "%4", "zsh"};
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.pane_id, "%4");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridge);
}

TEST_F(ContextIdentityTest, SourceNamesAreDistinct) {
  EXPECT_STREQ(rime::DescribeIdentitySource(ContextIdentitySource::kBridge), "bridge");
  EXPECT_STREQ(rime::DescribeIdentitySource(ContextIdentitySource::kBridgeRemote),
               "bridge-remote");
  EXPECT_STREQ(rime::DescribeIdentitySource(ContextIdentitySource::kBoundRemote),
               "bound-remote");
  EXPECT_STREQ(rime::DescribeIdentitySource(ContextIdentitySource::kTmux), "tmux");
}

// Pins the bug found in review: `copilot/context_memory/max_entries: -5` in a
// schema reaches Config::GetInt as a negative int. static_cast<size_t>(-5)
// does not clamp it, it WRAPS to a value near SIZE_MAX, and
// RemoteBinding::SetMaxEntries then reads that as "no bound" -- the exact
// unbounded table the class exists to prevent. Passes the raw -5 straight
// into SetRemoteBindingOptions -- the same call copilot.cc makes, and the
// same signature (a raw int, not a pre-clamped size_t) -- so the clamp under
// test is the one the production call site actually runs, not one the test
// performs on its own behalf. A prior version of this test clamped -5 itself
// before the call and so passed whether or not SetRemoteBindingOptions
// clamped anything. Drives the real production path (SetRemoteBindingOptions
// -> ClampMaxEntries -> RemoteBinding, all through GetContextIdentity) rather
// than asserting on ClampMaxEntries in isolation, so it actually observes the
// eviction the bound promises.
TEST_F(ContextIdentityTest, HostileConfiguredMaxEntriesStaysBounded) {
  rime::SetRemoteBindingOptions({"ssh"}, -5);

  // Bind local pane %1 (running ssh) behind remote host-a.
  g_bridge = RemoteId("host-a", "%A");
  g_tmux = Identity{"default", "%1", "ssh"};
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  // Bind a SECOND local pane, %2, behind remote host-b. With a clamped bound
  // of 1 this must evict %1's binding.
  g_bridge = RemoteId("host-b", "%B");
  g_tmux = Identity{"default", "%2", "ssh"};
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  // %1 must no longer resolve to host-a -- its binding was evicted.
  g_bridge = Identity{"default", "%1", "ssh"};
  auto r1 = rime::GetContextIdentity();
  ASSERT_TRUE(r1.has_value());
  EXPECT_EQ(r1->source, ContextIdentitySource::kBridge);
  EXPECT_EQ(r1->id.host, "");

  // %2's binding, the more recent one, must still answer.
  g_bridge = Identity{"default", "%2", "ssh"};
  auto r2 = rime::GetContextIdentity();
  ASSERT_TRUE(r2.has_value());
  EXPECT_EQ(r2->source, ContextIdentitySource::kBoundRemote);
  EXPECT_EQ(r2->id.host, "host-b");
}
