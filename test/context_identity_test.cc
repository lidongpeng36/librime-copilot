#include "context_identity.h"

#include <gtest/gtest.h>

using rime::ContextIdentitySource;
using rime::context_memory::Identity;

namespace {

using rime::context_memory::PendingBind;

std::optional<Identity> g_bridge;
std::optional<PendingBind> g_pending;
std::optional<Identity> g_tmux;
bool g_terminal = true;

std::optional<Identity> BridgeHook() { return g_bridge; }
std::optional<Identity> TmuxHook() { return g_tmux; }
bool TerminalHook() { return g_terminal; }

// DRAINS, exactly as ImeBridgeState::TakePendingBind does. A fake that returned
// the same value forever would rebind on every key event -- which is the bug
// the drain exists to prevent, so a non-draining fake would make these tests
// pass over an implementation that does not drain.
std::optional<PendingBind> PendingHook() {
  auto taken = g_pending;
  g_pending.reset();
  return taken;
}

Identity Local(const char* pane, const char* command) {
  return Identity{"default", pane, command};
}

Identity Remote(const char* host, const char* pane, const char* command) {
  return Identity{"/tmp/tmux-1000/default", pane, command, host};
}

class ContextIdentityTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_bridge.reset();
    g_pending.reset();
    g_tmux.reset();
    g_terminal = true;
    rime::SetContextIdentityTestHooks(&BridgeHook, &PendingHook, &TmuxHook, &TerminalHook);
    rime::ResetRemoteBindingForTest();
    rime::SetRemoteBindingOptions({"ssh", "mosh", "et"}, 256);
  }
  void TearDown() override {
    rime::SetContextIdentityTestHooks(nullptr, nullptr, nullptr, nullptr);
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

TEST_F(ContextIdentityTest, RemotePushIsReportedAsBridgeRemote) {
  g_bridge = Remote("devbox", "%2", "zsh");
  g_tmux = Local("%4", "ssh");
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "devbox");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridgeRemote);
}

// 核心行为，改用新的接缝：绑定来自到达时配好的那一对，之后本机推送该 pane
// 就能拿回远端身份。
TEST_F(ContextIdentityTest, ALocalSshPaneResolvesToTheRemoteBehindIt) {
  g_pending = PendingBind{Remote("devbox", "%2", "zsh"), Local("%4", "ssh")};
  g_bridge = Remote("devbox", "%2", "zsh");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Local("%4", "ssh");
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "devbox");
  EXPECT_EQ(r->id.pane_id, "%2");
  EXPECT_EQ(r->source, ContextIdentitySource::kBoundRemote);
}

// 守卫仍在：配对的本地 pane 跑的不是 remote_commands 里的东西就不绑。
TEST_F(ContextIdentityTest, RefusesToBindBehindANonRemoteCommand) {
  g_pending = PendingBind{Remote("devbox", "%2", "zsh"), Local("%4", "zsh")};
  g_bridge = Remote("devbox", "%2", "zsh");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Local("%4", "zsh");
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->id.host, "");
  EXPECT_EQ(r->source, ContextIdentitySource::kBridge);
}

// 没有配对就不绑 —— 本地 tmux 钩子没装的机器就是这个状态。同时也是
// 「轮询彻底退出绑定路径」的证据：g_tmux 明明答得出 %4，绑定仍然没有
// 发生。轮询仍是解析的第二优先级（FallsBackToTmux 钉着），但不再能决定
// 任何东西绑到哪儿。
TEST_F(ContextIdentityTest, RefusesToBindWhenNothingWasPending) {
  g_bridge = Remote("devbox", "%2", "zsh");
  g_tmux = Local("%4", "ssh");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Local("%4", "ssh");
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
  g_pending = PendingBind{Remote("host-a", "%A", "zsh"), Local("%1", "ssh")};
  g_bridge = Remote("host-a", "%A", "zsh");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  // Bind a SECOND local pane, %2, behind remote host-b. With a clamped bound
  // of 1 this must evict %1's binding.
  g_pending = PendingBind{Remote("host-b", "%B", "zsh"), Local("%2", "ssh")};
  g_bridge = Remote("host-b", "%B", "zsh");
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

// 2026-09-08 观察到的故障，一条测试。推送到达时配的是 %15；用户还没打字
// 就切到另一个 ssh pane %18。今天的代码在按键时轮询，绑到 %18 —— 而 %18
// 后面根本没有远端 tmux，永远不会有推送来纠正它。
TEST_F(ContextIdentityTest, BindsThePaneCapturedAtArrivalNotTheOneInFrontNow) {
  g_pending = PendingBind{Remote("Mac-Mini", "%1", "claude"), Local("%15", "ssh")};
  g_tmux = Local("%18", "ssh");
  g_bridge = Local("%18", "ssh");
  auto other = rime::GetContextIdentity();
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(other->source, ContextIdentitySource::kBridge);
  EXPECT_EQ(other->id.pane_id, "%18");
  EXPECT_EQ(other->id.host, "");

  g_bridge = Local("%15", "ssh");
  auto back = rime::GetContextIdentity();
  ASSERT_TRUE(back.has_value());
  EXPECT_EQ(back->source, ContextIdentitySource::kBoundRemote);
  EXPECT_EQ(back->id.host, "Mac-Mini");
}

// 排空发生在前台应用闸门之前。闸门管的是「要不要相信这个身份」——推送的
// 格子没有过期，用户切到微信时不能再认它。绑定是另一回事：「%15 后面挂着
// mini 的 %1」不管前台是谁都成立。放在闸门后面，用户切走再切回来、中间
// 没有新推送时这次绑定就丢了。
TEST_F(ContextIdentityTest, APendingBindIsAppliedEvenWhenTheTerminalIsNotFrontmost) {
  g_terminal = false;
  g_pending = PendingBind{Remote("Mac-Mini", "%1", "claude"), Local("%15", "ssh")};
  EXPECT_FALSE(rime::GetContextIdentity().has_value());

  g_terminal = true;
  g_bridge = Local("%15", "ssh");
  auto r = rime::GetContextIdentity();
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(r->source, ContextIdentitySource::kBoundRemote);
  EXPECT_EQ(r->id.host, "Mac-Mini");
}

// Part A 与 Part B 一起：错绑之后，正确的那个 pane 下一次推送把它挤掉。
TEST_F(ContextIdentityTest, AWrongBindingIsReleasedByTheRightPanesNextPush) {
  g_pending = PendingBind{Remote("Mac-Mini", "%1", "zsh"), Local("%18", "ssh")};
  g_bridge = Remote("Mac-Mini", "%1", "zsh");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Local("%18", "ssh");
  ASSERT_EQ(rime::GetContextIdentity()->source, ContextIdentitySource::kBoundRemote);

  g_pending = PendingBind{Remote("Mac-Mini", "%1", "claude"), Local("%15", "ssh")};
  g_bridge = Remote("Mac-Mini", "%1", "claude");
  ASSERT_TRUE(rime::GetContextIdentity().has_value());

  g_bridge = Local("%18", "ssh");
  auto released = rime::GetContextIdentity();
  ASSERT_TRUE(released.has_value());
  EXPECT_EQ(released->source, ContextIdentitySource::kBridge);

  g_bridge = Local("%15", "ssh");
  auto claimed = rime::GetContextIdentity();
  ASSERT_TRUE(claimed.has_value());
  EXPECT_EQ(claimed->source, ContextIdentitySource::kBoundRemote);
}
