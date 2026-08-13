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

// Gates the "which source won" logging below with LOG(INFO) rather than
// DLOG(INFO): the librime build this ships in compiles -DNDEBUG, under which
// DLOG is stripped entirely, so DLOG here would never print in the build a
// user actually installs. Driven by `copilot/surrounding_debug`, read once in
// Copilot's constructor -- read on the input thread, written once at startup,
// so a plain atomic is enough and avoids a mutex on every key event.
void SetSurroundingDebug(bool debug);

}  // namespace rime
