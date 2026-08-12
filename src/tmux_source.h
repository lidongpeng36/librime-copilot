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

struct TmuxSourceConfig {
  bool enabled = false;
  std::string binary;                       // empty -> probe well-known paths
  std::string socket;                       // empty -> tmux default socket
  std::vector<std::string> app_bundle_ids;  // empty -> DefaultTerminalBundleIds()
  int timeout_ms = 50;
  int prefix_chars = 1;
};

// Terminal emulators that may receive tmux-scraped context. Built in rather
// than config-only: the gate is a safety mechanism, and a user who has to
// hand-write bundle ids to get the feature working will be tempted to widen it
// carelessly. Narrow it in config to restrict; use `enabled: false` to disable.
const std::vector<std::string>& DefaultTerminalBundleIds();

void ConfigureTmuxSource(const TmuxSourceConfig& config);

// nullopt whenever the answer would be a guess: disabled, wrong frontmost app,
// tmux missing, query timed out, output unparseable, or two attached clients
// tied on activity.
std::optional<SurroundingText> GetTmuxSurroundingText();

}  // namespace rime
