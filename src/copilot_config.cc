#include "copilot_config.h"
#include <filesystem>
#include "llm_scorer.h"
#include "rerank_filter.h"

#include <rime/config.h>

#include <algorithm>

#include "surrounding_source.h"  // kMaxSurroundingPrefixChars

namespace rime {

CopilotSharedConfig ReadCopilotSharedConfig(Config* config) {
  CopilotSharedConfig c;
  if (!config) {
    return c;
  }
  config->GetString("copilot/db", &c.db);
  config->GetBool("copilot/rerank/enable", &c.rerank_enable);
  config->GetInt("copilot/rerank/max_context_chars", &c.rerank_max_context_chars);
  config->GetBool("copilot/rerank/llm/enable", &c.llm_enable);
  config->GetString("copilot/rerank/llm/model", &c.llm_model);
  config->GetBool("copilot/rerank/llm/battery_active", &c.llm_battery_active);
  config->GetInt("copilot/rerank/llm/context_chars", &c.llm_context_chars);
  // Clamped as part of the read, not by each consumer afterwards: both of
  // these size the per-keystroke surrounding query (SurroundingPrefixChars),
  // and SurroundingPrefixChars clamps each term BEFORE taking the max so that
  // one out-of-range key cannot raise the fetch on behalf of a consumer that
  // would itself have clamped down. Clamping here means there is no unclamped
  // value in circulation for a future third reader to pick up.
  c.rerank_max_context_chars =
      std::clamp(c.rerank_max_context_chars, 1, kMaxSurroundingPrefixChars);
  c.llm_context_chars = std::clamp(c.llm_context_chars, 1, kMaxSurroundingPrefixChars);
  return c;
}

telemetry::Options ReadTelemetryOptions(Config* config) {
  telemetry::Options options;
  if (config) {
    config->GetBool("copilot/telemetry/enable", &options.enable);
    config->GetInt("copilot/telemetry/top_n", &options.top_n);
    // Config has GetInt but no GetInt64 (rime/config/config_component.h), so
    // this reads through an int seeded with the int64 default and assigns back
    // only when the key was present -- otherwise an absent key would narrow
    // the default through int on every read. Preserved exactly as both old
    // sites spelled it.
    int max_file_bytes = static_cast<int>(options.max_file_bytes);
    if (config->GetInt("copilot/telemetry/max_file_bytes", &max_file_bytes)) {
      options.max_file_bytes = max_file_bytes;
    }
    config->GetInt("copilot/telemetry/keep_generations", &options.keep_generations);
    config->GetInt("copilot/telemetry/sample_ok", &options.sample_ok);
    config->GetBool("copilot/telemetry/auto_sync", &options.auto_sync);
  }
  // Unconditional, including for a null config: CopilotComponent::Create
  // already clamped outside its own `if (schema/config)` block, and clamping
  // in-range defaults is a no-op, so this is the stricter of the two old
  // spellings rather than a behaviour change.
  telemetry::ClampOptions(options);
  return options;
}

nlohmann::json TelemetryConfig(Config* config, int fetch_chars) {
  const auto r = ReadRerankOptions(config);
  const auto t = ReadTelemetryOptions(config);
  LlmScorerOptions scorer;
  nlohmann::json filters = nlohmann::json::array();
  if (config) {
    config->GetInt("copilot/rerank/llm/n_gpu_layers", &scorer.n_gpu_layers);
    config->GetInt("copilot/rerank/llm/n_threads", &scorer.n_threads);
    if (auto list = config->GetList("engine/filters")) {
      for (size_t i = 0; i < list->size(); ++i) {
        std::string name;
        if (auto value = list->GetValueAt(i); value && value->GetString(&name)) {
          filters.push_back(name);
        }
      }
    }
  }
  return {{"rerank_enable", r.enable},
          {"window", r.window},
          {"max_rank", r.max_rank},
          {"max_context_chars", r.max_context_chars},
          {"same_span_only", r.same_span_only},
          {"llm_enable", r.llm.enable},
          {"model", std::filesystem::path(r.llm.model).filename().string()},
          {"battery_active", r.llm.battery_active},
          {"context_chars", r.llm.context_chars},
          {"require_han_context", r.llm.require_han_context},
          {"top_n", r.llm.top_n},
          {"margin", r.llm.margin},
          {"length_exponent", r.llm.length_exponent},
          {"n_gpu_layers", scorer.n_gpu_layers},
          {"n_threads", scorer.n_threads},
          {"backend", "llama"},
          {"fetch_chars", fetch_chars},
          {"sample_ok", t.sample_ok},
          {"telemetry_top_n", t.top_n},
          {"filters", filters}};
}

}  // namespace rime
