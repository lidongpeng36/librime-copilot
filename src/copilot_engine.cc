#include "copilot_engine.h"

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include "copilot_db.h"

#include "db_provider.h"
#include "llm_provider.h"

namespace rime {

static const ResourceType kCopilotDbResourceType = {"copilot_db", "", ""};

CopilotEngine::CopilotEngine(std::vector<std::shared_ptr<Provider>> providers,
                             std::shared_ptr<::copilot::History>& history, int max_iterations)
    : providers_(std::move(providers)), history_(history), max_iterations_(max_iterations) {
  if (providers_.empty()) {
    LOG(ERROR) << "CopilotEngine: no providers";
  }
}

CopilotEngine::~CopilotEngine() {}

bool CopilotEngine::Copilot(Context* ctx, const string& context_query) {
  // LOG(INFO) << "CopilotEngine::Copilot [" << context_query << "]";
  // history_->add(context_query);
  bool ret = false;
  for (auto& provider : providers_) {
    ret |= provider->Predict(context_query);
  }
  if (ret) {
    query_ = context_query;
  }
  return ret;
}

void CopilotEngine::Clear() {
  DLOG(INFO) << "CopilotEngine::Clear";
  query_.clear();
  cands_.clear();
  for (auto& provider : providers_) {
    provider->Clear();
  }
}

void CopilotEngine::CreateCopilotSegment(Context* ctx) const {
  // DLOG(INFO) << "CopilotEngine::CreateCopilotSegment";
  int end = int(ctx->input().length());
  Segment segment(end, end);
  segment.tags.insert("copilot");
  segment.tags.insert("placeholder");
  ctx->composition().AddSegment(segment);
  ctx->composition().back().tags.erase("raw");
  // DLOG(INFO) << "segments: " << ctx->composition();
}

void CopilotEngine::BackSpace() {
  history_->clear();
  query_.clear();
  // history_->pop();
  // query_ = history_->back();
  DLOG(INFO) << "CopilotEngine::BackSpace [" << query_ << "]";
  cands_.clear();
  for (auto& provider : providers_) {
    provider->OnBackspace();
  }
}

const std::vector<::copilot::Entry>& CopilotEngine::candidates() {
  // if (!cands_.empty()) {
  //   return cands_;
  // }
  cands_.clear();
  for (auto& provider : providers_) {
    auto cands = provider->Retrive(200'000);
    if (!cands.empty()) {
      cands_.insert(cands_.end(), cands.begin(), cands.end());
    }
  }
  std::sort(cands_.begin(), cands_.end(), [](const ::copilot::Entry& a, const ::copilot::Entry& b) {
    return a.weight < b.weight;
  });
  return cands_;
}

static const ResourceType kCopilotLLMResourceType = {"", "", ""};

CopilotEngineComponent::CopilotEngineComponent()
    : db_pool_(the<ResourceResolver>(
          Service::instance().CreateResourceResolver(kCopilotDbResourceType))) {}

CopilotEngineComponent::~CopilotEngineComponent() {}

CopilotEngine* CopilotEngineComponent::Create(const Ticket& ticket) {
  std::vector<std::shared_ptr<Provider>> providers;
  string db_name = "copilot.db";
  int max_candidates = 0;
  int max_iterations = 0;

  string model_name = "";
  int n_predict = 8;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    if (config->GetString("copilot/db", &db_name)) {
      LOG(INFO) << "custom copilot/db: " << db_name;
    }
    if (!config->GetInt("copilot/max_candidates", &max_candidates)) {
      LOG(INFO) << "copilot/max_candidates is not set in schema";
    }
    if (!config->GetInt("copilot/max_iterations", &max_iterations)) {
      LOG(INFO) << "copilot/max_iterations is not set in schema";
    }
    if (config->GetString("copilot/model", &model_name)) {
      config->GetInt("copilot/n_predict", &n_predict);
    }
  }
  std::shared_ptr<::copilot::History> history = std::make_shared<::copilot::History>(100);
  if (!model_name.empty()) {
    auto r =
        the<ResourceResolver>(Service::instance().CreateResourceResolver(kCopilotLLMResourceType));
    auto model_path = r->ResolvePath(model_name);
    if (std::filesystem::exists(model_path)) {
      LOG(INFO) << "[copilot] LLM: " << model_path;
      providers.push_back(std::make_shared<LLMProvider>(model_path, history, n_predict));
    }
  }
  if (auto db = db_pool_.GetDb(db_name)) {
    if (db->IsOpen() || db->Load()) {
      LOG(INFO) << "[copilot] DB: " << db_name;
      providers.push_back(std::make_shared<DBProvider>(db, max_candidates));
    } else {
      LOG(ERROR) << "failed to load copilot db: " << db_name;
    }
  }
  if (!providers.empty()) {
    return new CopilotEngine(providers, history, max_iterations);
  }
  return nullptr;
}

an<CopilotEngine> CopilotEngineComponent::GetInstance(const Ticket& ticket) {
  if (Schema* schema = ticket.schema) {
    auto found = copilot_engine_by_schema_id.find(schema->schema_id());
    if (found != copilot_engine_by_schema_id.end()) {
      if (auto instance = found->second.lock()) {
        return instance;
      }
    }
    an<CopilotEngine> new_instance{Create(ticket)};
    if (new_instance) {
      copilot_engine_by_schema_id[schema->schema_id()] = new_instance;
      return new_instance;
    }
  }
  return nullptr;
}

}  // namespace rime
