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
#include <mutex>
#include <thread>

#include <rime/common.h>

#include "frontmost_app.h"
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

// An IMK process inherits a minimal PATH, and on a Homebrew Mac `tmux` is not
// on it at all, so absolute probing is the only thing that works.
std::string ResolveBinary(const std::string& configured) {
  if (g_binary_resolved) {
    return g_resolved_binary;
  }
  std::string found;
  if (!configured.empty()) {
    // An explicit path is a deliberate choice -- a wrapper, a different
    // install, one bound to a different socket namespace. Honor it or fail;
    // silently substituting a well-known path would mean reading a different
    // tmux server's panes and handing that text to AutoSpacer, which is
    // exactly the guessing this feature must refuse to do.
    if (access(configured.c_str(), X_OK) == 0) {
      found = configured;
    }
  } else {
    const char* candidates[] = {"/opt/homebrew/bin/tmux", "/usr/local/bin/tmux", "/usr/bin/tmux"};
    for (const char* c : candidates) {
      if (access(c, X_OK) == 0) {
        found = c;
        break;
      }
    }
  }
  g_resolved_binary = found;
  g_binary_resolved = true;
  return found;
}

// Spawn `bin` with `args`, collect stdout, and never block past `timeout_ms`.
// Runs on the input thread, so the deadline is load-bearing.
bool RunTmux(const std::string& bin, const std::vector<std::string>& args, int timeout_ms,
             std::string* out) {
  int fds[2];
  if (pipe(fds) != 0) {
    return false;
  }
  const int devnull = open("/dev/null", O_RDWR);
  if (devnull < 0) {
    close(fds[0]);
    close(fds[1]);
    return false;
  }

  posix_spawn_file_actions_t actions;
  posix_spawn_file_actions_init(&actions);
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
  // kill it. Poll for exit with WNOHANG up to the original deadline; only
  // past that force it, and the final wait is bounded because a killed
  // process reaps essentially immediately.
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
    if (Clock::now() >= deadline) {
      kill(pid, SIGKILL);
      timed_out = true;
      continue;  // next iteration blocks; SIGKILL guarantees a fast exit
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }

  if (timed_out) {
    return false;
  }
  return reaped == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
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
  // into a synchronous IME freeze on a keystroke.
  g_config.timeout_ms = std::clamp(g_config.timeout_ms, 1, 500);
  g_config.prefix_chars = std::clamp(g_config.prefix_chars, 1, 64);
  g_resolved_binary.clear();
  g_binary_resolved = false;
  g_backoff_until = Clock::time_point{};
}

std::optional<SurroundingText> GetTmuxSurroundingText() {
  TmuxSourceConfig config;
  std::string binary;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_config.enabled) {
      return std::nullopt;
    }
    if (Clock::now() < g_backoff_until) {
      return std::nullopt;
    }
    config = g_config;
  }

  // The IME process is never frontmost while handling a key event, so this is
  // the app being typed into. Without this gate, any app whose text field
  // answers NSNotFound -- Electron, Java, assorted web inputs -- would be
  // handed the terminal's text. ConfigureTmuxSource guarantees the list is
  // non-empty, so a lookup miss means "not a terminal", not "unconfigured".
  const std::string frontmost = FrontmostBundleId();
  // An empty id means "unknown app" (always true on non-Apple, and possible
  // on Apple too), not "matches everything". Without this check, a user
  // config with one empty app_bundle_ids entry -- or FrontmostBundleId()
  // simply not knowing -- would turn the gate into "always pass".
  if (frontmost.empty()) {
    return std::nullopt;
  }
  if (std::find(config.app_bundle_ids.begin(), config.app_bundle_ids.end(), frontmost) ==
      config.app_bundle_ids.end()) {
    return std::nullopt;
  }

  {
    std::lock_guard<std::mutex> lock(g_mutex);
    binary = ResolveBinary(config.binary);
  }
  if (binary.empty()) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(60);
    return std::nullopt;
  }

  const std::vector<std::string> args = tmux_detail::BuildTmuxArgs(config.socket);

  std::string out;
  if (!RunTmux(binary, args, config.timeout_ms, &out)) {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(5);
    return std::nullopt;
  }

  auto snap = tmux_detail::ParseTmuxOutput(out);
  if (!snap) {
    // A tmux that answers garbage will keep answering garbage.
    std::lock_guard<std::mutex> lock(g_mutex);
    g_backoff_until = Clock::now() + std::chrono::seconds(5);
    return std::nullopt;
  }

  // Transient, so no backoff: refuse this one query and try again next time.
  const auto verdict = tmux_detail::JudgeClients(snap->client_activity, snap->focus_events);
  if (verdict != tmux_detail::ClientVerdict::kAccept) {
    DLOG(INFO) << "[tmux] Refusing to answer: " << tmux_detail::DescribeVerdict(verdict);
    return std::nullopt;
  }

  auto ctx = tmux_detail::ExtractContext(*snap, config.prefix_chars);
  if (!ctx) {
    return std::nullopt;
  }

  const std::string client_key = tmux_detail::MakeClientKey(config.socket, snap->pane_id);
  return SurroundingText{ctx->before, ctx->after, client_key};
}

}  // namespace rime
