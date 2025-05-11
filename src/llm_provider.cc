#include "llm_provider.h"

#include <glog/logging.h>

#include "llm.h"
#include "utils.h"

#define USE_SIMPLE_CLIENT

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
#ifdef USE_SIMPLE_CLIENT
  ClientConfig config;
  config.n_predict = c.n_predict;
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
#else
  session_ = CreateSession("copilot");
  Predict("WarmUp");
  Clear(session_);
#endif
  if (!config_.battery_active) {
    is_on_ac_ = copilot::IsACPowerConnected();
    copilot::RegisterPowerChange([this](bool is_ac_power) {
      if (is_ac_power != is_on_ac_) {
        is_on_ac_ = is_ac_power;
        DLOG(INFO) << "[LLM]: AC Power Connected:" << is_on_ac_;
      }
    });
  }
}

LLMProvider::~LLMProvider() {}

void LLMProvider::Backspace(const std::shared_ptr<Session>& session) {}

void LLMProvider::Commit(const std::string& input, const std::shared_ptr<Session>& session) {
  std::string prompt = history_->gets(config_.max_history);
  DLOG(INFO) << "[LLM] Prompt: '" << prompt << "'";
  session->response.clear();
  session->promise = std::make_shared<std::promise<std::string>>();
  session->future = session->promise->get_future().share();
  session->client->clear();
  session->client->commit(prompt, /* async = */ true);
}

std::string LLMProvider::GetResults(const std::shared_ptr<LLMProvider::Session>& session,
                                    int timeout_us) const {
  static const std::string empty_result;
  auto& response = session->response;
  auto& future = session->future;
  if (!future.valid()) {
    return empty_result;
  }
  if (future.wait_for(std::chrono::microseconds(timeout_us)) != std::future_status::timeout) {
    response = StripAndNormalize(future.get());
    // LOG(INFO) << "[LLM] response: '" << response << "'";
  }
  return response;
}

std::string LLMProvider::GetCurrentResults(int timeout_us, const std::string& app_id) const {
  static const std::string empty_result;
  auto it = sessions_.find(app_id);
  if (it == sessions_.end()) {
    return empty_result;
  }
  return GetResults(it->second, timeout_us);
}

std::shared_ptr<LLMProvider::Session> LLMProvider::CreateSession(const std::string& app_id) {
  auto session = std::make_shared<Session>();
  session->history = std::make_shared<::copilot::History>(100);
  ClientConfig config;
  config.apply_chat_template = false;
  config.n_predict = config_.n_predict;
  config.no_perf = false;

  auto& manager = llama::LLMManager::Instance();
  std::weak_ptr<Session> weak_session = session;
  session->client = manager.CreateClient(config_.model, app_id, config, nullptr,
                                         [weak_session](const std::string& response) {
                                           if (auto session = weak_session.lock()) {
                                             session->promise->set_value(response);
                                           }
                                         });

  return session;
}

std::shared_ptr<LLMProvider::Session> LLMProvider::GetOrCreateSession(const std::string& app_id) {
  auto it = sessions_.find(app_id);
  if (it != sessions_.end()) {
    return it->second;
  }
  auto session = CreateSession(app_id);
  return sessions_.emplace(app_id, session).first->second;
}

void LLMProvider::Clear(const std::shared_ptr<Session>& session) { session->client->clear(); }

bool LLMProvider::Predict(const std::string& input) {
  if (!is_on_ac_) {
    return false;
  }
#ifdef USE_SIMPLE_CLIENT
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
#else
  Commit(input, session_);
  return true;
#endif
}

std::vector<copilot::Entry> LLMProvider::Retrive(int timeout_us) const {
  if (!is_on_ac_) {
    return {};
  }
#ifdef USE_SIMPLE_CLIENT
  if (!future_.valid()) {
    return {};
  }
  std::string response;
  if (future_.wait_for(std::chrono::microseconds(timeout_us)) != std::future_status::timeout) {
    response = StripAndNormalize(future_.get());
  }
#else
  auto response = GetResults(session_, timeout_us);
#endif
  DLOG(INFO) << "[LLM] response: '" << response << "'";
  if (response.empty()) {
    return {};
  }
  return {copilot::Entry{response, 4.0, copilot::ProviderType::kLLM}};
}

}  // namespace rime
