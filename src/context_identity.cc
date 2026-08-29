#include "context_identity.h"

#include "ime_bridge.h"
#include "tmux_source.h"

namespace rime {
namespace {

IdentityFn g_bridge_hook = nullptr;
IdentityFn g_tmux_hook = nullptr;
TerminalPredicateFn g_terminal_hook = nullptr;

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

const char* DescribeIdentitySource(ContextIdentitySource source) {
  switch (source) {
    case ContextIdentitySource::kBridge:
      return "bridge";
    case ContextIdentitySource::kTmux:
      return "tmux";
    default:
      return "none";
  }
}

std::optional<ResolvedIdentity> GetContextIdentity() {
  // Priority 1: pushed by the tmux hook. Costs nothing per keystroke.
  //
  // Gated on the frontmost app because the pushed value is a cell with no
  // expiry: tmux tells us a pane was selected, never that the user has since
  // switched to another application. Without this gate the memory table would
  // record another app's mode against the last tmux pane.
  if (FrontmostIsTerminalOrHook()) {
    if (auto pushed = PushedIdentity()) {
      if (!pushed->pane_id.empty()) {
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
