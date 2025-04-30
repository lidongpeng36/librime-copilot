#ifndef RIME_PREDICTOR_H_
#define RIME_PREDICTOR_H_

#include <rime/processor.h>

namespace rime {

class Context;
class CopilotEngine;
class CopilotEngineComponent;

class Copilot : public Processor {
 public:
  Copilot(const Ticket& ticket, an<CopilotEngine> copilot_engine);
  virtual ~Copilot();

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override;

 protected:
  void OnContextUpdate(Context* ctx);
  void OnSelect(Context* ctx);
  void CopilotAndUpdate(Context* ctx, const string& context_query);

 private:
  enum Action { kUnspecified, kSelect, kDelete, kSpecial };
  Action last_action_ = kUnspecified;
  bool self_updating_ = false;
  int iteration_counter_ = 0;  // times has been copiloted

  an<CopilotEngine> copilot_engine_;
  connection select_connection_;
  connection context_update_connection_;
  connection delete_connection_;
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

#endif  // RIME_PREDICTOR_H_
