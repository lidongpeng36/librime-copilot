#include "copilot_translator.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/translation.h>
#include "copilot_engine.h"

namespace rime {

CopilotTranslator::CopilotTranslator(const Ticket& ticket, an<CopilotEngine> copilot_engine)
    : Translator(ticket), copilot_engine_(copilot_engine) {}

an<Translation> CopilotTranslator::Query(const string& input, const Segment& segment) {
  if (!copilot_engine_) {
    return nullptr;
  }
  // LOG(INFO) << "[copilot] CopilotTranslator::Query: " << input;
  if (copilot_engine_->query().empty()) {
    return nullptr;
  }
  if (!segment.HasTag("copilot")) {
    return nullptr;
  }
  const auto& candidates = copilot_engine_->candidates();
  if (candidates.empty()) {
    return nullptr;
  }
  auto translation = New<FifoTranslation>();
  for (const auto& c : candidates) {
    if (c.text.empty()) continue;
    size_t end = segment.end;
    std::string comment = "";
    if (c.type == ::copilot::ProviderType::kLLM) {
      comment = u8"𝓛";
    }
    translation->Append(New<SimpleCandidate>("copilot", end, end, c.text, comment));
  }
  return translation;
}

CopilotTranslatorComponent::CopilotTranslatorComponent(an<CopilotEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

CopilotTranslatorComponent::~CopilotTranslatorComponent() {}

CopilotTranslator* CopilotTranslatorComponent::Create(const Ticket& ticket) {
  return new CopilotTranslator(ticket, engine_factory_->GetInstance(ticket));
}

}  // namespace rime
