#include "surrounding_source.h"

#include <rime/common.h>

#include "ime_bridge.h"
#include "tmux_source.h"

namespace rime {

std::optional<SurroundingText> GetSurroundingContext() {
#ifdef __APPLE__
  // Priority 1: IMK Client (macOS system query)
  if (auto context = GetIMKSurroundingText()) {
    DLOG(INFO) << "[Surrounding] Using IMK context: before='" << context->before << "', after='"
               << context->after << "'";
    return context;
  }
#endif

  // Priority 2: ImeBridge (clients like Neovim), used only when IMK has no context.
  if (auto context = ImeBridgeServer::Instance().GetActiveContext()) {
    DLOG(INFO) << "[Surrounding] Using ImeBridge context: before='" << context->before
               << "', after='" << context->after << "'";
    return context;
  }

  // Priority 3: the active tmux pane. Below ImeBridge on purpose — a bridge
  // client reports its real edit buffer, which beats scraping the screen (no
  // prompt text, survives soft wrap). This catches everything else in the
  // terminal: the shell prompt, less, git commit, REPLs.
  if (auto context = GetTmuxSurroundingText()) {
    DLOG(INFO) << "[Surrounding] Using tmux context: before='" << context->before << "', after='"
               << context->after << "'";
    return context;
  }

  // Priority 4: no real context; the caller falls back to commit history.
  return std::nullopt;
}

}  // namespace rime
