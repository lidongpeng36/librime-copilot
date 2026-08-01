#pragma once

#include <optional>
#include <string>
#include <unordered_map>

#include "copilot_plugin.h"
#include "imk_client.h"

namespace rime {

class Candidate;
class Context;

// True when `cand` — the candidate the user just picked — converts only a
// PREFIX of the current input, so Rime would confirm that segment and keep
// composing the rest (typing 云枢 as "yyuu" and picking 云, which spans "yy").
//
// The AutoSpacer must defer to Rime's Selector in that state instead of
// committing: Composition::GetCommitText() appends the unconverted tail
// verbatim, so committing would put "云uu" on screen and steal the user's
// chance to pick 枢.
bool SelectionLeavesUnconvertedInput(Context* ctx, const an<Candidate>& cand);

// Compute the text to commit when Space finalizes the current composition,
// including CJK/Latin auto-spacing against the surrounding `before`/`after`.
//
// The whole composition is committed (all selected segments concatenated via
// Context::GetCommitText), NOT just the last segment — committing only the
// last segment drops earlier selections of a long, multi-segment input.
//
// Declared here (rather than kept file-local) so it can be unit-tested with a
// hand-built Context, without standing up a full Rime engine.
std::string ComputeSpaceCommitText(Context* ctx, const std::string& before,
                                   const std::string& after, bool enable_right_space);

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
    std::string before;
    std::string after;
  };
  std::unordered_map<std::string, ClientState> client_states_;
  bool enable_right_space_ = true;
};

}  // namespace rime
