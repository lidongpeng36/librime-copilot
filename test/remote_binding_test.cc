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

TEST(IsRemoteCommandTest, MatchesExactlyAndOnlyListedCommands) {
  const std::vector<std::string> commands{"ssh", "mosh", "et"};
  EXPECT_TRUE(IsRemoteCommand(commands, "ssh"));
  EXPECT_TRUE(IsRemoteCommand(commands, "mosh"));
  EXPECT_FALSE(IsRemoteCommand(commands, "zsh"));
  EXPECT_FALSE(IsRemoteCommand(commands, ""));
  // 前缀不算 —— "sshfs" 不是一个远端 shell。
  EXPECT_FALSE(IsRemoteCommand(commands, "sshfs"));
}

TEST(IsRemoteCommandTest, EmptyListBindsNothing) {
  EXPECT_FALSE(IsRemoteCommand({}, "ssh"));
}
