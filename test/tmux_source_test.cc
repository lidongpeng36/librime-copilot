// Drives the impure half of the tmux source: the binary probe and the
// posix_spawn runner's deadline, EOF and reap paths.
//
// No live tmux server is stood up -- the runner is pointed at /bin/echo,
// /bin/sh and a path that does not exist, which is enough to exercise every
// branch that can hang or wedge the input thread. test/ime_bridge_socket_test.cc
// is the repo's precedent for a non-pure test (see CLAUDE.md).

#include "tmux_source_internal.h"

#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <string>
#include <vector>

#include <gtest/gtest.h>

using namespace rime::tmux_detail;

namespace {

using Clock = std::chrono::steady_clock;

int ElapsedMs(Clock::time_point start) {
  return static_cast<int>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count());
}

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
  ASSERT_TRUE(RunTmuxProcess("/bin/sh", {"-c", "i=0; while [ $i -lt 2000 ]; do echo abcdefghij; "
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
