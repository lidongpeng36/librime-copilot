// Drives the impure half of the tmux source: the binary probe, the
// posix_spawn runner's deadline/EOF/reap paths, and the per-key-event memo
// that keeps a keystroke down to one spawn.
//
// No live tmux server is stood up. The runner is pointed at /bin/echo,
// /bin/sh and a path that does not exist -- enough to exercise every branch
// that can hang or wedge the input thread -- and the memo is driven through
// the tmux_detail test hooks with a stub runner, so the spawn count is
// directly observable. test/ime_bridge_socket_test.cc is the repo's precedent
// for a non-pure test (see CLAUDE.md).

#include "tmux_source_internal.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "tmux_source.h"

using namespace rime::tmux_detail;

namespace {

using Clock = std::chrono::steady_clock;

int ElapsedMs(Clock::time_point start) {
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

// --- stubs for the memoization tests -------------------------------------
//
// The hooks are plain function pointers, so the fake runner's bookkeeping has
// to live at namespace scope.

constexpr const char* kDefaultStubOutput = "CLI|1786506891\nFOC|1\nCUR|%7|3|0|40\nabc\n";

int g_run_calls = 0;
std::string g_stub_frontmost = "org.alacritty";
std::string g_stub_output = kDefaultStubOutput;

std::string StubFrontmost() { return g_stub_frontmost; }

bool StubRun(const std::string&, const std::vector<std::string>&, int, std::string* out) {
  ++g_run_calls;
  *out = g_stub_output;
  return true;
}

// Installs the stubs and a known configuration, and restores the real
// implementations afterwards -- the hooks are process-global.
class TmuxMemoTest : public ::testing::Test {
 protected:
  void SetUp() override {
    g_run_calls = 0;
    g_stub_frontmost = "org.alacritty";
    g_stub_output = kDefaultStubOutput;
    rime::TmuxSourceConfig config;
    config.enabled = true;
    config.binary = "/bin/echo";  // must exist; StubRun never execs it
    config.prefix_chars = 1;
    rime::ConfigureTmuxSource(config);
    SetTmuxTestHooks(&StubFrontmost, &StubRun);
  }

  void TearDown() override {
    SetTmuxTestHooks(nullptr, nullptr);
    rime::ConfigureTmuxSource(rime::TmuxSourceConfig{});  // disabled again
  }
};

}  // namespace

TEST(TmuxProbe, HonorsAnExplicitExecutablePath) {
  EXPECT_EQ(ProbeTmuxBinary("/bin/echo"), "/bin/echo");
}

TEST(TmuxProbe, RefusesAnExplicitPathThatIsNotExecutable) {
  // Silently falling back to a well-known tmux would mean reading a different
  // server's panes than the user asked for.
  EXPECT_EQ(ProbeTmuxBinary("/nonexistent/tmux"), "");
  EXPECT_EQ(ProbeTmuxBinary("/etc/hosts"), "");
}

TEST(TmuxProbe, CandidatesAreAbsolutePaths) {
  // An IMK process inherits a minimal PATH; a bare "tmux" would never resolve.
  ASSERT_FALSE(TmuxBinaryCandidates().empty());
  for (const auto& candidate : TmuxBinaryCandidates()) {
    EXPECT_EQ(candidate.front(), '/') << candidate;
  }
}

TEST(TmuxRun, CapturesStdoutOfASuccessfulChild) {
  std::string out;
  ASSERT_TRUE(RunTmuxProcess("/bin/echo", {"CUR|%0|1|0|40"}, 2000, &out));
  EXPECT_EQ(out, "CUR|%0|1|0|40\n");
}

TEST(TmuxRun, ReadsOutputLargerThanOneBufferFill) {
  // The read loop appends in 4KB chunks; a single-shot read would truncate a
  // real pane dump.
  std::string out;
  ASSERT_TRUE(RunTmuxProcess("/bin/sh",
                             {"-c",
                              "i=0; while [ $i -lt 2000 ]; do echo abcdefghij; "
                              "i=$((i+1)); done"},
                             10000, &out));
  EXPECT_EQ(out.size(), 2000u * 11u);
}

TEST(TmuxRun, ReportsANonZeroExitAsFailure) {
  // `tmux` with no server exits non-zero; its stdout must not be parsed.
  std::string out;
  EXPECT_FALSE(RunTmuxProcess("/bin/sh", {"-c", "echo partial; exit 1"}, 2000, &out));
}

TEST(TmuxRun, SpawnFailureIsReportedNotThrown) {
  std::string out;
  EXPECT_FALSE(RunTmuxProcess("/nonexistent/tmux", {"list-clients"}, 2000, &out));
  EXPECT_TRUE(out.empty());
}

TEST(TmuxRun, HonorsTheDeadlineAndKillsTheChild) {
  // The deadline is the whole reason this runner exists: it runs on the input
  // thread. A child that never writes must not hold a keystroke hostage.
  std::string out;
  const auto start = Clock::now();
  EXPECT_FALSE(RunTmuxProcess("/bin/sh", {"-c", "sleep 30"}, 50, &out));
  const int elapsed = ElapsedMs(start);
  EXPECT_LT(elapsed, 3000) << "returned after " << elapsed << "ms; the deadline did not hold";
}

TEST(TmuxRun, LeavesNoZombieBehindAfterATimeout) {
  std::string out;
  ASSERT_FALSE(RunTmuxProcess("/bin/sh", {"-c", "sleep 30"}, 20, &out));
  // The runner reaps its own child, so nothing is left for us to collect;
  // otherwise waitpid(-1) would hand back the abandoned pid.
  errno = 0;
  EXPECT_EQ(waitpid(-1, nullptr, WNOHANG), -1);
  EXPECT_EQ(errno, ECHILD);
}

TEST(TmuxRun, TheReapGraceDoesNotInheritTheReadDeadline) {
  // The child closes stdout at once but lingers, so the read loop finishes
  // long before the process is reapable. The reap loop used to reuse the read
  // deadline, which made its budget "whatever is left of timeout_ms" -- wrong
  // in both directions. Too long: a lingering child pins the input thread for
  // the rest of the configured timeout even though the output already arrived
  // (this test -- 5s of it). Too short: when EOF lands a hair inside the
  // deadline the child is not yet reapable, WNOHANG returns 0, and a perfectly
  // good query gets SIGKILLed with a 5s backoff armed. Its own small grace
  // window fixes both.
  std::string out;
  const auto start = Clock::now();
  const bool ok = RunTmuxProcess("/bin/sh", {"-c", "echo ok; exec 1>&-; sleep 30"}, 5000, &out);
  const int elapsed = ElapsedMs(start);
  EXPECT_FALSE(ok) << "the child never exited cleanly, so the query must fail";
  EXPECT_LT(elapsed, 1000) << "returned after " << elapsed
                           << "ms; the reap waited out the read deadline instead of its own grace";
}

TEST_F(TmuxMemoTest, TheFirstQueryOfAGenerationSpawnsAndAnswers) {
  auto ctx = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(ctx->before, "c");
  EXPECT_EQ(ctx->client_key, "tmux:default:%7");
  EXPECT_EQ(g_run_calls, 1);
}

TEST_F(TmuxMemoTest, RepeatedQueriesWithinAGenerationShareOneSpawn) {
  // AutoSpacer, the re-ranking filter's menu build and GetPredictionContext
  // each call this independently during a single key event. Uncached that was
  // 2-3 forks per keystroke on the input thread.
  const auto first = rime::GetTmuxSurroundingText();
  const auto second = rime::GetTmuxSurroundingText();
  const auto third = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(third.has_value());
  EXPECT_EQ(second->before, first->before);
  EXPECT_EQ(third->client_key, first->client_key);
  EXPECT_EQ(g_run_calls, 1);
}

TEST_F(TmuxMemoTest, InvalidatingRunsAFreshQuery) {
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 1);

  rime::InvalidateTmuxSnapshot();
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 2);
}

TEST_F(TmuxMemoTest, AFreshQuerySeesThePaneMoveOn) {
  // The memo must not outlive its generation: a stale pane handed to
  // AutoSpacer is exactly the cross-talk this source exists to prevent.
  auto before = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(before.has_value());
  EXPECT_EQ(before->before, "c");

  g_stub_output = "CLI|1786506891\nFOC|1\nCUR|%7|4|0|40\nabcd\n";
  // Still the same generation, so the old answer stands...
  EXPECT_EQ(rime::GetTmuxSurroundingText()->before, "c");
  // ...and only a new key event picks the change up.
  rime::InvalidateTmuxSnapshot();
  auto after = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(after.has_value());
  EXPECT_EQ(after->before, "d");
}

TEST_F(TmuxMemoTest, ASnapshotNeverCrossesIntoAnotherApplication) {
  // The frontmost bundle id is part of the memo key, so even if a generation
  // somehow spanned an app switch the terminal's text cannot leak out of it.
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 1);

  g_stub_frontmost = "com.apple.Terminal";
  auto other = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(other.has_value());
  EXPECT_EQ(g_run_calls, 2) << "the memo answered for a different application";
}

TEST_F(TmuxMemoTest, ANonTerminalApplicationIsRefusedWithoutSpawning) {
  g_stub_frontmost = "com.microsoft.VSCode";
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  g_stub_frontmost = "";
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 0);
}

TEST_F(TmuxMemoTest, ARefusalIsMemoizedToo) {
  // A refusal costs a spawn just like an answer, so re-asking within the same
  // key event must not re-spawn to be told "no" again.
  g_stub_output = "FOC|1\nCUR|%7|3|0|40\nabc\n";  // no CLI| line: nothing attached
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 1);
}

TEST_F(TmuxMemoTest, ReconfiguringDropsTheSnapshot) {
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 1);

  rime::TmuxSourceConfig config;
  config.enabled = true;
  config.binary = "/bin/echo";
  config.socket = "/tmp/other.sock";
  rime::ConfigureTmuxSource(config);

  auto ctx = rime::GetTmuxSurroundingText();
  ASSERT_TRUE(ctx.has_value());
  EXPECT_EQ(g_run_calls, 2);
  EXPECT_EQ(ctx->client_key, "tmux:/tmp/other.sock:%7");
}

TEST_F(TmuxMemoTest, ADisabledSourceNeverSpawns) {
  rime::ConfigureTmuxSource(rime::TmuxSourceConfig{});
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(g_run_calls, 0);
}

TEST_F(TmuxMemoTest, TheConfiguredTimeoutIsClampedToTheSharedFloor) {
  // copilot.cc clamps the schema value with the same bounds; a second, lower
  // floor here would make the effective minimum depend on ordering.
  EXPECT_EQ(rime::kMinTimeoutMs, 5);
  EXPECT_LT(rime::kMinTimeoutMs, rime::kMaxTimeoutMs);

  static int seen_timeout = 0;
  struct Capture {
    static bool Run(const std::string&, const std::vector<std::string>&, int timeout_ms,
                    std::string* out) {
      seen_timeout = timeout_ms;
      *out = g_stub_output;
      return true;
    }
  };
  SetTmuxTestHooks(&StubFrontmost, &Capture::Run);

  rime::TmuxSourceConfig config;
  config.enabled = true;
  config.binary = "/bin/echo";
  config.timeout_ms = 0;
  rime::ConfigureTmuxSource(config);
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(seen_timeout, rime::kMinTimeoutMs);

  config.timeout_ms = 100000;
  rime::ConfigureTmuxSource(config);
  ASSERT_TRUE(rime::GetTmuxSurroundingText().has_value());
  EXPECT_EQ(seen_timeout, rime::kMaxTimeoutMs);
}

// The identity must survive the refusals that are about the *text*: a cursor
// outside the captured pane makes the surrounding text unusable but says
// nothing about which pane the caret is in. Routing identity through
// GetTmuxSurroundingText() would drop it at random, silently merging two panes
// into one memory slot.
TEST_F(TmuxMemoTest, IdentitySurvivesCursorOutOfPane) {
  // cursor_y past the last captured row: ExtractContext refuses.
  g_stub_output = "CLI|1786506891\nFOC|1\nCUR|%7|0|9|80|zsh\nprompt\n";
  rime::InvalidateTmuxSnapshot();
  const int before = g_run_calls;
  EXPECT_FALSE(rime::GetTmuxSurroundingText().has_value());
  auto id = rime::GetTmuxPaneIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->pane_id, "%7");
  EXPECT_EQ(id->command, "zsh");
  // The snapshot memo is what saves this from a second spawn now that the
  // ExtractContext refusal is no longer memoized on its own -- see
  // AcquireSnapshot().
  EXPECT_EQ(g_run_calls, before + 1);
}

TEST_F(TmuxMemoTest, IdentityRefusedForNonTerminalApp) {
  g_stub_frontmost = "com.tencent.xinWeChat";
  g_stub_output = "CLI|1786506891\nFOC|1\nCUR|%7|0|0|80|zsh\nprompt\n";
  rime::InvalidateTmuxSnapshot();
  EXPECT_FALSE(rime::GetTmuxPaneIdentity().has_value());
  EXPECT_FALSE(rime::FrontmostIsTerminal());
}

TEST_F(TmuxMemoTest, IdentityRefusedWhenClientsAmbiguous) {
  // Two attached clients stamped in the same second, focus-events off: the
  // pre-existing JudgeClients refusal. It is about reaching tmux, not about
  // the text, so identity must refuse with it.
  g_stub_output =
      "CLI|1786506891\nCLI|1786506891\nFOC|0\nCUR|%7|0|0|80|zsh\nprompt\n";
  rime::InvalidateTmuxSnapshot();
  EXPECT_FALSE(rime::GetTmuxPaneIdentity().has_value());
}

// The cross-rung key invariant, on the side that actually derives the socket.
// The pushed rung (rime-copilot-clients' tmux reporter, scripts/report.sh)
// can only report ${TMUX%%,*}, an absolute path in EVERY case including the
// default socket, so the polled rung
// must key on tmux's own `#{socket_path}` -- not on `copilot/tmux_source/socket`,
// which is empty by default. They used to agree only by the coincidence that
// MakeKey renders an empty socket as "default", which is also the default
// socket's basename; that made every non-default socket build two slots for
// one pane, and its mode came back from whichever rung answered last.
TEST_F(TmuxMemoTest, IdentitySocketIsTmuxsOwnPathNotTheConfiguredOne) {
  g_stub_output = "CLI|1786506891\nFOC|1\nSCK|/tmp/tmux-501/work\nCUR|%7|0|0|80|zsh\nprompt\n";
  rime::InvalidateTmuxSnapshot();
  auto id = rime::GetTmuxPaneIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->socket, "/tmp/tmux-501/work");
  // The literal the reporter emits for TMUX=/tmp/tmux-501/work,... -- pinned
  // on the shell side by rime-copilot-clients' test/report_test.py,
  // test_socket_field_is_the_full_path_the_plugin_keys_on.
  EXPECT_EQ(rime::context_memory::MakeKey(*id, true), "tmux:/tmp/tmux-501/work:%7|zsh");
}

TEST_F(TmuxMemoTest, IdentityFallsBackToTheConfiguredSocketWhenTmuxCannotSayIt) {
  // A tmux too old to know `#{socket_path}` expands it to nothing. Falling
  // back to the configured socket is exactly the pre-existing behaviour, so
  // such a machine is no worse off than before this field was asked for.
  rime::TmuxSourceConfig config;
  config.enabled = true;
  config.binary = "/bin/echo";
  config.prefix_chars = 1;
  config.socket = "/tmp/configured.sock";
  rime::ConfigureTmuxSource(config);
  g_stub_output = "CLI|1786506891\nFOC|1\nCUR|%7|0|0|80|zsh\nprompt\n";
  rime::InvalidateTmuxSnapshot();
  auto id = rime::GetTmuxPaneIdentity();
  ASSERT_TRUE(id.has_value());
  EXPECT_EQ(id->socket, "/tmp/configured.sock");
}

TEST_F(TmuxMemoTest, IdentityAndSurroundingShareOneSpawn) {
  // The whole cost argument for rung 2 is that identity rides the snapshot
  // AutoSpacer forces anyway. If these ever stop sharing, that argument is
  // void and nothing else would say so.
  rime::InvalidateTmuxSnapshot();
  const int before = g_run_calls;
  rime::GetTmuxPaneIdentity();
  rime::GetTmuxSurroundingText();
  EXPECT_EQ(g_run_calls, before + 1);
}
