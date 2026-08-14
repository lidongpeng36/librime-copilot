#pragma once

#include <rime/processor.h>

#include "rerank_trace.h"
#include "telemetry.h"

namespace rime {

class Context;
class CopilotEngine;
class CopilotEngineComponent;

class Copilot : public Processor {
 public:
  Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine,
          an<RerankTraceStore> rerank_traces, an<telemetry::Writer> telemetry,
          const telemetry::Options& telemetry_options);
  virtual ~Copilot();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 protected:
  void OnContextUpdate(Context* ctx);
  void OnSelect(Context* ctx);
  void CopilotAndUpdate(Context* ctx, const string& context_query);
  void OnCommit(Context* ctx);

 private:
  ProcessResult RunProcessors(const KeyEvent& key_event);
  // Text before the caret to predict from: the frontend's surrounding-text
  // snapshot plus `committed`, or empty when no real context is available (the
  // engine then falls back to the plugin's own commit history).
  string GetPredictionContext(const string& committed) const;

  enum Action { kUnspecified, kSelect, kDelete, kSpecial };
  Action last_action_ = kUnspecified;
  bool self_updating_ = false;
  int iteration_counter_ = 0;  // times has been copiloted

  an<CopilotEngine> copilot_engine_;
  connection select_connection_;
  connection context_update_connection_;
  connection delete_connection_;
  connection commit_connection_;
  an<RerankTraceStore> rerank_traces_;
  an<telemetry::Writer> telemetry_;
  telemetry::Options telemetry_options_;

  int last_keycode_ = 0;
  bool use_surrounding_context_ = true;
  int surrounding_context_chars_ = 8;
  std::vector<std::shared_ptr<Processor>> processors_;
};

class CopilotComponent : public Copilot::Component {
 public:
  explicit CopilotComponent(an<CopilotEngineComponent> engine_factory);
  virtual ~CopilotComponent();

  Copilot* Create(const Ticket& ticket) override;

 protected:
  an<CopilotEngineComponent> engine_factory_;
};

}  // namespace rime
