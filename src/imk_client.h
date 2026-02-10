// IMK Client - Access IMKTextInput client from within a librime plugin
// via ObjC runtime method swizzling (macOS only).
//
// This provides a way to query the text surrounding the cursor from
// IMKTextInput-compliant applications on macOS.

#ifndef RIME_COPILOT_IMK_CLIENT_H_
#define RIME_COPILOT_IMK_CLIENT_H_

#include <optional>
#include <string>

namespace rime {

// Text surrounding the cursor position
struct SurroundingText {
  std::string before;  // UTF-8 character immediately before cursor
  std::string after;   // UTF-8 character immediately after cursor
  // Client/app identity for per-app state isolation.
  // For ImeBridge, this is "app:instance". For IMK, this is a client pointer key.
  std::string client_key;
};

// Get surrounding text from the most recent IMK client interaction.
// Returns nullopt if no valid context is available.
// This function should be called during key event processing.
#ifdef __APPLE__
std::optional<SurroundingText> GetIMKSurroundingText();
#endif

}  // namespace rime

#endif  // RIME_COPILOT_IMK_CLIENT_H_
