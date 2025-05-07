#pragma once

#include <future>
#include <memory>
#include <string>
#include <unordered_map>

#include "history.h"
#include "provider.h"

namespace llama {
class Client;
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
  };
  LLMProvider(const Config& config, const std::shared_ptr<::copilot::History>& history);
  virtual ~LLMProvider();

  // 提交输入，异步发起推理
  void Commit(const std::string& input, const std::string& app_id) {
    Commit(input, GetOrCreateSession(app_id));
  }

  // 获取最近推理完成的结果
  std::string GetCurrentResults(int timeout_us, const std::string& app_id) const;

  void Clear(const std::string& app_id) { Clear(GetOrCreateSession(app_id)); }
  void Backspace(const std::string& app_id) { Backspace(GetOrCreateSession(app_id)); }

  // Provider interface
  void OnBackspace() override {}
  int Rank() const override { return config_.rank; }
  bool Predict(const std::string& input) override;
  std::vector<::copilot::Entry> Retrive(int timeout_us) const override;

 private:
  struct Session {
    std::unique_ptr<llama::Client> client;
    std::shared_ptr<::copilot::History> history;
    std::shared_ptr<std::promise<std::string>> promise;
    std::shared_future<std::string> future;
    std::string response;
    // 可以加更多，比如：
    // uint64_t last_update_time;
  };

  void Clear(const std::shared_ptr<Session>& session);
  void Backspace(const std::shared_ptr<Session>& session);
  void Commit(const std::string& input, const std::shared_ptr<Session>& session);
  std::string GetResults(const std::shared_ptr<Session>& session, int timeout_us) const;
  std::shared_ptr<Session> CreateSession(const std::string& app_id);
  std::shared_ptr<Session> GetOrCreateSession(const std::string& app_id);

  std::unordered_map<std::string, std::shared_ptr<Session>> sessions_;

  std::shared_ptr<Session> session_;
  std::shared_ptr<::copilot::History> history_;

  Config config_;

  std::unique_ptr<llama::ClientSimple> client_;
  std::shared_ptr<std::promise<std::string>> promise_;
  std::shared_future<std::string> future_;
};

}  // namespace rime
