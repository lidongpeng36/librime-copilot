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
  }
  void TearDown() override { rime::SetContextIdentityTestHooks(nullptr, nullptr, nullptr); }
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
// tools/rime_ctx_report.sh. This pins the C++ half against a literal, and
// tools/test/rime_ctx_report_test.py pins the shell half against the same
// one: the reporter's `data.socket` for TMUX=/tmp/tmux-501/work,... must be
// exactly the string fed here. Change one and the other fails.
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
