// 纯逻辑，不需要 Rime 引擎 —— 和 context_memory.h 同一个理由，也是
// test/ 里其余文件的通行做法。
#include "remote_binding.h"

#include <gtest/gtest.h>

using rime::context_memory::Identity;
using rime::context_memory::IsRemoteCommand;
using rime::context_memory::RemoteBinding;

namespace {
Identity Remote(const char* host, const char* pane, const char* command) {
  Identity id;
  id.socket = "/tmp/tmux-1000/default";
  id.pane_id = pane;
  id.command = command;
  id.host = host;
  return id;
}

// Like Remote(), but with an explicit socket -- for pinning that the
// eviction key includes it. Kept separate rather than adding a parameter to
// Remote(), which four other tests already call positionally.
Identity RemoteOnSocket(const char* host, const char* pane, const char* command,
                        const char* socket) {
  Identity id = Remote(host, pane, command);
  id.socket = socket;
  return id;
}
}  // namespace

TEST(RemoteBinding, LookupMissesBeforeAnythingIsBound) {
  RemoteBinding b;
  EXPECT_FALSE(b.Lookup("%4", "ssh").has_value());
}

TEST(RemoteBinding, BoundRemoteIsFoundBehindTheLocalPane) {
  RemoteBinding b;
  b.Bind("%4", "ssh", Remote("devbox", "%2", "zsh"));
  auto found = b.Lookup("%4", "ssh");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->host, "devbox");
  EXPECT_EQ(found->pane_id, "%2");
}

// 守卫的整个存在理由：pane 里的 ssh 退出后换成了 shell，这条记录就必须
// 停止生效，而且不需要有人来清它。两边读的都是 pane_current_command
// （轮询走 tmux 快照，推送走钩子脚本），所以它们天然一致。
TEST(RemoteBinding, LookupDeclinesWhenTheLocalCommandChanged) {
  RemoteBinding b;
  b.Bind("%4", "ssh", Remote("devbox", "%2", "zsh"));
  EXPECT_FALSE(b.Lookup("%4", "zsh").has_value());
  EXPECT_TRUE(b.Lookup("%4", "ssh").has_value());
}

TEST(RemoteBinding, RebindingOneLocalPaneReplacesItsEntry) {
  RemoteBinding b;
  b.Bind("%4", "ssh", Remote("devbox", "%2", "zsh"));
  b.Bind("%4", "ssh", Remote("devbox", "%9", "zsh"));
  auto found = b.Lookup("%4", "ssh");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->pane_id, "%9");
  EXPECT_EQ(b.size(), 1u);
}

TEST(RemoteBinding, DistinctLocalPanesHoldDistinctRemotes) {
  RemoteBinding b;
  b.Bind("%4", "ssh", Remote("devbox", "%2", "zsh"));
  b.Bind("%7", "ssh", Remote("devbox", "%5", "zsh"));
  EXPECT_EQ(b.Lookup("%4", "ssh")->pane_id, "%2");
  EXPECT_EQ(b.Lookup("%7", "ssh")->pane_id, "%5");
}

// 光标不可能同时在两个地方。两个本地 pane 同时声称同一个远端 pane 是矛盾，
// 新的那次声称是更好的证据 —— 这正是 2026-09-08 观察到的故障形状：%15 和
// %18 都绑在 mini 的 %1 上，切一个另一个跟着切。
TEST(RemoteBinding, ClaimingARemotePaneReleasesItFromAnyOtherLocalPane) {
  RemoteBinding b;
  b.Bind("%18", "ssh", Remote("Mac-Mini", "%1", "zsh"));
  b.Bind("%15", "ssh", Remote("Mac-Mini", "%1", "claude"));
  EXPECT_EQ(b.size(), 1u);
  EXPECT_FALSE(b.Lookup("%18", "ssh").has_value());
  ASSERT_TRUE(b.Lookup("%15", "ssh").has_value());
  EXPECT_EQ(b.Lookup("%15", "ssh")->command, "claude");
}

// 驱逐的键是 (host, pane_id)，不是 host —— 上面那条
// DistinctLocalPanesHoldDistinctRemotes 就是反例，而它描述的是这个功能的
// 招牌场景（两个本地 pane 各自 ssh 到同一台机器的不同远端 pane）。按 host
// 驱逐会把整个功能废掉，所以这里再钉一次，紧挨着驱逐的实现。
TEST(RemoteBinding, TwoPanesOfOneHostAreNotAClaimOnEachOther) {
  RemoteBinding b;
  b.Bind("%4", "ssh", Remote("devbox", "%2", "zsh"));
  b.Bind("%7", "ssh", Remote("devbox", "%5", "zsh"));
  EXPECT_EQ(b.size(), 2u);
  EXPECT_TRUE(b.Lookup("%4", "ssh").has_value());
  EXPECT_TRUE(b.Lookup("%7", "ssh").has_value());
}

// 同一个 pane 重新绑定不该把自己驱逐掉。
TEST(RemoteBinding, RebindingTheSamePaneToTheSameRemoteKeepsIt) {
  RemoteBinding b;
  b.Bind("%15", "ssh", Remote("Mac-Mini", "%1", "zsh"));
  b.Bind("%15", "ssh", Remote("Mac-Mini", "%1", "claude"));
  EXPECT_EQ(b.size(), 1u);
  ASSERT_TRUE(b.Lookup("%15", "ssh").has_value());
  EXPECT_EQ(b.Lookup("%15", "ssh")->command, "claude");
}

// host 为空的远端身份不存在（Bind 只在远端推送时被调用），但如果哪天有了，
// 空 host 不能把彼此当成同一台机器 —— 那会把每个 pane_id 相同的本地 pane
// 互相驱逐。
TEST(RemoteBinding, AnEmptyHostIsNotAClaimOnAnything) {
  RemoteBinding b;
  Identity nohost;
  nohost.pane_id = "%1";
  nohost.command = "zsh";
  b.Bind("%4", "ssh", nohost);
  b.Bind("%7", "ssh", nohost);
  EXPECT_EQ(b.size(), 2u);
}

// 有界的理由和 Table 一样：pane id 单调增长，一台从不重启的机器上
// 无界的表会无限长。淘汰必须丢最久未用的那个。
TEST(RemoteBinding, EvictsTheLeastRecentlyUsedBeyondMaxEntries) {
  RemoteBinding b;
  b.SetMaxEntries(2);
  b.Bind("%1", "ssh", Remote("devbox", "%a", "zsh"));
  b.Bind("%2", "ssh", Remote("devbox", "%b", "zsh"));
  ASSERT_TRUE(b.Lookup("%1", "ssh").has_value());  // %1 现在是最近使用
  b.Bind("%3", "ssh", Remote("devbox", "%c", "zsh"));
  EXPECT_EQ(b.size(), 2u);
  EXPECT_TRUE(b.Lookup("%1", "ssh").has_value());
  EXPECT_TRUE(b.Lookup("%3", "ssh").has_value());
  EXPECT_FALSE(b.Lookup("%2", "ssh").has_value());  // 最久未用的被丢掉
}

// The eviction key is (socket, host, pane_id), not (host, pane_id): two
// independent tmux servers on the same host can number their panes
// identically (both have a "%2"), and that collision must not evict across
// servers. Mirrors TwoPanesOfOneHostAreNotAClaimOnEachOther above, but varies
// socket while host and pane_id stay fixed, instead of the other way round.
TEST(RemoteBinding, TwoTmuxServersOnOneHostAreNotAClaimOnEachOtherDespiteSamePaneId) {
  RemoteBinding b;
  b.Bind("%4", "ssh", RemoteOnSocket("devbox", "%2", "zsh", "/tmp/tmux-1000/default"));
  b.Bind("%7", "ssh", RemoteOnSocket("devbox", "%2", "zsh", "/tmp/tmux-1000/alt"));
  EXPECT_EQ(b.size(), 2u);
  EXPECT_TRUE(b.Lookup("%4", "ssh").has_value());
  EXPECT_TRUE(b.Lookup("%7", "ssh").has_value());
}

TEST(IsRemoteCommandTest, MatchesExactlyAndOnlyListedCommands) {
  const std::vector<std::string> commands{"ssh", "mosh", "et"};
  EXPECT_TRUE(IsRemoteCommand(commands, "ssh"));
  EXPECT_TRUE(IsRemoteCommand(commands, "mosh"));
  EXPECT_FALSE(IsRemoteCommand(commands, "zsh"));
  EXPECT_FALSE(IsRemoteCommand(commands, ""));
  // 前缀不算 —— "sshfs" 不是一个远端 shell。
  EXPECT_FALSE(IsRemoteCommand(commands, "sshfs"));
}

TEST(IsRemoteCommandTest, EmptyListBindsNothing) { EXPECT_FALSE(IsRemoteCommand({}, "ssh")); }
