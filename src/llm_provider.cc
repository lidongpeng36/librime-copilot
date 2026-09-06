#include "llm_provider.h"

#include <glog/logging.h>

#include "llm.h"
#include "utils.h"

namespace rime {

namespace {
inline std::string StripAndNormalize(const std::string& input) {
  size_t start = 0;
  size_t end = input.size();

  // 去掉前导空白
  while (start < end && std::isspace(static_cast<unsigned char>(input[start]))) {
    ++start;
  }
  // 去掉尾部空白
  while (end > start && std::isspace(static_cast<unsigned char>(input[end - 1]))) {
    --end;
  }

  std::string result;
  result.reserve(end - start);  // 提前分配内存，避免多次扩容

  for (size_t i = start; i < end; ++i) {
    char c = input[i];
    if (c == '\n' || c == '\r') {
      result.push_back(' ');
    } else {
      result.push_back(c);
    }
  }

  return result;
}
}  // namespace

LLMProvider::LLMProvider(const Config& c, const std::shared_ptr<::copilot::History>& history)
    : config_(c), history_(history) {
  --config_.rank;
  ClientConfig config;
  config.n_predict = c.n_predict;
  config.n_ctx = c.n_ctx;
  LOG(INFO) << "LLM model: '" << config_.model << "', n_predict:" << config_.n_predict
            << ", rank:" << config_.rank;
  client_ = std::make_unique<llama::ClientSimple>(config, config_.model,
                                                  [this](const std::string& response) {
                                                    if (promise_) {
                                                      promise_->set_value(response);
                                                    }
                                                  });
  client_->commit("WarmUp");
  client_->clear();
  if (!config_.battery_active) {
    is_on_ac_ = copilot::IsACPowerConnected();
    power_token_ = copilot::RegisterPowerChange([this](bool is_ac_power) {
      if (is_ac_power != is_on_ac_) {
        is_on_ac_ = is_ac_power;
        DLOG(INFO) << "[LLM]: AC Power Connected:" << is_on_ac_;
      }
    });
  }
}

LLMProvider::~LLMProvider() {
  // The monitor is a process-wide singleton: leaving the `this`-capturing
  // callback registered means the next plug/unplug writes into freed memory
  // (this provider dies on every schema redeploy).
  copilot::UnregisterPowerChange(power_token_);
  power_token_ = 0;
}

void LLMProvider::Clear() {
  if (client_) {
    // Stop (and wait for) an in-flight run before dropping promise_: the
    // finish callback reads promise_ on the llama worker thread, so resetting
    // it while a run is live would be a data race. This is the same handshake
    // Predict() already performs.
    client_->clear();
  }
  promise_.reset();
  future_ = std::shared_future<std::string>();
}

bool LLMProvider::Predict(const std::string& input, const std::string& context) {
  if (!is_on_ac_) {
    return false;
  }
  if (history_->size() < 3) {
    return false;
  }
  std::string prompt = history_->gets(config_.max_history);
  DLOG(INFO) << "[LLM] Predict: '" << prompt << "'";
  client_->clear();
  promise_ = std::make_shared<std::promise<std::string>>();
  future_ = promise_->get_future().share();
  client_->commit(prompt);
  return true;
}

std::vector<copilot::Entry> LLMProvider::Retrive(int timeout_us) const {
  if (!is_on_ac_) {
    return {};
  }
  if (!future_.valid()) {
    return {};
  }
  std::string response;
  if (future_.wait_for(std::chrono::microseconds(timeout_us)) != std::future_status::timeout) {
    response = StripAndNormalize(future_.get());
  }
  DLOG(INFO) << "[LLM] response: '" << response << "'";
  if (response.empty()) {
    return {};
  }
  return {copilot::Entry{response, 4.0, copilot::ProviderType::kLLM}};
}

}  // namespace rime
