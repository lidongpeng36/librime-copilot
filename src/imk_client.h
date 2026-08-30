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
enum class SurroundingSource { kNone, kIMK, kBridge, kTmux, kReconstructed };

inline const char* SurroundingSourceName(SurroundingSource source) {
  switch (source) {
    case SurroundingSource::kIMK:
      return "imk";
    case SurroundingSource::kBridge:
      return "bridge";
    case SurroundingSource::kTmux:
      return "tmux";
    case SurroundingSource::kReconstructed:
      return "reconstructed";
    case SurroundingSource::kNone:
      break;
  }
  return "none";
}

// Why `before` stopped where it did. Exactly one answer per fetch.
//
// kUnknown is deliberately the zero: a source that forgets to set this reports
// "I do not know" rather than confidently reporting kFull. It is also the honest
// answer for the ImeBridge, which receives a string over a socket and has no way
// to learn whether the client had more -- until step (c) puts `want_before` in
// the greeting and a `truncation` field in the `context` action.
//
// An input region of EXACTLY the requested length is NOT kByConfig: the region
// ended on its own and nothing was cut. That tie is written down because it is
// the one case three independently-written sources would each answer
// differently, and comparability across sources is this field's entire value.
enum class Truncation {
  kUnknown = 0,  // the source cannot say
  kFull,         // the input region ended; nothing was cut
  kByConfig,     // the source had more and the requested length capped it
  kByApp,        // the application answered less than was asked (IMK)
  kByScreen,     // the source cannot see further (tmux)
};

inline const char* TruncationName(Truncation t) {
  switch (t) {
    case Truncation::kFull:
      return "full";
    case Truncation::kByConfig:
      return "config";
    case Truncation::kByApp:
      return "app";
    case Truncation::kByScreen:
      return "screen";
    case Truncation::kUnknown:
      break;
  }
  return "unknown";
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
  // How many CHARACTERS (code points, not bytes and not UTF-16 units) each side
  // actually carries, and why `before` stopped. Recorded rather than inferred:
  // "how much did the source really get" has never been a readable number, and
  // every remaining question in this area -- should tmux cross hard newlines,
  // is the Han-context gate worth lifting -- is a question about this
  // distribution. See the 2026-08-23 surrounding-context design.
  int before_depth = 0;
  int after_depth = 0;
  Truncation truncation = Truncation::kUnknown;
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
