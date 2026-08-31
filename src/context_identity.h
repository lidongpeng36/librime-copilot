#pragma once

// "Where am I", resolved from an ordered list of sources.
//
// Shaped exactly like GetSurroundingContext() (surrounding_source.cc:25-66),
// which answers the neighbouring question "what text is around the caret": a
// sequence of ifs in priority order, first to answer wins, the answer tagged
// with its source. No class hierarchy -- the existing one has none either.

#include <optional>
#include <string>
#include <vector>

#include "context_memory.h"
#include "remote_binding.h"

namespace rime {

// kBridgeRemote / kBoundRemote are NOT decoration. Every way this feature
// fails is silent, and the `[ctxmem] A -> B via <source>` line is the only
// instrument there is; reporting "bridge" for all three would make the one
// interesting question -- did the binding fire? -- unanswerable from the log.
enum class ContextIdentitySource { kNone, kBridge, kBridgeRemote, kBoundRemote, kTmux };

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

// The commands a local pane must be running before a remote identity is
// bound behind it, and the bound table's LRU bound. Read from
// copilot/context_memory/remote_commands and .../max_entries.
//
// `max_entries` is taken RAW -- the configured int, straight off
// Config::GetInt, not pre-clamped by the caller -- and clamped internally via
// ClampMaxEntries below before it ever becomes a size_t. That used to be the
// caller's job (copilot.cc clamped in place, then cast); the clamp moved in
// here so that this function's own test can drive a hostile configured value
// through the SAME call the production code makes, rather than the test
// re-deriving the clamped value itself and never observing whether the real
// call site still clamps.
void SetRemoteBindingOptions(std::vector<std::string> remote_commands, int max_entries);

// Clamps a configured `copilot/context_memory/max_entries` to >= 1.
//
// Extracted to a free function -- rather than left as an inline clamp at the
// Config::GetInt call site -- so this exact invariant can be pinned by a
// test without constructing a real Copilot, which needs the Engine that
// Engine::Create() pulls a whole Switcher/deployer init for. The bug this
// guards: `context_memory::Options::max_entries` (context_memory_step.h) is
// an int, and every consumer of it -- SetRemoteBindingOptions above,
// context_memory::Table::set_max_entries via context_memory::Step's
// constructor -- takes it as a size_t. A negative int cast to size_t
// directly does not clamp, it wraps to a value near SIZE_MAX, which both
// RemoteBinding::SetMaxEntries and Table::set_max_entries then read as "no
// bound" -- the exact unbounded table each class exists to prevent. Every
// caller that turns the configured int into that size_t must clamp through
// here FIRST, not after. SetRemoteBindingOptions above does this itself;
// copilot.cc still calls it separately for its own copy of
// context_memory_options_.max_entries, which feeds context_memory::Table via
// context_memory::Step's constructor and has no clamp of its own.
int ClampMaxEntries(int configured);

// Drops every binding. Tests only -- the table is process-wide, exactly like
// context_memory::Instance(), so one test's binding would leak into the next.
void ResetRemoteBindingForTest();

const char* DescribeIdentitySource(ContextIdentitySource source);

}  // namespace rime
