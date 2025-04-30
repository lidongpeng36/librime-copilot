#ifndef RIME_PREDICT_TRANSLATOR_H_
#define RIME_PREDICT_TRANSLATOR_H_

#include <rime/translator.h>

namespace rime {

class Context;
class CopilotEngine;
class CopilotEngineComponent;

class CopilotTranslator : public Translator {
 public:
  CopilotTranslator(const Ticket& ticket, an<CopilotEngine> copilot_engine);

  an<Translation> Query(const string& input, const Segment& segment) override;

 private:
  an<CopilotEngine> copilot_engine_;
};

class CopilotTranslatorComponent : public CopilotTranslator::Component {
 public:
  explicit CopilotTranslatorComponent(an<CopilotEngineComponent> engine_factory);
  virtual ~CopilotTranslatorComponent();

  CopilotTranslator* Create(const Ticket& ticket) override;

 protected:
  an<CopilotEngineComponent> engine_factory_;
};

}  // namespace rime

#endif  // RIME_PREDICT_TRANSLATOR_H_
