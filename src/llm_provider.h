#pragma once

#include <future>
#include <memory>
#include <string>

#include "history.h"
#include "provider.h"
#include "utils.h"

namespace llama {
class ClientSimple;
}  // namespace llama

namespace rime {

class LLMProvider : public Provider {
 public:
  struct Config {
    std::string model;
    int max_history = 10;
    int n_predict = 8;
    int rank = 5;
    // KV cache size in tokens; see ClientConfig::n_ctx for what 0 costs.
    int n_ctx = 512;
    bool battery_active = false;
  };
  LLMProvider(const Config& config, const std::shared_ptr<::copilot::History>& history);
  virtual ~LLMProvider();

  // Provider interface
  void OnBackspace() override {}
  // Drops the pending/completed inference. Without this override the base
  // no-op ran instead, so a finished future kept handing the same sentence to
  // whatever context came next.
  void Clear() override;
  int Rank() const override { return config_.rank; }
  // Prediction context is not wired into the LLM prompt yet; it keeps using
  // the plugin's own commit history.
  bool Predict(const std::string& input, const std::string& context) override;
  std::vector<::copilot::Entry> Retrive(int timeout_us) const override;

 private:
  std::shared_ptr<::copilot::History> history_;

  Config config_;
  std::atomic<bool> is_on_ac_{true};
  ::copilot::PowerChangeToken power_token_ = 0;

  std::unique_ptr<llama::ClientSimple> client_;
  std::shared_ptr<std::promise<std::string>> promise_;
  std::shared_future<std::string> future_;
};

}  // namespace rime
