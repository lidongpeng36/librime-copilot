#include "tmux_source.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <thread>

#include <rime/common.h>

#include "frontmost_app.h"
#include "tmux_source_internal.h"
#include "tmux_source_util.h"

extern char** environ;

namespace rime {
namespace {

using Clock = std::chrono::steady_clock;

std::mutex g_mutex;
TmuxSourceConfig g_config;
std::string g_resolved_binary;  // "" = not resolved yet or resolved to nothing
bool g_binary_resolved = false;
Clock::time_point g_backoff_until;

// Per-key-event memo. See InvalidateTmuxSnapshot in tmux_source.h for why the
// generation counter is the invalidation trigger rather than a clock.
uint64_t g_generation = 1;
bool g_snapshot_valid = false;
uint64_t g_snapshot_generation = 0;
std::string g_snapshot_frontmost;
// The memo holds the raw tmux snapshot, not the final SurroundingText: it is
// shared by GetTmuxSurroundingText() (which applies ExtractContext to it) and
// GetTmuxPaneIdentity() (which never needs ExtractContext at all). See
// AcquireSnapshot(). A shared_ptr, not a value: a memo hit is consulted 2-3
// times per key event, and Snapshot carries a pane dump plus two style
// vectors -- a value memo would deep-copy several KB on every one of those
// hits instead of bumping a refcount.
std::shared_ptr<const tmux_detail::Snapshot> g_snapshot;

// Test seams; null means "use the real thing". Read under g_mutex is
// unnecessary -- tests install them before any query runs.
tmux_detail::FrontmostBundleIdFn g_frontmost_hook = nullptr;
tmux_detail::RunTmuxFn g_run_hook = nullptr;

// Set FD_CLOEXEC so a spawn from another thread, racing between our pipe()
// and the posix_spawn below, cannot inherit this call's write end and hold
// the pipe open -- which would cost us the full deadline waiting for an EOF
// that never comes. posix_spawn's adddup2 clears the flag on the duplicate,
// so our own child still gets its stdout.
void SetCloexec(int fd) {
  const int flags = fcntl(fd, F_GETFD);
  if (flags >= 0) {
    fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
  }
}

// Resolve once and remember, including a miss. Callers hold g_mutex.
// Returns true when this call performed the probe (so a failure is logged
// exactly once per configuration, not once per keystroke).
bool ResolveBinaryLocked(const std::string& configured, std::string* resolved) {
  if (g_binary_resolved) {
    *resolved = g_resolved_binary;
    return false;
  }
  g_resolved_binary = tmux_detail::ProbeTmuxBinary(configured);
  g_binary_resolved = true;
  *resolved = g_resolved_binary;
  return true;
}

}  // namespace

namespace tmux_detail {

const std::vector<std::string>& TmuxBinaryCandidates() {
  static const std::vector<std::string> candidates = {"/opt/homebrew/bin/tmux",
                                                      "/usr/local/bin/tmux", "/usr/bin/tmux"};
  return candidates;
}

std::string ProbeTmuxBinary(const std::string& configured) {
  if (!configured.empty()) {
    // An explicit path is a deliberate choice -- a wrapper, a different
    // install, one bound to a different socket namespace. Honor it or fail;
    // silently substituting a well-known path would mean reading a different
    // tmux server's panes and handing that text to AutoSpacer, which is
    // exactly the guessing this feature must refuse to do.
    return access(configured.c_str(), X_OK) == 0 ? configured : std::string();
  }
  for (const auto& candidate : TmuxBinaryCandidates()) {
    if (access(candidate.c_str(), X_OK) == 0) {
      return candidate;
    }
  }
  return std::string();
}

bool RunTmuxProcess(const std::string& bin, const std::vector<std::string>& args, int timeout_ms,
                    std::string* out) {
  int fds[2];
  if (pipe(fds) != 0) {
    return false;
  }
  SetCloexec(fds[0]);
  SetCloexec(fds[1]);
  const int devnull = open("/dev/null", O_RDWR | O_CLOEXEC);
  if (devnull < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  posix_spawn_file_actions_t actions;
  if (posix_spawn_file_actions_init(&actions) != 0) {
    close(fds[0]);
    close(fds[1]);
    close(devnull);
    return false;
  }
  posix_spawn_file_actions_adddup2(&actions, devnull, STDIN_FILENO);
  posix_spawn_file_actions_adddup2(&actions, fds[1], STDOUT_FILENO);
  posix_spawn_file_actions_adddup2(&actions, devnull, STDERR_FILENO);
  // dup2 leaves the original fds[1] number open in the child alongside its
  // duplicate on stdout. Close it: if anything the child hands off to ever
  // inherited and held that extra reference, the parent would never see EOF
  // on the pipe and would sit out the full deadline for nothing.
  posix_spawn_file_actions_addclose(&actions, fds[1]);
  posix_spawn_file_actions_addclose(&actions, fds[0]);

  std::vector<char*> argv;
  argv.push_back(const_cast<char*>(bin.c_str()));
  for (const auto& a : args) {
    argv.push_back(const_cast<char*>(a.c_str()));
  }
  argv.push_back(nullptr);

  pid_t pid = 0;
  const int rc = posix_spawn(&pid, bin.c_str(), &actions, nullptr, argv.data(), environ);
  posix_spawn_file_actions_destroy(&actions);
  close(fds[1]);
  close(devnull);
  if (rc != 0) {
    close(fds[0]);
    return false;
  }

  const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
  bool timed_out = false;
  char buf[4096];
  while (true) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now());
    if (remaining.count() <= 0) {
      timed_out = true;
      break;
    }
    struct pollfd pfd = {fds[0], POLLIN, 0};
    const int pr = poll(&pfd, 1, static_cast<int>(remaining.count()));
    if (pr == 0) {
      timed_out = true;
      break;
    }
    if (pr < 0) {
      if (errno == EINTR) continue;
      break;
    }
    const ssize_t n = read(fds[0], buf, sizeof(buf));
    if (n > 0) {
      out->append(buf, static_cast<size_t>(n));
      continue;
    }
    if (n < 0 && errno == EINTR) continue;
    break;  // EOF or hard read error
  }
  close(fds[0]);

  if (timed_out) {
    kill(pid, SIGKILL);
  }

  // A raw blocking `waitpid` here could stall the input thread with no
  // bound: a poll/read error other than EINTR exits the loop above with
  // `timed_out` still false and no guarantee the child has actually exited.
  // posix_spawn above is called with a null attrp (no
  // POSIX_SPAWN_SETSIGDEF), so the child inherits the IME's signal
  // dispositions -- if SIGPIPE is ignored there, a broken pipe alone won't
  // kill it. Poll for exit with WNOHANG, and only past a grace window force
  // it; the final wait is bounded because a killed process reaps essentially
  // immediately.
  //
  // The grace window is its own, deliberately *not* the read deadline: EOF
  // normally arrives a hair before the child is reapable, so reusing the
  // deadline meant a query that finished microseconds inside it got SIGKILLed
  // and its perfectly good output thrown away, with a 5s backoff armed on top.
  const auto reap_deadline = Clock::now() + std::chrono::milliseconds(10);
  int status = 0;
  pid_t reaped = 0;
  while (true) {
    reaped = waitpid(pid, &status, timed_out ? 0 : WNOHANG);
    if (reaped == pid) break;
    if (reaped < 0) {
      if (errno == EINTR) continue;
      break;  // e.g. ECHILD: nothing left to wait for
    }
    // reaped == 0: still running and not timed out yet.
    if (Clock::now() >= reap_deadline) {
      kill(pid, SIGKILL);
      timed_out = true;
      continue;  // next iteration blocks; SIGKILL guarantees a fast exit
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  if (timed_out) {
    return false;
  }
  return reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

void SetTmuxTestHooks(FrontmostBundleIdFn frontmost, RunTmuxFn run) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_frontmost_hook = frontmost;
  g_run_hook = run;
}

}  // namespace tmux_detail

namespace {

std::string Frontmost() { return g_frontmost_hook ? g_frontmost_hook() : FrontmostBundleId(); }

bool RunTmux(const std::string& bin, const std::vector<std::string>& args, int timeout_ms,
             std::string* out) {
  return g_run_hook ? g_run_hook(bin, args, timeout_ms, out)
                    : tmux_detail::RunTmuxProcess(bin, args, timeout_ms, out);
}

// Requires g_mutex held. The gate runs against the live list rather than a
// copy: this is the hot path, and deep-copying ten bundle ids on every
// keystroke in every application -- before even knowing the app is a
// terminal -- was pure waste.
bool IsTerminalAppLocked(const std::string& frontmost) {
  return !frontmost.empty() &&
         std::find(g_config.app_bundle_ids.begin(), g_config.app_bundle_ids.end(), frontmost) !=
             g_config.app_bundle_ids.end();
}

// The socket string the current config resolves to; "" means tmux's default.
std::string CurrentSocket() {
  std::lock_guard<std::mutex> lock(g_mutex);
  return g_config.socket;
}

// Everything GetTmuxSurroundingText() did before ExtractContext: the config
// read, the frontmost gate, the binary probe, RunTmux, ParseTmuxOutput,
// JudgeClients, and the memo. Split out so the identity path shares every
// refusal that is about *reaching tmux*, and none of the ones about the text.
std::shared_ptr<const tmux_detail::Snapshot> AcquireSnapshot() {
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_config.enabled) {
      return nullptr;
    }
  }

  // The IME process is never frontmost while handling a key event, so this is
  // the app being typed into. Without this gate, any app whose text field
  // answers NSNotFound -- Electron, Java, assorted web inputs -- would be
  // handed the terminal's text. ConfigureTmuxSource guarantees the list is
  // non-empty, so a lookup miss means "not a terminal", not "unconfigured".
  //
  // Queried before the memo is consulted, and folded into the memo's key: it
  // is far cheaper than a spawn, and it means a snapshot can never survive
  // into a different application even if the generation somehow did.
  const std::string frontmost = Frontmost();
  // An empty id means "unknown app" (always true on non-Apple, and possible
  // on Apple too), not "matches everything". Without this check, a user
  // config with one empty app_bundle_ids entry -- or FrontmostBundleId()
  // simply not knowing -- would turn the gate into "always pass".
  if (frontmost.empty()) {
    return nullptr;
  }

  std::string binary;
  std::string configured_binary;
  std::string socket;
  int timeout_ms = 0;
  uint64_t generation = 0;
  bool probed_now = false;
  bool debug = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!IsTerminalAppLocked(frontmost)) {
      return nullptr;
    }
    if (g_snapshot_valid && g_snapshot_generation == g_generation &&
        g_snapshot_frontmost == frontmost) {
      return g_snapshot;
    }
    if (Clock::now() < g_backoff_until) {
      return nullptr;
    }
    configured_binary = g_config.binary;
    probed_now = ResolveBinaryLocked(configured_binary, &binary);
    socket = g_config.socket;
    timeout_ms = g_config.timeout_ms;
    generation = g_generation;
    debug = g_config.debug;
  }

  // Install a result for this generation. Skipped if a key event bumped the
  // generation while the query was in flight: that snapshot describes a pane
  // state the new generation has not vouched for, and a stale pane handed to
  // AutoSpacer is exactly the cross-talk this source exists to prevent.
  auto remember = [&](std::shared_ptr<const tmux_detail::Snapshot> value) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_generation == generation) {
      g_snapshot = value;
      g_snapshot_generation = generation;
      g_snapshot_frontmost = frontmost;
      g_snapshot_valid = true;
    }
    return value;
  };

  if (binary.empty()) {
    if (probed_now) {
      // One-shot: the probe result is cached until the next reconfigure, so
      // this cannot fire per keystroke. Without it a user whose tmux is not at
      // one of these paths enables the feature and gets nothing at all, with
      // no way to tell whether the gate, the binary or the parse failed.
      std::string paths;
      if (!configured_binary.empty()) {
        paths = configured_binary + " (from copilot/tmux_source/binary)";
      } else {
        for (const auto& candidate : tmux_detail::TmuxBinaryCandidates()) {
          if (!paths.empty()) paths += ", ";
          paths += candidate;
        }
      }
      LOG(WARNING) << "[tmux] No executable tmux found; the tmux surrounding source is off. "
                   << "Probed: " << paths
                   << ". Set copilot/tmux_source/binary to an absolute path.";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(60);
    return nullptr;
  }

  const std::vector<std::string> args = tmux_detail::BuildTmuxArgs(socket);

  std::string out;
  if (!RunTmux(binary, args, timeout_ms, &out)) {
    // Logged at the transition, not while polling: we only reach here when the
    // backoff has expired, so this is at most one line per backoff window.
    if (debug) {
      LOG(INFO) << "[tmux] Query failed (no server, timeout past " << timeout_ms
                << "ms, or non-zero exit); backing off 5s";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(5);
    return nullptr;
  }

  auto snap = tmux_detail::ParseTmuxOutput(out);
  if (!snap) {
    // A tmux that answers garbage will keep answering garbage.
    if (debug) {
      LOG(INFO) << "[tmux] Unparseable output (" << out.size() << " bytes); backing off 5s";
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(5);
    return nullptr;
  }

  // Transient, so no backoff: refuse this one query and try again next time.
  const auto verdict = tmux_detail::JudgeClients(snap->client_activity, snap->focus_events);
  if (verdict != tmux_detail::ClientVerdict::kAccept) {
    if (debug) {
      LOG(INFO) << "[tmux] Refusing to answer: " << tmux_detail::DescribeVerdict(verdict);
    }
    return remember(nullptr);
  }

  return remember(std::make_shared<tmux_detail::Snapshot>(std::move(*snap)));
}

}  // namespace

const std::vector<std::string>& DefaultTerminalBundleIds() {
  // org.alacritty / net.kovidgoyal.kitty / com.github.wez.wezterm /
  // com.apple.Terminal were read off installed bundles on 2026-08-12. The rest
  // are the published ids for apps not installed here; a wrong string only
  // means the feature stays off in that terminal, which is the safe direction.
  static const std::vector<std::string> ids = {
      "org.alacritty",           // Alacritty     (verified)
      "net.kovidgoyal.kitty",    // kitty         (verified)
      "com.github.wez.wezterm",  // WezTerm       (verified)
      "com.apple.Terminal",      // Apple Terminal (verified)
      "com.googlecode.iterm2",   // iTerm2
      "com.mitchellh.ghostty",   // Ghostty
      "co.zeit.hyper",           // Hyper
      "dev.warp.Warp-Stable",    // Warp
      "com.raphaelamorim.rio",   // Rio
      "org.tabby",               // Tabby
  };
  return ids;
}

void ConfigureTmuxSource(const TmuxSourceConfig& config) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_config = config;
  if (g_config.app_bundle_ids.empty()) {
    g_config.app_bundle_ids = DefaultTerminalBundleIds();
  }
  // Task 4 clamps at the config-read site too, but this function is the
  // public API and RunTmux is the deadline owner, so the clamp belongs here
  // as well -- the same belt-and-suspenders pattern as
  // copilot.cc's surrounding_context_chars clamp plus
  // imk_client.mm's SetIMKSurroundingPrefixChars clamp. Unclamped, 0 or
  // negative makes every query declare an instant timeout (feature
  // permanently dead, spawn+kill every 5s forever), and a large value turns
  // into a synchronous IME freeze on a keystroke. The lower bound matches
  // copilot.cc's clamp exactly; two different floors would mean the effective
  // minimum depended on which one ran last.
  g_config.timeout_ms = std::clamp(g_config.timeout_ms, kMinTimeoutMs, kMaxTimeoutMs);
  g_config.prefix_chars = std::clamp(g_config.prefix_chars, 1, 64);
  g_resolved_binary.clear();
  g_binary_resolved = false;
  g_backoff_until = Clock::time_point{};
  // Anything cached was produced under the old config -- a different socket,
  // depth, or gate.
  ++g_generation;
  g_snapshot_valid = false;
  g_snapshot.reset();
}

void InvalidateTmuxSnapshot() {
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_generation;
  g_snapshot_valid = false;
  g_snapshot.reset();
}

std::optional<SurroundingText> GetTmuxSurroundingText() {
  auto snap = AcquireSnapshot();
  if (!snap) {
    return std::nullopt;
  }

  int prefix_chars = 1;
  bool debug = false;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    prefix_chars = g_config.prefix_chars;
    debug = g_config.debug;
  }

  auto ctx = tmux_detail::ExtractContext(*snap, prefix_chars);
  if (!ctx) {
    if (debug) {
      LOG(INFO) << "[tmux] Cursor out of the captured pane (x=" << snap->cursor_x
                << ", y=" << snap->cursor_y << ", width=" << snap->pane_width
                << ", rows=" << snap->rows.size() << ")";
    }
    return std::nullopt;
  }

  const std::string client_key = tmux_detail::MakeClientKey(CurrentSocket(), snap->pane_id);
  SurroundingText result;
  result.before = ctx->before;
  result.after = ctx->after;
  result.client_key = client_key;
  result.before_depth = ctx->before_depth;
  result.after_depth = ctx->after_depth;
  result.truncation = ctx->truncation;
  return result;
}

std::optional<context_memory::Identity> GetTmuxPaneIdentity() {
  auto snap = AcquireSnapshot();  // the same memoized acquire GetTmuxSurroundingText uses
  if (!snap) return std::nullopt;
  context_memory::Identity id;
  // tmux's own answer first, the configured socket only as a fallback. The
  // pushed rung reports ${TMUX%%,*} -- an absolute path in every case,
  // including the default socket -- and the two rungs must build byte-identical
  // keys or a pane gets two slots and its mode is restored from whichever one
  // answered last. `CurrentSocket()` cannot supply that: it is empty on the
  // default socket. It stays the fallback for a tmux too old to know
  // `#{socket_path}`, which is the pre-existing behaviour.
  id.socket = snap->socket_path.empty() ? CurrentSocket() : snap->socket_path;
  id.pane_id = snap->pane_id;
  id.command = snap->pane_command;
  if (id.pane_id.empty()) return std::nullopt;
  return id;
}

bool FrontmostIsTerminal() {
  const std::string frontmost = Frontmost();
  std::lock_guard<std::mutex> lock(g_mutex);
  return IsTerminalAppLocked(frontmost);
}

}  // namespace rime
