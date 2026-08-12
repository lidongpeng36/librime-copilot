#pragma once

// Single place that answers "what text surrounds the caret right now?".
//
// Two subsystems consume it — AutoSpacer (CJK/Latin boundary spacing) and the
// prediction path (n-gram lookup context) — so the source priority lives here
// instead of being duplicated per caller.

#include <optional>

#include "imk_client.h"  // SurroundingText

namespace rime {

// Priority: IMK client (macOS system query) > ImeBridge client (e.g. Neovim) >
// tmux pane scrape (terminal emulators the IMK query can never answer for) >
// nullopt. A nullopt result means no real context is available and the caller
// must fall back to its own history.
std::optional<SurroundingText> GetSurroundingContext();

}  // namespace rime
