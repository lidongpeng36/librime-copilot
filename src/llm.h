#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <string_view>

struct ClientConfig {
  float temp = -1;
  float top_k = -1;  // <= 0 表示关闭
  float top_p = -1;
  float min_p = -1;
  float typical_p = -1;
  float top_n_sigma = -1;
  float xtc_p = -1;
  float xtc_temp = -1;
  uint32_t xtc_seed = 42;
  float temp_ext_delta = -1;
  float temp_ext_exponent = 1.0f;

  float penalty_repeat = 1.0f;  // 1.0 = 无惩罚
  float penalty_freq = 0.0f;
  float penalty_present = 0.0f;
  int penalty_last_n = 64;

  int n_predict = 64;
  bool no_perf = true;
  bool apply_chat_template = false;
};

struct BackendConfig {
  int n_ctx = 0;  // = 0 表示使用模型的上下文大小
  int n_batch = 512;
  int n_gpu_layers = 99;

  bool no_perf = true;
  bool flash_attn = true;
  std::string model_path;
};

namespace llama {

using StreamCallback = std::function<bool(const std::string_view&)>;
using OnFinishCallback = std::function<void(const std::string&)>;

class Client;

bool PrintCallback(const std::string_view&);

class LLMManager {
 public:
  std::unique_ptr<Client> CreateClient(const std::string& model, const std::string& name,
                                       const ClientConfig&, StreamCallback callback = PrintCallback,
                                       OnFinishCallback on_finish = nullptr);

  static LLMManager& Instance() {
    static LLMManager manager;
    return manager;
  }

  class Impl;

 private:
  LLMManager();
  ~LLMManager();
  LLMManager(const LLMManager&) = delete;
  LLMManager& operator=(const LLMManager&) = delete;

  std::unique_ptr<LLMManager::Impl> impl_;
};

struct ClientImpl;
class Client {
 public:
  ~Client();

  void commit(const std::string& prompt = "", bool async = true);
  void wait();
  void clear();
  void pop_back();
  void pop_front();

  int seq_id() const;
  const std::string& model() const;
  const std::string& name() const;

 private:
  friend class LLMManager::Impl;
  explicit Client(const std::shared_ptr<ClientImpl>& impl);

  std::shared_ptr<ClientImpl> client_;
};

}  // namespace llama

struct llama_vocab;
struct llama_model;
struct llama_context;
struct llama_sampler;

namespace llama {
class ClientSimple {
 public:
  ClientSimple(ClientConfig config, const std::string& model, OnFinishCallback on_finish = nullptr);
  ~ClientSimple();
  void commit(const std::string& prompt = "");
  void wait();
  void clear();

 private:
  bool run(const std::string&);

  ClientConfig config_;
  std::string model_path_;
  OnFinishCallback on_finish_;

  int n_ctx_;
  std::string response_;
  std::atomic_bool shutdown_ = false;
  std::atomic_bool stop_ = false;
  std::shared_ptr<std::thread> worker_;
  std::mutex mutex_;
  std::condition_variable cond_;
  std::string pending_prompt_;
  bool has_new_task_ = false;
  std::shared_ptr<std::promise<void>> running_task_;  // 当前运行的任务
  std::shared_future<void> running_future_;

  llama_model* model_ = nullptr;
  llama_context* ctx_ = nullptr;
  llama_sampler* sampler_ = nullptr;
  const llama_vocab* vocab_ = nullptr;
};

}  // namespace llama
