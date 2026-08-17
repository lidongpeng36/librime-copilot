#ifndef RIME_PREDICT_ENGINE_H_
#define RIME_PREDICT_ENGINE_H_

#include <rime/component.h>
#include <rime/dict/db_pool.h>
#include "copilot_db.h"

#include <set>

#include "history.h"
#include "provider.h"
#include "rerank_trace.h"
#include "scorer.h"
#include "telemetry.h"

namespace rime {

class Context;
struct Segment;
struct Ticket;
class Translation;

// One provider's contribution to the candidate list.
struct RankedCandidates {
  // 0-based display position to pin the entries at; < 0 means "unranked",
  // i.e. merged into the weight-sorted pool (Provider::Rank()'s default).
  int rank = -1;
  std::vector<::copilot::Entry> entries;
};

// Merge the providers' candidates into the order the user sees (the result
// feeds a FifoTranslation verbatim).
//
// Unranked entries come first-by-likelihood: the whole data pipeline treats a
// HIGHER weight as more likely (make_copilot_data filters *below*
// --filter-weight and sorts descending; DBProvider keeps the top-N by weight),
// so they are sorted descending. Ranked providers are then inserted at their
// index, clamped to the end of the list.
//
// Declared here so the ordering can be unit-tested without providers, a db or
// a Rime engine.
std::vector<::copilot::Entry> MergeProviderCandidates(std::vector<RankedCandidates> per_provider);

class CopilotEngine : public Class<CopilotEngine, const Ticket&> {
 public:
  CopilotEngine(std::vector<std::shared_ptr<Provider>> providers,
                std::shared_ptr<::copilot::History>& history, int max_iterations,
                std::unique_ptr<Scorer> scorer = nullptr);
  virtual ~CopilotEngine();

  // `context_query` is what gates the prediction (query() must stay non-empty
  // for CopilotTranslator to translate). `surrounding_context` is the real text
  // before the caret when available, empty otherwise — see Provider::Predict.
  bool Copilot(Context* ctx, const string& context_query, const string& surrounding_context = {});
  void Clear();
  void CreateCopilotSegment(Context* ctx) const;

  int max_iterations() const { return max_iterations_; }
  const string& query() const { return query_; }

  const std::vector<::copilot::Entry>& candidates();

  std::shared_ptr<::copilot::History> history() const { return history_; }
  void BackSpace();

  // The re-ranking filter's scorer, shared here (not owned by the filter)
  // because the Copilot processor -- a separate component -- also needs to
  // reach it to warm the context on commit and at composition start (Task 5).
  // Null when rerank/llm is disabled or unconfigured: every caller must treat
  // that as "nothing to warm or score against", not an error.
  Scorer* scorer() const { return scorer_.get(); }

 private:
  int max_iterations_;  // copilot times limit
  string query_;        // cache last query

  std::vector<std::shared_ptr<Provider>> providers_;
  std::vector<::copilot::Entry> cands_;
  std::shared_ptr<::copilot::History> history_;
  std::unique_ptr<Scorer> scorer_;
};

class CopilotEngineComponent : public CopilotEngine::Component {
 public:
  CopilotEngineComponent();
  virtual ~CopilotEngineComponent();

  CopilotEngine* Create(const Ticket& ticket) override;

  an<CopilotEngine> GetInstance(const Ticket& ticket);

  // Opened-and-loaded db from the shared pool, or null. Exposed so the
  // re-ranking filter can score against the same file the prediction path
  // reads, without mapping it a second time.
  an<CopilotDb> GetDb(const string& db_name);

  // The re-ranking decisions for one schema, shared between the filter that
  // makes them and the processor that reads them at commit. Per schema, not a
  // file-level global, so two schemas cannot read each other's decisions.
  an<RerankTraceStore> GetRerankTraces(const string& schema_id);

  // The telemetry writer. One per process, not per schema: every schema writes
  // the same machine's file. Created on first request with the options of the
  // first caller.
  an<telemetry::Writer> GetTelemetryWriter(const telemetry::Options& options);

 protected:
  map<string, weak<CopilotEngine>> copilot_engine_by_schema_id;
  DbPool<CopilotDb> db_pool_;
  map<string, an<RerankTraceStore>> rerank_traces_by_schema_id_;
  an<telemetry::Writer> telemetry_writer_;
  // The options the writer above was actually built with (the first caller's),
  // kept so later callers whose options differ can be diagnosed instead of
  // silently overridden. See GetTelemetryWriter.
  telemetry::Options telemetry_writer_options_;
  // Distinct mismatch descriptions already warned about, so a schema that
  // reloads with the same losing options on every deploy warns once, not
  // every time.
  std::set<string> telemetry_mismatches_logged_;
};

// Lets a measurement tool (replay_copilot's --wait-for-warm) reach the SAME
// CopilotEngineComponent the schema's own processors were built from --
// rime_copilot_initialize() (copilot_module.cc) creates exactly one and
// shares it across CopilotComponent/CopilotTranslatorComponent/
// CopilotRerankFilterComponent, so GetInstance(ticket) here resolves to the
// identical, already-live CopilotEngine (and its Scorer) those components
// are using, not a second one. That is what lets the tool force-warm the
// re-ranking scorer and block until it reports hot, ahead of feeding the
// keystrokes that will consult it -- replay has none of the 1-2 seconds of
// real typing the fire-and-forget warm triggers (Copilot::WarmRerankContext)
// depend on, so left alone every request measures the cold/db fallback path.
// Not read by any runtime schema/processor code path; only replay_copilot.cc
// calls the getter.
void SetCopilotEngineComponentForTools(an<CopilotEngineComponent> component);
an<CopilotEngineComponent> GetCopilotEngineComponentForTools();

}  // namespace rime

#endif  // RIME_PREDICT_ENGINE_H_
