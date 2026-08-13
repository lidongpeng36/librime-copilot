#pragma once

// Surrounding text scraped from the active tmux pane.
//
// Exists because terminal emulators built on winit (Alacritty, and every other
// winit app) hardcode `selectedRange = NSNotFound` and
// `attributedSubstringForProposedRange: = nil`, so the IMK query in
// imk_client.mm can never answer for them. tmux does know, and can be asked
// from outside any pane.
//
// Ranks below ImeBridge on purpose: a client like the Neovim plugin reports its
// real buffer, which beats scraping the screen. See surrounding_source.cc.

#include <optional>
#include <string>
#include <vector>

#include "imk_client.h"  // SurroundingText

namespace rime {

// The single pair of bounds for `timeout_ms`. Both the schema-config reader in
// copilot.cc and ConfigureTmuxSource clamp with these; two different floors
// would mean the effective minimum depended on which clamp ran last.
constexpr int kMinTimeoutMs = 5;
constexpr int kMaxTimeoutMs = 500;

struct TmuxSourceConfig {
  bool enabled = false;
  std::string binary;                       // empty -> probe well-known paths
  std::string socket;                       // empty -> a full path for tmux -S
  std::vector<std::string> app_bundle_ids;  // empty -> DefaultTerminalBundleIds()
  int timeout_ms = 50;
  int prefix_chars = 1;
};

// Terminal emulators that may receive tmux-scraped context, used when the
// schema leaves `app_bundle_ids` empty. Built in rather than config-only so
// the out-of-the-box behavior is safe without the user hand-writing bundle
// ids. A configured list *replaces* this one rather than intersecting with it:
// editing schema YAML is a considered act, and intersecting would lock out
// terminals that are simply not on this list (custom builds, forks, newly
// released emulators). VS Code and other Electron editors are off it on
// purpose -- their editor panes would get the scrape along with their
// integrated terminals, and a Monaco buffer is not a tmux pane.
const std::vector<std::string>& DefaultTerminalBundleIds();

void ConfigureTmuxSource(const TmuxSourceConfig& config);

// Drop the memoized pane snapshot, so the next GetTmuxSurroundingText() runs a
// fresh query.
//
// Unlike the other two surrounding sources this one is a `posix_spawn`, not a
// cached-struct read, and it is consulted several times per key event --
// AutoSpacer, the re-ranking filter's menu build, and the prediction path each
// ask independently. Uncached that is 2-3 forks per keystroke on the input
// thread, arrow keys and Cmd-chords included.
//
// So the snapshot is memoized and Copilot::ProcessKeyEvent invalidates it once
// per key event, which collapses that to one query. It skips the invalidation
// while the context is composing: nothing has been committed to the PTY yet,
// so the pane provably cannot have changed, and the steady state becomes one
// query per composition -- the exact analogue of imk_client.mm's
// ClientIsComposing early-return, and what the design's 5ms measurement was
// justified on.
//
// Why no snapshot can outlive its usefulness: the only caller that can turn
// this source on is ConfigureTmuxSource, and the only caller of *that* is the
// Copilot processor's constructor -- the same object whose ProcessKeyEvent
// bumps the generation. A schema without the `copilot` processor never
// configures the source, so it stays disabled and never caches anything.
// Reconfiguring also drops the snapshot, and the frontmost bundle id is part
// of the memo key, so a snapshot cannot cross an application boundary either.
void InvalidateTmuxSnapshot();

// nullopt whenever the answer would be a guess: disabled, wrong frontmost app,
// tmux missing, query timed out, output unparseable, no attached client,
// multiple clients without `focus-events on`, or attached clients tied on
// activity. Answers from the memoized snapshot when one is current; see
// InvalidateTmuxSnapshot.
std::optional<SurroundingText> GetTmuxSurroundingText();

}  // namespace rime
