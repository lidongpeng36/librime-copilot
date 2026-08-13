#pragma once

// Which application currently owns the keyboard.
//
// The IME process is never itself frontmost while handling a key event — the
// app owning the text field is — so this identifies the typing target. Used to
// gate the tmux surrounding source, which must never answer for a non-terminal
// app that merely failed its IMK query.

#include <string>

namespace rime {

#ifdef __APPLE__
std::string FrontmostBundleId();
#else
inline std::string FrontmostBundleId() { return ""; }
#endif

}  // namespace rime
