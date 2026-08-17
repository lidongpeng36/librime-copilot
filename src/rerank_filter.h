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

#include <rime/context.h>
#include <rime/filter.h>

#include <optional>
#include <string>
#include <vector>

#include "copilot_db.h"
#include "provider.h"
#include "rerank_llm.h"
#include "rerank_trace.h"
#include "scorer.h"
#include "utils.h"  // copilot::PowerChangeToken

namespace rime {

class CopilotEngine;
class CopilotEngineComponent;
struct Segment;

// One context key and the db's successors for it.
struct ContinuationSet {
  std::string key;
  int key_len = 0;  // in characters, not bytes
  std::vector<::copilot::Entry> entries;
};

// The db's successors for each key of one context, longest key first. Shared
// with the translations that use it: a later keystroke replaces the filter's
// cache, and a menu that is still paging must keep reading the context it was
// built for.
using RerankContinuations = std::vector<ContinuationSet>;

struct RerankOptions {
  bool enable = true;
  // Longest context key: at most this many Han characters before the caret.
  int max_context_chars = 8;
  // Only the first N candidates are considered.
  int window = 32;
  // A continuation ranked below this among its key's continuations never
  // promotes. Rank rather than weight share: merged dictionaries live on
  // different scales, and a key can have thousands of continuations.
  int max_rank = 10;
  // The LLM scoring path -- primary source when it can run; see rerank_llm.h.
  LlmRerankOptions llm;
};

class CopilotRerankFilter : public Filter {
 public:
  // `copilot_engine` supplies the Scorer (Task 5: it is owned by CopilotEngine
  // now, not by this filter, so the Copilot processor's warm-cache triggers
  // can reach the same instance). May be null -- e.g. rerank/enable is false,
  // so the caller never asked CopilotEngineComponent for an instance -- in
  // which case scoring is simply unavailable, same as before.
  CopilotRerankFilter(const Ticket& ticket, const an<CopilotDb>& db, const RerankOptions& options,
                      an<RerankTraceStore> traces, an<CopilotEngine> copilot_engine);
  ~CopilotRerankFilter() override;

  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override;

  // Skip the prediction placeholder: those candidates are already db-ordered.
  //
  // Also the one place the Segment is reachable. librime calls this immediately
  // before Menu::AddFilter(), which calls Apply() synchronously for the same
  // segment (engine.cc:225-226 -> menu.cc:22-24, the only call sites of
  // either), and Apply() itself receives only a Translation. So the segment's
  // extent is stashed here and consumed by the next Apply().
  bool AppliesToSegment(Segment* segment) override;

  // The span the next Apply() will key its trace on, exposed only so a test can
  // pin the writer's convention: constructing the filter needs no engine, but
  // reaching its Translation does.
  const std::optional<TraceSpan>& pending_trace_span() const { return pending_trace_span_; }

 private:
  // Continuations for each key of `context`, longest key first. Cached because
  // Apply() runs on every keystroke while the context only changes between
  // compositions.
  an<RerankContinuations> LookupContinuations(const std::string& context);

  an<CopilotDb> db_;
  RerankOptions options_;
  an<RerankTraceStore> traces_;
  // Keeps the Scorer (below) alive: CopilotEngine owns it, and this filter
  // may be the only thing holding a strong reference to that engine instance
  // (a schema can run the filter without the `copilot` processor at all) --
  // see CopilotEngine::scorer() and rerank_filter.cc's constructor.
  an<CopilotEngine> copilot_engine_;
  Scorer* scorer_ = nullptr;  // borrowed from copilot_engine_, never owned

  // Cached rather than queried on every keystroke -- same reason and same
  // pattern as LLMProvider (llm_provider.cc:63-78): kept current by a
  // process-wide power monitor callback instead of a syscall per Apply().
  // Assumed true (AC) until told otherwise, same fallback IsACPowerConnected()
  // itself uses when the platform cannot tell.
  bool is_on_ac_ = true;
  ::copilot::PowerChangeToken power_token_ = 0;

  // Set by AppliesToSegment, consumed by Apply. Consumed rather than merely
  // read: an Apply() that somehow arrived without a preceding
  // AppliesToSegment() must record no trace at all rather than key one on the
  // previous segment's span. Dropping an event is acceptable; misattributing
  // one is not. This is unconditional: AppliesToSegment also resets the span
  // on the path where it declines the segment (the "copilot" placeholder tag),
  // since that segment's Apply() never runs and would otherwise leave a stale
  // span for the next segment's Apply() to pick up.
  std::optional<TraceSpan> pending_trace_span_;

  // The segment AppliesToSegment saw, alongside pending_trace_span_ and reset
  // on exactly the same paths (including to nullptr when AppliesToSegment
  // declines a segment) — Apply() needs it to find where in the composition
  // "before this segment" ends, via ConfirmedPrefix (rerank.h).
  const Segment* pending_segment_ = nullptr;

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
