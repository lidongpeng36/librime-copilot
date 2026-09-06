
#include <stdexcept>
#include <string>
#include <vector>

#include <llama.h>

#include "llm.h"

namespace {
// `vocab` is only needed by the penalties sampler, which sizes its frequency
// table by the vocabulary; every other sampler in the chain is vocab-agnostic.
llama_sampler* create_sampler(const ClientConfig& cfg, const llama_vocab* vocab) {
  llama_sampler_chain_params params = llama_sampler_chain_default_params();
  params.no_perf = cfg.no_perf;
  llama_sampler* sampler = llama_sampler_chain_init(params);

  // 添加 repetition / freq / presence penalty（非默认时）
  if (cfg.penalty_repeat != 1.0f || cfg.penalty_freq != 0.0f || cfg.penalty_present != 0.0f) {
    llama_sampler_chain_add(
        sampler,
        llama_sampler_init_penalties(llama_vocab_n_tokens(vocab), cfg.penalty_last_n,
                                     cfg.penalty_repeat, cfg.penalty_freq, cfg.penalty_present));
  }

  if (cfg.top_k > 0) {
    llama_sampler_chain_add(sampler, llama_sampler_init_top_k(static_cast<int>(cfg.top_k)));
  }

  if (cfg.top_p > 0.0f) {
    llama_sampler_chain_add(sampler, llama_sampler_init_top_p(cfg.top_p, 1));
  }

  if (cfg.min_p > 0.0f) {
    llama_sampler_chain_add(sampler, llama_sampler_init_min_p(cfg.min_p, 1));
  }

  if (cfg.typical_p > 0.0f) {
    llama_sampler_chain_add(sampler, llama_sampler_init_typical(cfg.typical_p, 1));
  }

  if (cfg.top_n_sigma > 0.0f) {
    llama_sampler_chain_add(sampler, llama_sampler_init_top_n_sigma(cfg.top_n_sigma));
  }

  if (cfg.xtc_p > 0.0f && cfg.xtc_temp > 0.0f) {
    llama_sampler_chain_add(sampler,
                            llama_sampler_init_xtc(cfg.xtc_p, cfg.xtc_temp, 1, cfg.xtc_seed));
  }

  if (cfg.temp_ext_delta > 0.0f) {
    llama_sampler_chain_add(
        sampler, llama_sampler_init_temp_ext(cfg.temp, cfg.temp_ext_delta, cfg.temp_ext_exponent));
  } else if (cfg.temp > 0.0f) {
    llama_sampler_chain_add(sampler, llama_sampler_init_temp(cfg.temp));
  }

  // 末尾一定要有采样器
  llama_sampler_chain_add(sampler, llama_sampler_init_greedy());
  // llama_sampler_chain_add(sampler, llama_sampler_init_dist(LLAMA_DEFAULT_SEED));

  return sampler;
}
}  // namespace

namespace llama {

ClientSimple::ClientSimple(ClientConfig config, const std::string& model,
                           OnFinishCallback on_finish)
    : config_(config), model_path_(model), on_finish_(on_finish) {
  llama_log_set([](ggml_log_level /*level*/, const char* /*text*/, void* /*user_data*/) {},
                nullptr);
  llama_backend_init();

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = 99;
  model_ = llama_model_load_from_file(model_path_.c_str(), model_params);
  if (!model_) {
    throw std::runtime_error("模型加载失败");
  }

  vocab_ = llama_model_get_vocab(model_);

  auto ctx_params = llama_context_default_params();
  // From config, not hardcoded 0. Zero means "the model's declared context",
  // which for Qwen3-0.6B is 40960 tokens of KV cache -- 4.38 GB -- allocated
  // whether or not anything ever asks for a prediction.
  ctx_params.n_ctx = config_.n_ctx;
  ctx_params.n_batch = 512;
  ctx_params.no_perf = false;
  ctx_params.n_threads = std::thread::hardware_concurrency();

  ctx_ = llama_init_from_model(model_, ctx_params);
  if (!ctx_) {
    throw std::runtime_error("上下文初始化失败");
  }
  n_ctx_ = llama_n_ctx(ctx_);
  sampler_ = create_sampler(config, vocab_);

  worker_ = std::make_shared<std::thread>([this]() {
    while (true) {
      std::string prompt;
      std::shared_ptr<std::promise<void>> task;
      {
        // Read the task under the same lock commit() publishes it with:
        // pending_prompt_ / has_new_task_ / running_task_ are all written from
        // the input thread, and an unlocked publish can also lose the wakeup
        // (predicate checked, then set+notify, then wait) — the prediction
        // would silently never run.
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return has_new_task_ || shutdown_; });
        if (shutdown_) {
          break;
        }
        prompt = pending_prompt_;
        task = running_task_;
        has_new_task_ = false;
      }
      run(prompt);
      if (task) {
        task->set_value();
      }
    }
  });
}

ClientSimple::~ClientSimple() {
  stop_ = true;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    shutdown_ = true;
  }
  cond_.notify_one();
  // Join BEFORE releasing the llama objects: run() only observes stop_ after
  // llama_decode / llama_sampler_sample have already used ctx_ and sampler_,
  // so freeing them first is a use-after-free whenever a prediction is still
  // in flight (e.g. the schema is redeployed mid-inference).
  if (worker_ && worker_->joinable()) {
    worker_->join();
  }
  llama_sampler_free(sampler_);
  llama_free(ctx_);
  llama_model_free(model_);
  llama_backend_free();
}

void ClientSimple::wait() {
  if (running_future_.valid()) {
    running_future_.wait();  // 等当前prompt完成
  }
}

void ClientSimple::commit(const std::string& prompt) {
  stop_ = true;
  wait();  // let the in-flight run() bail out first
  stop_ = false;
  auto task = std::make_shared<std::promise<void>>();
  // Take the future before publishing the task: once the lock is released the
  // worker may pick it up and call set_value(), and promise's members are not
  // safe to call concurrently.
  auto future = task->get_future().share();
  {
    std::lock_guard<std::mutex> lock(mutex_);
    pending_prompt_ = prompt;
    running_task_ = task;
    has_new_task_ = true;
  }
  running_future_ = std::move(future);
  cond_.notify_one();
}

bool ClientSimple::run(const std::string& prompt) {
  int n_prompt = 0;
  llama_token new_token_id;
  llama_batch batch;
  std::vector<llama_token> prompt_tokens;

  const bool is_first = true;
  auto& p = prompt;
  n_prompt = -llama_tokenize(vocab_, p.data(), p.size(), nullptr, 0, is_first, true);
  prompt_tokens.resize(n_prompt);
  if (llama_tokenize(vocab_, p.data(), p.size(), prompt_tokens.data(), n_prompt, is_first, true) <
      0) {
    return false;
  }
  batch = llama_batch_get_one(prompt_tokens.data(), prompt_tokens.size());

  llama_memory_seq_rm(llama_get_memory(ctx_), 0, -1, -1);
  int n_pos = 0;
  char buf[128];
  std::string response;
  while (n_pos < config_.n_predict) {
    if (llama_decode(ctx_, batch) != 0) {
      return false;
    }

    n_pos += batch.n_tokens;
    new_token_id = llama_sampler_sample(sampler_, ctx_, -1);
    if (llama_vocab_is_eog(vocab_, new_token_id)) {
      break;
    }

    int n = llama_token_to_piece(vocab_, new_token_id, buf, sizeof(buf), 0, true);
    if (stop_) {
      return false;
    }
    response.append(buf, n);
    batch = llama_batch_get_one(&new_token_id, 1);
  }
  if (on_finish_) {
    on_finish_(response);
  }
  return true;
}

void ClientSimple::clear() {
  stop_ = true;
  wait();
  stop_ = false;
}

}  // namespace llama
