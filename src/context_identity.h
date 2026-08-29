#pragma once

// "Where am I", resolved from an ordered list of sources.
//
// Shaped exactly like GetSurroundingContext() (surrounding_source.cc:25-66),
// which answers the neighbouring question "what text is around the caret": a
// sequence of ifs in priority order, first to answer wins, the answer tagged
// with its source. No class hierarchy -- the existing one has none either.

#include <optional>

#include "context_memory.h"

namespace rime {

enum class ContextIdentitySource { kNone, kBridge, kTmux };

struct ResolvedIdentity {
  context_memory::Identity id;
  ContextIdentitySource source = ContextIdentitySource::kNone;
};

// nullopt means "no identity finer than the application", which is what stops
// the memory feature from acting. There is deliberately no bundle-id rung:
// per-application ascii_mode is already correct without this feature, and
// keying on it would unify two input fields in one app that today hold
// independent modes.
std::optional<ResolvedIdentity> GetContextIdentity();

using IdentityFn = std::optional<context_memory::Identity> (*)();
using TerminalPredicateFn = bool (*)();
// Three nullptrs restore the real sources.
void SetContextIdentityTestHooks(IdentityFn bridge, IdentityFn tmux, TerminalPredicateFn terminal);

const char* DescribeIdentitySource(ContextIdentitySource source);

}  // namespace rime
