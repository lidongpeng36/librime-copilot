#include "copilot_engine.h"

#include <filesystem>
#include <map>
#include <sstream>

#include <rime/candidate.h>
#include <rime/context.h>
#include <rime/deployer.h>
#include <rime/dict/db_pool_impl.h>
#include <rime/engine.h>
#include <rime/key_event.h>
#include <rime/menu.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/service.h>
#include <rime/ticket.h>
#include <rime/translation.h>

#include "db_provider.h"
#include "llm_provider.h"
#include "llm_scorer.h"
#include "utils.h"

namespace rime {

static const ResourceType kCopilotDbResourceType = {"copilot_db", "", ""};

CopilotEngine::CopilotEngine(std::vector<std::shared_ptr<Provider>> providers,
                             std::shared_ptr<::copilot::History>& history, int max_iterations,
                             std::unique_ptr<Scorer> scorer)
    : providers_(std::move(providers)),
      history_(history),
      max_iterations_(max_iterations),
      scorer_(std::move(scorer)) {
  if (providers_.empty()) {
    LOG(ERROR) << "CopilotEngine: no providers";
  }
}

CopilotEngine::~CopilotEngine() {}

bool CopilotEngine::Copilot(Context* ctx, const string& context_query,
                            const string& surrounding_context) {
  // LOG(INFO) << "CopilotEngine::Copilot [" << context_query << "]";
  // history_->add(context_query);
  bool ret = false;
  for (auto& provider : providers_) {
    ret |= provider->Predict(context_query, surrounding_context);
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

namespace {

inline void SortByWeightDesc(std::vector<::copilot::Entry>& entries) {
  std::stable_sort(
      entries.begin(), entries.end(),
      [](const ::copilot::Entry& a, const ::copilot::Entry& b) { return a.weight > b.weight; });
}

}  // namespace

std::vector<::copilot::Entry> MergeProviderCandidates(std::vector<RankedCandidates> per_provider) {
  std::vector<::copilot::Entry> merged;
  std::vector<RankedCandidates*> ranked;
  for (auto& contribution : per_provider) {
    if (contribution.entries.empty()) {
      continue;
    }
    if (contribution.rank < 0) {
      merged.insert(merged.end(), contribution.entries.begin(), contribution.entries.end());
    } else {
      ranked.push_back(&contribution);
    }
  }
  SortByWeightDesc(merged);

  // Insert the pinned providers low-rank-first so each index refers to the
  // list as it looked before the later (further down) insertions.
  std::stable_sort(
      ranked.begin(), ranked.end(),
      [](const RankedCandidates* a, const RankedCandidates* b) { return a->rank < b->rank; });
  for (auto* contribution : ranked) {
    SortByWeightDesc(contribution->entries);
    size_t pos = std::min(static_cast<size_t>(contribution->rank), merged.size());
    merged.insert(merged.begin() + pos, contribution->entries.begin(), contribution->entries.end());
  }
  return merged;
}

const std::vector<::copilot::Entry>& CopilotEngine::candidates() {
  std::vector<RankedCandidates> per_provider;
  per_provider.reserve(providers_.size());
  for (auto& provider : providers_) {
    per_provider.push_back({provider->Rank(), provider->Retrive(200'000)});
  }
  cands_ = MergeProviderCandidates(std::move(per_provider));

  /*
  for (size_t i = 0; i < cands_.size(); ++i) {
    if (cands_[i].text.empty()) {
      continue;
    }
    size_t n = std::min(i + 15, cands_.size());
    std::stringstream ss;
    for (int j = i; j < n; ++j) {
      ss << "\n* " << j + 1 << ":" << cands_[j];
    }
    LOG(INFO) << "candidates:" << ss.str();
    break;
  }
  */

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
  int max_iterations = 0;

  DBProvider::Config db_config;
  LLMProvider::Config llm_config;
  string model_name = "";
  // copilot/rerank/*: same three keys CopilotRerankFilterComponent::Create
  // reads to decide whether to build a Scorer -- kept in lockstep with it so
  // "off by default" holds here too and the filter's LOG line ("llm.model=ok"
  // when a scorer exists) still matches reality.
  // Defaults true: a schema that names copilot/llm/model and expects
  // predictions keeps getting them. Setting it false is how a schema stops
  // paying for a model it never uses -- see the construction site below.
  bool llm_enable = true;
  bool rerank_enable = true;
  bool rerank_llm_enable = false;
  string rerank_llm_model;
  LlmScorerOptions rerank_llm_scorer;
  if (auto* schema = ticket.schema) {
    auto* config = schema->config();
    if (config->GetString("copilot/db", &db_name)) {
      LOG(INFO) << "custom copilot/db: " << db_name;
    }
    if (!config->GetInt("copilot/max_candidates", &db_config.max_candidates)) {
      LOG(INFO) << "copilot/max_candidates is not set in schema";
    }
    if (!config->GetInt("copilot/max_hints", &db_config.max_hints)) {
      LOG(INFO) << "copilot/max_hints is not set in schema";
    }
    if (!config->GetInt("copilot/max_iterations", &max_iterations)) {
      LOG(INFO) << "copilot/max_iterations is not set in schema";
    }
    if (config->GetString("copilot/llm/model", &model_name)) {
      config->GetInt("copilot/llm/max_history", &llm_config.max_history);
      config->GetInt("copilot/llm/n_predict", &llm_config.n_predict);
      config->GetInt("copilot/llm/rank", &llm_config.rank);
      config->GetInt("copilot/llm/n_ctx", &llm_config.n_ctx);
      config->GetBool("copilot/llm/battery_active", &llm_config.battery_active);
      config->GetBool("copilot/llm/enable", &llm_enable);
    }
    config->GetBool("copilot/rerank/enable", &rerank_enable);
    config->GetBool("copilot/rerank/llm/enable", &rerank_llm_enable);
    config->GetString("copilot/rerank/llm/model", &rerank_llm_model);
    // Where the model runs. Absent, both keep the values that were hard-coded
    // before tools/bench_scorer.cc made them measurable; see LlmScorerOptions.
    config->GetInt("copilot/rerank/llm/n_gpu_layers", &rerank_llm_scorer.n_gpu_layers);
    config->GetInt("copilot/rerank/llm/n_threads", &rerank_llm_scorer.n_threads);
  }
  std::shared_ptr<::copilot::History> history = std::make_shared<::copilot::History>(100);
  // `enable` gates CONSTRUCTION, not just output, and that distinction is the
  // whole point: LLMProvider's constructor loads the model and runs a warm-up
  // prediction immediately (llm_provider.cc), so a schema that merely names a
  // model paid for it whether or not the `copilot` switch was ever turned on.
  // Measured on the deployed setup: 4.38 GB resident for a feature the user
  // had never enabled.
  //
  // Defaults to true, so a schema that names a model and expects predictions
  // keeps getting them; turning this off is the explicit way to stop paying.
  if (!model_name.empty() && llm_enable) {
    auto r =
        the<ResourceResolver>(Service::instance().CreateResourceResolver(kCopilotLLMResourceType));
    auto model_path = r->ResolvePath(model_name);
    if (std::filesystem::exists(model_path)) {
      LOG(INFO) << "[copilot] LLM: " << model_path;
      llm_config.model = model_path;
      providers.push_back(std::make_shared<LLMProvider>(llm_config, history));
    }
  }
  if (!model_name.empty() && !llm_enable) {
    LOG(INFO) << "[copilot] LLM: disabled by copilot/llm/enable; model not loaded";
  }
  if (auto db = db_pool_.GetDb(db_name)) {
    if (db->IsOpen() || db->Load()) {
      LOG(INFO) << "[copilot] DB: " << db_name;
      providers.push_back(std::make_shared<DBProvider>(db, history, db_config));
    } else {
      LOG(ERROR) << "failed to load copilot db: " << db_name;
    }
  }
  // Lazy-loading (llm_scorer.h): the ~1 GB model is not touched until the
  // first WarmUp(), so building this even when rerank ends up off at runtime
  // for battery reasons costs nothing but the pointer.
  //
  // Resolved through the SAME ResourceResolver as copilot/llm/model above --
  // `rerank_llm_model` is a config string like "private/Qwen3-0.6B-q4_K_M.gguf",
  // relative to the user's Rime directory, not to whatever the process's cwd
  // happens to be. Passing it to LlmScorer unresolved (as this did before)
  // means llama_model_load_from_file() only succeeds if cwd happens to equal
  // the Rime user dir -- true for no real deployment and for no offline
  // harness run, so the LLM re-rank path silently never loads a model at all.
  std::unique_ptr<Scorer> scorer;
  if (rerank_enable && rerank_llm_enable && !rerank_llm_model.empty()) {
    auto r =
        the<ResourceResolver>(Service::instance().CreateResourceResolver(kCopilotLLMResourceType));
    auto model_path = r->ResolvePath(rerank_llm_model);
    if (std::filesystem::exists(model_path)) {
      scorer = std::make_unique<LlmScorer>(model_path, rerank_llm_scorer);
    } else {
      LOG(ERROR) << "[copilot] rerank llm: model not found at " << model_path
                 << " (copilot/rerank/llm/model: " << rerank_llm_model << ")";
    }
  }
  // A scorer with no db/predictor providers is a real, supported
  // configuration -- Task 4 established that a null db must not disable the
  // LLM re-ranking path, and this component is now the scorer's only source
  // (moved off CopilotRerankFilter in Task 5). Returning null here whenever
  // `providers` is empty would silently drop a successfully-built scorer
  // along with it, turning LLM re-ranking off in exactly the configuration
  // Task 4 protected.
  if (!providers.empty() || scorer) {
    return new CopilotEngine(providers, history, max_iterations, std::move(scorer));
  }
  return nullptr;
}

an<CopilotDb> CopilotEngineComponent::GetDb(const string& db_name) {
  auto db = db_pool_.GetDb(db_name);
  if (!db) {
    return nullptr;
  }
  if (!db->IsOpen() && !db->Load()) {
    return nullptr;
  }
  return db;
}

an<RerankTraceStore> CopilotEngineComponent::GetRerankTraces(const string& schema_id) {
  auto& traces = rerank_traces_by_schema_id_[schema_id];
  if (!traces) {
    traces = New<RerankTraceStore>();
  }
  return traces;
}

namespace {

// Only the fields Writer::Write/Rotate actually read from the frozen
// Options — NOT top_n. top_n never reaches the writer at all: OnCommit
// passes its own freshly-read, per-schema Options straight to
// BuildCommitEvents, so a later schema's top_n is always honored regardless
// of what the writer was built with. Including it here would warn about an
// override that never happens.
string DescribeTelemetryOptions(const telemetry::Options& options) {
  std::ostringstream oss;
  oss << "enable=" << (options.enable ? "true" : "false")
      << ", max_file_bytes=" << options.max_file_bytes
      << ", keep_generations=" << options.keep_generations;
  return oss.str();
}

}  // namespace

an<telemetry::Writer> CopilotEngineComponent::GetTelemetryWriter(
    const telemetry::Options& options) {
  auto& deployer = Service::instance().deployer();
  const string machine = telemetry::MachineName(deployer.user_id);
  // The writer's name is fixed at construction (it is what the path is derived
  // from) and this writer is process-wide, so a deployment that changes
  // installation_id leaves it writing the previous machine's file -- while
  // EmitCommitTelemetry, which re-reads user_id per commit, stamps every line
  // with the new name. That is not hypothetical: it ran for four days and
  // 221 lines on this author's laptop (see telemetry.h's MachineName). Rebuild
  // rather than warn, so the filename follows the config; the old writer stays
  // alive in whatever Copilot still holds it and flushes its tail to the old
  // file on destruction, which is where that data belongs.
  if (telemetry_writer_ && telemetry_writer_->machine() != machine) {
    LOG(ERROR) << "[copilot] telemetry: installation_id changed " << telemetry_writer_->machine()
               << " -> " << machine << "; reopening under the new name (was "
               << telemetry_writer_->path() << ")";
    telemetry_writer_.reset();
  }
  if (!telemetry_writer_) {
    telemetry_writer_options_ = options;
    // Under private/: see README "Telemetry" for why — a Rime user directory
    // is commonly a git repo of the user's own config on top of upstream, and
    // private/ is the conventional gitignore line, so this transcript of the
    // user's input cannot be committed by accident.
    telemetry_writer_ = New<telemetry::Writer>(
        deployer.user_data_dir / "private" / "copilot_telemetry", machine, options);
    LOG(INFO) << "[copilot] telemetry: enable=" << options.enable
              << ", file=" << telemetry_writer_->path();
    return telemetry_writer_;
  }

  // The writer is process-wide, built once from the first schema's options
  // (see the field comment on telemetry_writer_options_); every later
  // caller's `enable`/`max_file_bytes`/`keep_generations` is silently
  // overridden — including `enable`, which means a schema that turns
  // telemetry on can end up writing nothing at all if an earlier-loaded
  // schema turned it off. Make that observable, but only once per distinct
  // mismatch: a warning that repeats on every deploy is noise people learn to
  // ignore. `top_n` is deliberately not compared here; see
  // DescribeTelemetryOptions.
  if (options.enable != telemetry_writer_options_.enable ||
      options.max_file_bytes != telemetry_writer_options_.max_file_bytes ||
      options.keep_generations != telemetry_writer_options_.keep_generations) {
    string ignored = DescribeTelemetryOptions(options);
    if (telemetry_mismatches_logged_.insert(ignored).second) {
      LOG(WARNING) << "[copilot] telemetry: options differ across schemas; the writer is "
                      "process-wide and the first schema loaded wins for the whole process. "
                      "In effect: "
                   << DescribeTelemetryOptions(telemetry_writer_options_)
                   << ". Ignored (this schema wanted): " << ignored;
    }
  }
  return telemetry_writer_;
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

namespace {
// Function-local static: avoids static-initialization-order questions
// between this translation unit and copilot_module.cc's rime_copilot_initialize
// (which is the only writer, see SetCopilotEngineComponentForTools).
an<CopilotEngineComponent>& ComponentForToolsSlot() {
  static an<CopilotEngineComponent> instance;
  return instance;
}
}  // namespace

void SetCopilotEngineComponentForTools(an<CopilotEngineComponent> component) {
  ComponentForToolsSlot() = std::move(component);
}

an<CopilotEngineComponent> GetCopilotEngineComponentForTools() { return ComponentForToolsSlot(); }

}  // namespace rime
