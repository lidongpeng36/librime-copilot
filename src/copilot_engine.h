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
