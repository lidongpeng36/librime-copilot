#include "context_identity.h"

#include <mutex>
#include <vector>

#include "ime_bridge.h"
#include "remote_binding.h"
#include "tmux_source.h"

namespace rime {
namespace {

IdentityFn g_bridge_hook = nullptr;
IdentityFn g_tmux_hook = nullptr;
TerminalPredicateFn g_terminal_hook = nullptr;

context_memory::RemoteBinding g_binding;
std::mutex g_options_mutex;
std::vector<std::string> g_remote_commands{"ssh", "mosh", "et"};
// The remote identity most recently bound. Compared, not consumed: a remote
// pane switch pushes once and is then read on every keystroke until the next
// switch, and re-polling the local pane on each of those would put a
// posix_spawn on every key.
std::optional<context_memory::Identity> g_last_bound;

std::vector<std::string> RemoteCommands() {
  std::lock_guard<std::mutex> lock(g_options_mutex);
  return g_remote_commands;
}

std::optional<context_memory::Identity> PushedIdentity() {
  if (g_bridge_hook) return g_bridge_hook();
  return ImeBridgeServer::Instance().GetPushedIdentity();
}

std::optional<context_memory::Identity> PolledIdentity() {
  if (g_tmux_hook) return g_tmux_hook();
  return GetTmuxPaneIdentity();
}

bool FrontmostIsTerminalOrHook() {
  if (g_terminal_hook) return g_terminal_hook();
  return FrontmostIsTerminal();
}

}  // namespace

void SetContextIdentityTestHooks(IdentityFn bridge, IdentityFn tmux, TerminalPredicateFn terminal) {
  g_bridge_hook = bridge;
  g_tmux_hook = tmux;
  g_terminal_hook = terminal;
}

void SetRemoteBindingOptions(std::vector<std::string> remote_commands, int max_entries) {
  {
    std::lock_guard<std::mutex> lock(g_options_mutex);
    g_remote_commands = std::move(remote_commands);
  }
  g_binding.SetMaxEntries(static_cast<size_t>(ClampMaxEntries(max_entries)));
}

int ClampMaxEntries(int configured) { return configured < 1 ? 1 : configured; }

void ResetRemoteBindingForTest() {
  g_binding.Clear();
  g_last_bound.reset();
}

const char* DescribeIdentitySource(ContextIdentitySource source) {
  switch (source) {
    case ContextIdentitySource::kBridge:
      return "bridge";
    case ContextIdentitySource::kBridgeRemote:
      return "bridge-remote";
    case ContextIdentitySource::kBoundRemote:
      return "bound-remote";
    case ContextIdentitySource::kTmux:
      return "tmux";
    default:
      return "none";
  }
}

std::optional<ResolvedIdentity> GetContextIdentity() {
  // Priority 1: pushed by a tmux hook. Costs nothing per keystroke.
  //
  // Gated on the frontmost app because the pushed value is a cell with no
  // expiry: tmux tells us a pane was selected, never that the user has since
  // switched to another application.
  if (FrontmostIsTerminalOrHook()) {
    if (auto pushed = PushedIdentity()) {
      if (!pushed->pane_id.empty()) {
        if (!pushed->host.empty()) {
          // A remote pane switch. Bind it behind whichever local pane the
          // user is sitting in -- correct by construction, because a remote
          // switch happens while they are in the local pane whose ssh
          // carries it.
          //
          // This is the ONLY place that binding may happen. The bridge's
          // connection thread must not do it: the FrontmostIsTerminalOrHook()
          // above reaches [[NSWorkspace sharedWorkspace] frontmostApplication]
          // (frontmost_app.mm:11), and AppKit off the main thread is not
          // documented as safe.
          if (!g_last_bound || *g_last_bound != *pushed) {
            if (auto local = PolledIdentity()) {
              if (!local->pane_id.empty() &&
                  context_memory::IsRemoteCommand(RemoteCommands(), local->command)) {
                g_binding.Bind(local->pane_id, local->command, *pushed);
                g_last_bound = *pushed;
              }
            }
          }
          return ResolvedIdentity{*pushed, ContextIdentitySource::kBridgeRemote};
        }
        // A local pane switch. If something remote was bound behind this
        // pane, and the pane is still running the command it was bound
        // under, that remote pane is where the caret actually is.
        if (auto bound = g_binding.Lookup(pushed->pane_id, pushed->command)) {
          return ResolvedIdentity{*bound, ContextIdentitySource::kBoundRemote};
        }
        return ResolvedIdentity{*pushed, ContextIdentitySource::kBridge};
      }
    }
  }

  // Priority 2: polled from the memoized tmux snapshot. Applies its own
  // frontmost gate internally.
  if (auto polled = PolledIdentity()) {
    if (!polled->pane_id.empty()) {
      return ResolvedIdentity{*polled, ContextIdentitySource::kTmux};
    }
  }

  return std::nullopt;
}

}  // namespace rime
