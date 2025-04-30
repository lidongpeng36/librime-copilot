#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "copilot.h"
#include "copilot_engine.h"
#include "copilot_translator.h"

using namespace rime;

static void rime_copilot_initialize() {
  Registry& r = Registry::instance();
  an<CopilotEngineComponent> engine_factory = New<CopilotEngineComponent>();
  r.Register("copilot", new CopilotComponent(engine_factory));
  r.Register("copilot_translator", new CopilotTranslatorComponent(engine_factory));
}

static void rime_copilot_finalize() {}

RIME_REGISTER_MODULE(copilot)
