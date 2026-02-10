#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "copilot_plugin.h"
#include "imk_client.h"

namespace rime {

class Context;
class AutoSpacer : public CopilotPlugin<AutoSpacer> {
 public:
  explicit AutoSpacer(const Ticket& ticket);

  ProcessResult Process(const KeyEvent& key_event);

 private:
  ProcessResult Process(Context* ctx, const KeyEvent& key_event);

  // Path 1: Process with real surrounding context (completely independent)
  ProcessResult ProcessWithSurroundingContext(Context* ctx, const KeyEvent& key_event,
                                              const SurroundingText& surrounding,
                                              const std::string& client_key);

  // Path 2: Process with commit_history (original logic)
  ProcessResult ProcessWithCommitHistory(Context* ctx, const KeyEvent& key_event);

  ProcessResult HandleNumberKey(Context* ctx, const KeyEvent& key_event) const;

  // Get surrounding text with priority: ImeBridge > IMK Client > commit_history
  std::optional<SurroundingText> GetSurroundingText() const;

  struct ClientState {
    // Stores boundary when composition starts, used at commit time.
    // During composition, IMK context may reflect marked text position.
    std::string context_before_composition;
    std::string context_after_composition;
  };
  std::unordered_map<std::string, ClientState> client_states_;
  bool enable_right_space_ = true;
};

}  // namespace rime
