#include "context_identity.h"

#include <mutex>
#include <vector>

#include "ime_bridge.h"
#include "remote_binding.h"
#include "tmux_source.h"

namespace rime {
namespace {

IdentityFn g_bridge_hook = nullptr;
PendingBindFn g_pending_hook = nullptr;
IdentityFn g_tmux_hook = nullptr;
TerminalPredicateFn g_terminal_hook = nullptr;

context_memory::RemoteBinding g_binding;
std::mutex g_options_mutex;
std::vector<std::string> g_remote_commands{"ssh", "mosh", "et"};

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

std::optional<context_memory::PendingBind> TakePendingBind() {
  if (g_pending_hook) return g_pending_hook();
  return ImeBridgeServer::Instance().TakePendingBind();
}

}  // namespace

void SetContextIdentityTestHooks(IdentityFn bridge, PendingBindFn pending, IdentityFn tmux,
                                 TerminalPredicateFn terminal) {
  g_bridge_hook = bridge;
  g_pending_hook = pending;
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

void ResetRemoteBindingForTest() { g_binding.Clear(); }

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
  // Ahead of the frontmost gate on purpose. The gate decides whether to
  // BELIEVE a pushed identity -- the pushed cell has no expiry, so it must not
  // be trusted once the user is in another application. Binding is a different
  // claim ("local pane %15 has mini's %1 behind it") and is true whatever is
  // frontmost; deferring it behind the gate loses the binding whenever the
  // user tabs away and back by a route that produces no new push.
  //
  // The pair was captured on the bridge thread when the push arrived. Nothing
  // is polled here: asking tmux where the user is NOW is what bound a plain
  // `ssh` pane to another pane's remote tmux, permanently. See the 2026-09-08
  // design.
  if (auto pending = TakePendingBind()) {
    if (!pending->local_target.pane_id.empty() &&
        context_memory::IsRemoteCommand(RemoteCommands(), pending->local_target.command)) {
      g_binding.Bind(pending->local_target.pane_id, pending->local_target.command, pending->remote);
    }
  }

  // Priority 1: pushed by a tmux hook. Costs nothing per keystroke.
  //
  // Gated on the frontmost app because the pushed value is a cell with no
  // expiry: tmux tells us a pane was selected, never that the user has since
  // switched to another application.
  if (FrontmostIsTerminalOrHook()) {
    if (auto pushed = PushedIdentity()) {
      if (!pushed->pane_id.empty()) {
        if (!pushed->host.empty()) {
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
