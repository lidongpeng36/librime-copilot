#ifndef RIME_PREDICT_ENGINE_H_
#define RIME_PREDICT_ENGINE_H_

#include <rime/component.h>
#include <rime/dict/db_pool.h>
#include "copilot_db.h"

#include "history.h"
#include "provider.h"

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
                std::shared_ptr<::copilot::History>& history, int max_iterations);
  virtual ~CopilotEngine();

  bool Copilot(Context* ctx, const string& context_query);
  void Clear();
  void CreateCopilotSegment(Context* ctx) const;

  int max_iterations() const { return max_iterations_; }
  const string& query() const { return query_; }

  const std::vector<::copilot::Entry>& candidates();

  std::shared_ptr<::copilot::History> history() const { return history_; }
  void BackSpace();

 private:
  int max_iterations_;  // copilot times limit
  string query_;        // cache last query

  std::vector<std::shared_ptr<Provider>> providers_;
  std::vector<::copilot::Entry> cands_;
  std::shared_ptr<::copilot::History> history_;
};

class CopilotEngineComponent : public CopilotEngine::Component {
 public:
  CopilotEngineComponent();
  virtual ~CopilotEngineComponent();

  CopilotEngine* Create(const Ticket& ticket) override;

  an<CopilotEngine> GetInstance(const Ticket& ticket);

 protected:
  map<string, weak<CopilotEngine>> copilot_engine_by_schema_id;
  DbPool<CopilotDb> db_pool_;
};

}  // namespace rime

#endif  // RIME_PREDICT_ENGINE_H_
