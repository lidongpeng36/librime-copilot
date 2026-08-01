#include "surrounding_source.h"

#include <rime/common.h>

#include "ime_bridge.h"

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

  // Priority 3: no real context; the caller falls back to commit history.
  return std::nullopt;
}

}  // namespace rime
