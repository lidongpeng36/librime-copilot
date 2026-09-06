#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
  // KV cache size, in tokens. NOT the model's own context length, which is
  // what this used to allocate: llm.cc hardcoded `ctx_params.n_ctx = 0`, so
  // Qwen3-0.6B took its full 40960 -- 112 KB of KV per token, 4.38 GB
  // resident -- to predict the 8 tokens copilot/llm/n_predict asks for.
  //
  // 512 is generous for that job: the prompt is at most `max_history` recent
  // commits. Raise it via copilot/llm/n_ctx if a caller needs more; 0 restores
  // the old "whatever the model declares" behaviour and its cost.
  int n_ctx = 512;
  bool no_perf = true;
};

struct llama_vocab;
struct llama_model;
struct llama_context;
struct llama_sampler;

namespace llama {

using OnFinishCallback = std::function<void(const std::string&)>;

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
