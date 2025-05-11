#include <rime/component.h>
#include <rime/registry.h>
#include <rime_api.h>

#include "copilot.h"
#include "copilot_engine.h"
#include "copilot_translator.h"

#include "auto_spacer.h"
#include "filters.h"
#include "select_character.h"

using namespace rime;

static void rime_copilot_initialize() {
  Registry& r = Registry::instance();
  an<CopilotEngineComponent> engine_factory = New<CopilotEngineComponent>();
  r.Register("copilot", new CopilotComponent(engine_factory));
  r.Register("copilot_translator", new CopilotTranslatorComponent(engine_factory));

  r.Register("auto_spacer", new CopilotPluginComponent<AutoSpacer>());
  r.Register("select_character", new CopilotPluginComponent<SelectCharacter>());

  r.Register("auto_spacer_filter", new Component<AutoSpacerFilter>);
  r.Register("raw_input_filter", new Component<RawInputFilter>);
  r.Register("copilot_filter", new Component<CopilotFilter>);
}

static void rime_copilot_finalize() {}

RIME_REGISTER_MODULE(copilot)
