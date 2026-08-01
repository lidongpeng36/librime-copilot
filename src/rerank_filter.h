#pragma once

// Contextual candidate re-ranking, as a filter of our own.
//
// Promotes the candidate the db expects to follow the text before the caret:
// typing the syllable for 瓴 right after 高屋建 puts 瓴 first instead of 令.
//
// This deliberately does NOT use librime's grammar/contextual_suggestions
// pipeline. That path needs a component registered under the name "grammar",
// and librime-octagram — bundled with Squirrel — is loaded after this plugin
// and replaces it (Registry::Register overwrites on name collision). Owning
// the filter keeps us out of that fight, and lets us use the real text before
// the caret (IMK / IME Bridge) rather than librime's "last commit only".
//
// Re-ranking is skipped entirely unless a frontend supplies real surrounding
// text: with only commit history we cannot tell that the caret was moved by a
// mouse click, and a wrong promotion is worse than none.

#include <rime/filter.h>

#include <string>
#include <vector>

#include "copilot_db.h"
#include "provider.h"

namespace rime {

class CopilotEngineComponent;
struct Segment;

// The db's successors for each key of one context, longest key first. Shared
// with the translations that use it: a later keystroke replaces the filter's
// cache, and a menu that is still paging must keep reading the context it was
// built for.
using RerankContinuations = std::vector<std::vector<::copilot::Entry>>;

struct RerankOptions {
  bool enable = true;
  // Longest context key: at most this many Han characters before the caret.
  int max_context_chars = 8;
  // Only the first N candidates are considered.
  int window = 32;
  // A continuation ranked below this among its key's continuations never
  // promotes. Rank rather than weight share: merged dictionaries live on
  // different scales, and a key can have thousands of continuations.
  int max_rank = 50;
};

class CopilotRerankFilter : public Filter {
 public:
  CopilotRerankFilter(const Ticket& ticket, const an<CopilotDb>& db, const RerankOptions& options);

  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override;

  // Skip the prediction placeholder: those candidates are already db-ordered.
  bool AppliesToSegment(Segment* segment) override;

 private:
  // Continuations for each key of `context`, longest key first. Cached because
  // Apply() runs on every keystroke while the context only changes between
  // compositions.
  an<RerankContinuations> LookupContinuations(const std::string& context);

  an<CopilotDb> db_;
  RerankOptions options_;

  std::string cached_context_;
  an<RerankContinuations> cached_continuations_;
};

class CopilotRerankFilterComponent : public Filter::Component {
 public:
  explicit CopilotRerankFilterComponent(an<CopilotEngineComponent> engine_factory);
  virtual ~CopilotRerankFilterComponent();

  CopilotRerankFilter* Create(const Ticket& ticket) override;

 protected:
  an<CopilotEngineComponent> engine_factory_;
};

}  // namespace rime
