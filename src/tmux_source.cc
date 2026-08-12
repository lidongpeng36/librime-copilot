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
  const char* candidates[] = {"/opt/homebrew/bin/tmux", "/usr/local/bin/tmux", "/usr/bin/tmux"};
  std::string found;
  if (!configured.empty() && access(configured.c_str(), X_OK) == 0) {
    found = configured;
  } else {
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
  int status = 0;
  while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {
  }
  if (timed_out) {
    return false;
  }
  return WIFEXITED(status) && WEXITSTATUS(status) == 0;
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

  // No -t anywhere: display-message and capture-pane must resolve the *same*
  // current pane, and letting tmux decide that is the entire point.
  std::vector<std::string> args;
  if (!config.socket.empty()) {
    args.push_back("-S");
    args.push_back(config.socket);
  }
  args.push_back("list-clients");
  args.push_back("-F");
  args.push_back("CLI|#{client_activity}");
  args.push_back(";");
  args.push_back("display-message");
  args.push_back("-p");
  args.push_back("-F");
  args.push_back("CUR|#{pane_id}|#{cursor_x}|#{cursor_y}|#{pane_width}");
  args.push_back(";");
  args.push_back("capture-pane");
  args.push_back("-p");

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
  if (tmux_detail::ClientsAreAmbiguous(snap->client_activity)) {
    DLOG(INFO) << "[tmux] Attached clients tied on activity; refusing to guess";
    return std::nullopt;
  }

  auto ctx = tmux_detail::ExtractContext(*snap, config.prefix_chars);
  if (!ctx) {
    return std::nullopt;
  }

  // The pane id MUST be in the key: AutoSpacer indexes per-client state by it
  // (src/auto_spacer.cc:282-286), so a constant key would let one pane's
  // spacing state bleed into the next.
  const std::string socket_tag = config.socket.empty() ? "default" : config.socket;
  const std::string client_key = "tmux:" + socket_tag + ":" + snap->pane_id;
  return SurroundingText{ctx->before, ctx->after, client_key};
}

}  // namespace rime
