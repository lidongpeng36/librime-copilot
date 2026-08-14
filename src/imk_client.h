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

// Which subsystem answered "what text surrounds the caret?". Lives here rather
// than in surrounding_source.h because that header already includes this one
// for SurroundingText, and the reverse include would be a cycle.
enum class SurroundingSource { kNone, kIMK, kBridge, kTmux };

inline const char* SurroundingSourceName(SurroundingSource source) {
  switch (source) {
    case SurroundingSource::kIMK:
      return "imk";
    case SurroundingSource::kBridge:
      return "bridge";
    case SurroundingSource::kTmux:
      return "tmux";
    case SurroundingSource::kNone:
      break;
  }
  return "none";
}

// Text surrounding the cursor position
struct SurroundingText {
  // UTF-8 text immediately before the cursor. At least the boundary character
  // (all AutoSpacer looks at); up to SetIMKSurroundingPrefixChars() characters
  // when the prediction path asks for more and the client can answer.
  std::string before;
  std::string after;  // UTF-8 character immediately after cursor
  // Client/app identity for per-app state isolation.
  // For ImeBridge, this is "app:instance". For IMK, this is a client pointer key.
  std::string client_key;
  // Which source in the priority chain produced this snapshot. Set by
  // GetSurroundingContext(); AutoSpacer ignores it, the telemetry records it.
  SurroundingSource source = SurroundingSource::kNone;
};

// Get surrounding text from the most recent IMK client interaction.
// Returns nullopt if no valid context is available.
// This function should be called during key event processing.
#ifdef __APPLE__
std::optional<SurroundingText> GetIMKSurroundingText();

// How many characters before the caret the hook should fetch (default 1, which
// is all AutoSpacer needs). The prediction path raises it when
// copilot/use_surrounding_context is on. Clamped to [1, 64].
void SetIMKSurroundingPrefixChars(int n);
#endif

}  // namespace rime

#endif  // RIME_COPILOT_IMK_CLIENT_H_
