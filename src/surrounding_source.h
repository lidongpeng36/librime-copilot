#pragma once

// Single place that answers "what text surrounds the caret right now?".
//
// Two subsystems consume it — AutoSpacer (CJK/Latin boundary spacing) and the
// prediction path (n-gram lookup context) — so the source priority lives here
// instead of being duplicated per caller.

#include <algorithm>
#include <optional>

#include "imk_client.h"  // SurroundingText

namespace rime {

// Every surrounding-text source clamps its own fetch depth to this, and so
// does the max below, so no consumer can ask for more than the sources will
// ever return. Matches tmux_source.cc's clamp and copilot.cc's on
// `surrounding_context_chars`.
inline constexpr int kMaxSurroundingPrefixChars = 64;

// What each consumer of the surrounding text declares it will read. The
// sources fetch once per key event and hand the same string to all of them,
// so the depth they must fetch is the deepest declaration among the consumers
// that are actually on.
struct SurroundingConsumers {
  // AutoSpacer needs exactly one character (the boundary) and is always on;
  // `use_surrounding_context` is the prediction path's own switch, and with it
  // off the per-key query stays exactly as cheap as it was before any of this.
  bool use_surrounding_context = true;
  int surrounding_context_chars = 8;
  // `copilot/rerank/enable`. Both re-ranking consumers live inside
  // CopilotRerankFilter, which returns early on `!options_.enable`
  // (rerank_filter.cc:448), so neither can read anything when this is false.
  bool rerank_enable = true;
  // The db path's Han-only n-gram tail -- 8, sized for n-gram keys.
  int rerank_max_context_chars = 8;
  // `copilot/rerank/llm/enable`, checked separately inside the filter
  // (rerank_filter.cc:487) and defaulting to false (rerank_llm.h:21).
  bool llm_enable = false;
  // The SCORER's context -- longer than the db's, because an LM reads
  // punctuation and Latin, not just the Han run.
  int llm_context_chars = 32;
};

// How deep the sources must fetch. Folding `llm_context_chars` in here is what
// makes `copilot/rerank/llm/context_chars` an actual fetch depth rather than a
// declaration nothing honours: until it was a term in this max, the sources
// stopped at 8 whatever the schema said, and telemetry's kByConfig bucket --
// "the source HAD more and prefix_chars cut it" -- held 71% of all fetches
// with a configured 32 in the schema.
//
// Each term is clamped before the max, not after, so one out-of-range key
// cannot raise the fetch on behalf of a consumer that would itself have
// clamped down. The floor is 1 rather than 0 because AutoSpacer always needs
// the boundary character.
inline int SurroundingPrefixChars(const SurroundingConsumers& c) {
  auto bounded = [](int n) { return std::clamp(n, 1, kMaxSurroundingPrefixChars); };
  int chars = c.use_surrounding_context ? bounded(c.surrounding_context_chars) : 1;
  if (c.rerank_enable) {
    chars = std::max(chars, bounded(c.rerank_max_context_chars));
    if (c.llm_enable) chars = std::max(chars, bounded(c.llm_context_chars));
  }
  return chars;
}

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
