#include "surrounding_source.h"

#include <atomic>

#include <rime/common.h>

#include "ime_bridge.h"
#include "tmux_source.h"

namespace rime {

namespace {
// See SetSurroundingDebug in surrounding_source.h for why this is LOG(INFO)
// gated by an atomic rather than DLOG.
std::atomic<bool> g_surrounding_debug{false};
}  // namespace

void SetSurroundingDebug(bool debug) {
  g_surrounding_debug.store(debug, std::memory_order_relaxed);
}

std::optional<SurroundingText> GetSurroundingContext() {
  const bool debug = g_surrounding_debug.load(std::memory_order_relaxed);
#ifdef __APPLE__
  // Priority 1: IMK Client (macOS system query)
  if (auto context = GetIMKSurroundingText()) {
    context->source = SurroundingSource::kIMK;
    if (debug) {
      LOG(INFO) << "[Surrounding] Using IMK context: before='" << context->before << "', after='"
                << context->after << "', before_depth=" << context->before_depth
                << ", after_depth=" << context->after_depth
                << ", truncation=" << TruncationName(context->truncation);
    }
    return context;
  }
#endif

  // Priority 2: ImeBridge (clients like Neovim), used only when IMK has no context.
  if (auto context = ImeBridgeServer::Instance().GetActiveContext()) {
    context->source = SurroundingSource::kBridge;
    if (debug) {
      LOG(INFO) << "[Surrounding] Using ImeBridge context: before='" << context->before
                << "', after='" << context->after << "', before_depth=" << context->before_depth
                << ", after_depth=" << context->after_depth
                << ", truncation=" << TruncationName(context->truncation);
    }
    return context;
  }

  // Priority 3: the active tmux pane. Below ImeBridge on purpose — a bridge
  // client reports its real edit buffer, which beats scraping the screen (no
  // prompt text, survives soft wrap). This catches everything else in the
  // terminal: the shell prompt, less, git commit, REPLs.
  if (auto context = GetTmuxSurroundingText()) {
    context->source = SurroundingSource::kTmux;
    if (debug) {
      LOG(INFO) << "[Surrounding] Using tmux context: before='" << context->before << "', after='"
                << context->after << "', before_depth=" << context->before_depth
                << ", after_depth=" << context->after_depth
                << ", truncation=" << TruncationName(context->truncation);
    }
    return context;
  }

  // Priority 4: no real context; the caller falls back to commit history.
  return std::nullopt;
}

}  // namespace rime
