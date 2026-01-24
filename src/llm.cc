
#include <deque>
#include <future>
#include <iostream>
#include <list>
#include <string>
#include <thread>
#include <unordered_set>
#include <vector>

#include <llama.h>
// #include <sampling.h>

#include "llm.h"

namespace {
llama_sampler* create_sampler(const ClientConfig& cfg) {
  llama_sampler_chain_params params = llama_sampler_chain_default_params();
  params.no_perf = cfg.no_perf;
  llama_sampler* sampler = llama_sampler_chain_init(params);

  // 添加 repetition / freq / presence penalty（非默认时）
  if (cfg.penalty_repeat != 1.0f || cfg.penalty_freq != 0.0f || cfg.penalty_present != 0.0f) {
    llama_sampler_chain_add(sampler,
                            llama_sampler_init_penalties(cfg.penalty_last_n, cfg.penalty_repeat,
                                                         cfg.penalty_freq, cfg.penalty_present));
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
namespace {

struct HistoryEntry {
  HistoryEntry() : time_us(llama_time_us()) {}
  HistoryEntry(uint32_t id, llama_token token_id)
      : last_token_id(token_id), seq_id(id), time_us(llama_time_us()) {}
  HistoryEntry(uint32_t id, llama_token token_id, llama_pos p0, llama_pos p1, llama_pos p)
      : last_token_id(token_id), seq_id(id), p0(p0), p1(p1), pos(p), time_us(llama_time_us()) {}

  llama_token last_token_id = 0;
  llama_pos p0 = -1;
  llama_pos p1 = -1;
  llama_pos pos = -1;
  uint32_t seq_id = 0;
  int64_t time_us = 0;
};

struct Reciept {
  int n_decoded = 0;
  llama_pos p0 = -1;
  llama_pos p1 = -1;
  llama_pos p2 = -1;
  llama_pos pos = -1;
  llama_token token_id = -1;
  std::string result;
};

struct Ticket : public Reciept {
  Ticket(int id, int n_pr) : seq_id(id), n_predict(n_pr) {}
  const int seq_id;
  const int n_predict;
  llama_sampler* sampler;
  // common_sampler* smpl;
  StreamCallback callback = nullptr;
  std::function<void(const Reciept&)> on_first_token = nullptr;
  std::function<void(const Reciept&)> on_finish = nullptr;

  std::promise<bool> promise;
  std::vector<llama_token> tokens;
  int i_batch = -1;
};

class Backend {
 public:
  explicit Backend(const BackendConfig& config);
  ~Backend();

  void commit(std::unique_ptr<Ticket>&&);

  void pop_back(const HistoryEntry&);
  void pop_front(const HistoryEntry&);

  int pos_max(int seq_id) const { return llama_memory_seq_pos_max(llama_get_memory(ctx_), seq_id); }
  std::string detokenize(llama_token id) const {
    char buf[128];
    int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
    if (n > 0) {
      return std::string(buf, n);
    }
    return "";
  }

  llama_model* model() { return model_; }

  int Tokenize(int seq_id, const std::string& prompt, bool is_first,
               std::vector<llama_token>* prompt_tokens, bool apply_chat_template) const;
  std::string ApplyChatTemplate(const std::string& prompt) const;

 private:
  bool init(const BackendConfig& config);
  void resize_kv_cache();
  void run();
  void process(int n_tokens, std::list<std::unique_ptr<Ticket>>* ts);

  llama_context* ctx_ = nullptr;
  llama_model* model_ = nullptr;
  const llama_vocab* vocab_ = nullptr;

  BackendConfig config_;
  const char* tpl_ = nullptr;

  std::thread worker_;
  std::list<std::unique_ptr<Ticket>> tickets_;
  std::mutex mutex_;
  std::condition_variable cv_;
  std::thread thread_;
  bool running_;

  std::mutex kv_mutex_;
  std::deque<HistoryEntry> pop_back_;
  std::deque<HistoryEntry> pop_front_;

  // llama_batch
  std::vector<llama_token> token_;
  std::vector<llama_pos> pos_;
  std::vector<int32_t> n_seq_id_;
  std::vector<llama_seq_id*> seq_id_;
  std::vector<llama_seq_id> seq_ids_;
  std::vector<int8_t> logits_;
};

}  // namespace
}  // namespace llama

// Client
namespace llama {
class History : public std::deque<HistoryEntry> {};

bool PrintCallback(const std::string_view& token) {
  std::cout << token << std::flush;
  return true;
}

struct ClientImpl {
  ClientImpl() = default;
  ~ClientImpl();

  int run(const std::string& prompt = "");
  void commit(const std::string& prompt = "");
  void wait();
  void cancel();
  void clear();
  void pop_back();
  void pop_front();

  int seq_id = 0;
  std::string name;
  std::string model;
  ClientConfig config;
  llama_sampler* sampler = nullptr;
  // common_sampler* smpl = nullptr;
  StreamCallback callback = nullptr;
  OnFinishCallback on_finish = nullptr;
  llama_pos pos = 0;
  std::shared_ptr<Backend> backend;

  std::unique_ptr<History> history;
  std::shared_future<bool> future;
  std::atomic_bool stop = false;

  std::function<void(const std::string&)> on_destruction;
};

inline ClientImpl::~ClientImpl() {
  cancel();
  llama_sampler_free(sampler);
  // common_sampler_free(smpl);
  on_destruction(model);
}

inline void ClientImpl::wait() {
  if (future.valid()) {
    future.wait();
  }
}

inline void ClientImpl::clear() {
  cancel();
  if (history->empty()) {
    return;
  }
  pos = 0;
  history->clear();
  HistoryEntry entry;
  entry.seq_id = seq_id;
  entry.p0 = -1;
  entry.p1 = -1;
  backend->pop_back(entry);
}

inline void ClientImpl::pop_back() {
  if (history->empty()) {
    return;
  }
  const auto& last_entry = history->back();
  backend->pop_back(last_entry);
  pos = last_entry.pos;
  history->pop_back();
}

inline void ClientImpl::pop_front() {
  if (history->empty()) {
    return;
  }
  const auto& first_entry = history->front();
  backend->pop_front(first_entry);
  history->pop_front();
}

inline void ClientImpl::cancel() {
  stop = true;
  wait();
  stop = false;
}

inline void ClientImpl::commit(const std::string& prompt) {
  cancel();
  std::vector<llama_token> tokens;

  // common_sampler_reset(smpl);

  // std::cerr << "[commit. id: " << seq_id << ", pos: " << pos << ", history:" << history->size()
  //           << ", str: '" << prompt << "']" << std::endl;
  if (prompt.empty()) {
    if (history->empty()) {
      return;
    }
    const auto& id = history->back().last_token_id;
    // std::cerr << "[empty. id: " << seq_id << ", str: '" << backend->detokenize(id) << "']"
    //           << std::endl;
    tokens = {id};
  } else {
    backend->Tokenize(seq_id, prompt, history->empty(), &tokens, config.apply_chat_template);
  }
  int n_prompt = tokens.size();
  // int n_predict = config.n_predict - n_prompt;
  int n_predict = config.n_predict;
  auto ticket = std::make_unique<Ticket>(seq_id, n_predict);
  ticket->tokens = std::move(tokens);
  ticket->sampler = sampler;
  // ticket->smpl = smpl;
  ticket->callback = [this](const std::string_view& token) {
    if (stop) {
      return false;
    }
    return callback(token);
  };
  ticket->on_first_token = [this, empty = prompt.empty()](const Reciept& r) {
    pos = r.pos;
    if (!empty) {
      // std::cerr << "[on_first_token. id: " << seq_id << ", p:[" << r.p0 << "," << r.p1
      //           << "], pos: " << pos << ", history:" << history->size() << ", str: '"
      //           << backend->detokenize(r.token_id) << "']" << std::endl;
      history->emplace_back(HistoryEntry(seq_id, r.token_id, r.p0, r.p1, pos));
    }
  };
  ticket->on_finish = [this](const Reciept& r) {
    pos = r.pos;
    // std::cerr << "[on_finish. id: " << seq_id << ", p:[" << r.p1 << "," << r.p2 << "], pos: " <<
    // pos
    //           << ", history:" << history->size() << ", str: '" << backend->detokenize(r.token_id)
    //           << "']" << std::endl;
    history->emplace_back(HistoryEntry(seq_id, r.token_id, r.p1, r.p2, pos));
    on_finish(r.result);
  };
  ticket->p0 = -1;
  ticket->pos = pos;
  future = ticket->promise.get_future();
  backend->commit(std::move(ticket));
}

Client::Client(const std::shared_ptr<ClientImpl>& impl) { client_ = impl; }

Client::~Client() {}

int Client::seq_id() const { return client_->seq_id; }
const std::string& Client::name() const { return client_->name; }
const std::string& Client::model() const { return client_->model; }

void Client::commit(const std::string& prompt, bool async) {
  client_->commit(prompt);
  if (!async) {
    client_->wait();
  }
}
void Client::wait() { client_->wait(); }
void Client::clear() { client_->clear(); }
void Client::pop_back() { client_->pop_back(); }
void Client::pop_front() { client_->pop_front(); }

}  // namespace llama

// Backend
namespace llama {
namespace {
Backend::Backend(const BackendConfig& config) { init(config); }
Backend::~Backend() {
  {
    std::lock_guard<std::mutex> lk(mutex_);
    running_ = false;
  }
  cv_.notify_one();
  llama_free(ctx_);
  llama_model_free(model_);
  llama_backend_free();
  worker_.join();
}

inline bool Backend::init(const BackendConfig& cfg) {
  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = cfg.n_gpu_layers;
  model_ = llama_model_load_from_file(cfg.model_path.c_str(), model_params);
  if (!model_) {
    throw std::runtime_error("模型加载失败");
    return false;
  }

  vocab_ = llama_model_get_vocab(model_);

  auto ctx_params = llama_context_default_params();
  ctx_params.n_ctx = cfg.n_ctx;
  ctx_params.n_batch = cfg.n_batch;
  ctx_params.no_perf = cfg.no_perf;
  ctx_params.n_threads = std::thread::hardware_concurrency();

  ctx_ = llama_init_from_model(model_, ctx_params);
  if (!ctx_) {
    throw std::runtime_error("上下文初始化失败");
    return false;
  }
  config_.n_ctx = llama_n_ctx(ctx_);

  tpl_ = llama_model_chat_template(model_, nullptr);

  token_.resize(config_.n_ctx);
  pos_.assign(config_.n_ctx, 0);
  n_seq_id_.assign(config_.n_ctx, 1);
  seq_id_.resize(config_.n_ctx);
  seq_ids_.assign(config_.n_ctx, 0);
  logits_.assign(config_.n_ctx, 0);
  for (int i = 0; i < config_.n_ctx; ++i) {
    seq_id_[i] = &seq_ids_[i];
  }

  running_ = true;
  worker_ = std::thread([this] { this->run(); });

  return true;
}

inline int Backend::Tokenize(int seq_id, const std::string& prompt, bool is_first,
                             std::vector<llama_token>* prompt_tokens,
                             bool apply_chat_template) const {
  std::string_view p(prompt);
  std::string formatted;

  if (apply_chat_template) {
    formatted = ApplyChatTemplate(prompt);
    p = formatted;
  }
  int n_prompt = -llama_tokenize(vocab_, p.data(), p.size(), nullptr, 0, is_first, true);
  prompt_tokens->resize(n_prompt);
  if (llama_tokenize(vocab_, p.data(), p.size(), prompt_tokens->data(), n_prompt, is_first, true) <
      0) {
    return -1;
    // throw std::runtime_error("prompt 分词失败");
  }
  return n_prompt;
}

inline std::string Backend::ApplyChatTemplate(const std::string& prompt) const {
  std::string result;
  result.resize(prompt.size() * 2);
  llama_chat_message message{"user", prompt.c_str()};
  int new_len = llama_chat_apply_template(tpl_, &message, 1, true, result.data(), result.size());
  if (new_len > result.size()) {
    result.resize(new_len);
    new_len = llama_chat_apply_template(tpl_, &message, 1, true, result.data(), result.size());
  }
  if (new_len < 0) {
    // fprintf(stderr, "failed to apply the chat template\n");
    return "";
  }

  return result;
}

inline void Backend::commit(std::unique_ptr<Ticket>&& ticket) {
  {
    std::lock_guard<std::mutex> lock(mutex_);
    tickets_.push_back(std::move(ticket));
  }
  cv_.notify_one();
}

inline void Backend::pop_back(const HistoryEntry& entry) {
  std::lock_guard<std::mutex> lock(kv_mutex_);
  pop_back_.push_back(entry);
}

inline void Backend::pop_front(const HistoryEntry& entry) {
  std::lock_guard<std::mutex> lock(kv_mutex_);
  pop_front_.push_back(entry);
}

inline void Backend::resize_kv_cache() {
  std::deque<HistoryEntry> back;
  std::deque<HistoryEntry> front;
  {
    std::lock_guard<std::mutex> lock(kv_mutex_);
    back.swap(pop_back_);
    front.swap(pop_front_);
  }
  std::unordered_map<uint32_t, int> pop_back_map;
  for (const auto& entry : back) {
    auto it = pop_back_map.find(entry.seq_id);
    if (it == pop_back_map.end()) {
      pop_back_map.insert({entry.seq_id, entry.p0});
    } else {
      it->second = std::min(entry.p0, it->second);
    }
  }
  for (const auto& entry : pop_back_map) {
    int seq_id = entry.first;
    int p0 = entry.second;
    // std::cerr << "[pop_back: [" << p0 << ", " << "-1] ]";
    llama_memory_seq_rm(llama_get_memory(ctx_), seq_id, p0, -1);
  }
  std::unordered_map<uint32_t, int> pop_front_map;
  for (const auto& entry : front) {
    auto it = pop_front_map.find(entry.seq_id);
    if (it == pop_front_map.end()) {
      pop_front_map.emplace(entry.seq_id, entry.p1);
    } else {
      it->second = std::max(it->second, entry.p1);
    }
  }
  for (const auto& entry : pop_front_map) {
    int seq_id = entry.first;
    int p1 = entry.second;
    llama_memory_seq_rm(llama_get_memory(ctx_), seq_id, -1, p1 - 1);
  }
}

inline void Backend::process(int n_tokens, std::list<std::unique_ptr<Ticket>>* ts) {
  int n_batch = config_.n_batch;
  auto& tickets = *ts;
  char buf[128];
  std::list<std::unique_ptr<Ticket>> decoded;
  decoded.splice(decoded.end(), tickets);
  for (int i = 0; i < n_tokens; i += n_batch) {
    int i_tokens = std::min(n_tokens - i, n_batch);
    llama_batch batch = {
        .n_tokens = i_tokens,
        .token = token_.data() + i,
        .embd = nullptr,
        .pos = nullptr,
        .n_seq_id = n_seq_id_.data() + i,
        .seq_id = seq_id_.data() + i,
        .logits = logits_.data() + i,
    };
    const int ret = llama_decode(ctx_, batch);

    if (ret != 0) {
      if (n_batch == 1 || ret < 0) {
        // if you get here, it means the KV cache is full - try increasing it via the context size
        break;
      }
      // retry with half the batch size to try to find a free slot in the KV cache
      n_batch /= 2;
      i -= n_batch;
      continue;
    }

    auto it = decoded.begin();
    while (it != decoded.end()) {
      auto& t = *it;
      int ith = t->i_batch - i;
      if (ith < 0 || ith >= i_tokens) {
        ++it;
        continue;
      }
      ++t->n_decoded;
      const llama_token id = llama_sampler_sample(t->sampler, ctx_, ith);

      // const llama_token id = common_sampler_sample(t->smpl, ctx_, ith);
      // common_sampler_accept(t->smpl, id, true);
      if (t->n_decoded == 1) {
        int n = pos_max(t->seq_id);
        t->p1 = n;
        t->on_first_token(*t);
      }
      t->token_id = id;
      if (llama_vocab_is_eog(vocab_, id)) {
        t->on_finish(*t);
        t->promise.set_value(true);
        auto current = it++;
        decoded.erase(current);
        continue;
      }
      int n = llama_token_to_piece(vocab_, id, buf, sizeof(buf), 0, true);
      if (n > 0) {
        t->result.append(buf, n);
        bool ret = t->callback(std::string_view(buf, n));
        if (!ret) {
          llama_memory_seq_rm(llama_get_memory(ctx_), t->seq_id, t->p1, -1);
          logits_[t->i_batch] = 0;  // stop sampling
          t->promise.set_value(false);
          auto current = it++;
          decoded.erase(current);
          continue;
        }
        bool exceed = (t->n_decoded >= t->n_predict);
        if (exceed) {
          // logits_[t->i_batch] = 0;  // stop sampling
          t->p2 = pos_max(t->seq_id);
          t->on_finish(*t);
          t->promise.set_value(true);
          auto current = it++;
          decoded.erase(current);
          continue;
        }
      }
      t->tokens = {id};
      auto current = it++;
      tickets.splice(tickets.end(), decoded, current);
    }
  }
}

inline void Backend::run() {
  std::list<std::unique_ptr<Ticket>> tickets;
  while (true) {
    if (tickets.empty()) {
      std::unique_lock<std::mutex> lock(mutex_);
      cv_.wait(lock, [this]() { return !tickets_.empty() || !running_; });
    }
    {
      std::unique_lock<std::mutex> lock(mutex_);
      if (!running_) {
        break;  // 优雅退出
      }
      tickets.splice(tickets.end(), tickets_);
    }

    resize_kv_cache();

    int n_tokens = 0;
    for (auto& t : tickets) {
      const auto& tokens = t->tokens;
      if (t->p0 < 0) {
        t->p0 = llama_memory_seq_pos_max(llama_get_memory(ctx_), t->seq_id);
      }
      int n = tokens.size();
      for (int i = 0; i < n; ++i) {
        token_[n_tokens + i] = tokens[i];
        pos_[n_tokens + i] = t->pos + i;
        seq_ids_[n_tokens + i] = t->seq_id;
        logits_[n_tokens + i] = 0;
      }
      n_tokens += n;
      logits_[n_tokens - 1] = 1;
      t->i_batch = n_tokens - 1;
      t->token_id = tokens.back();
      t->pos = pos_[n_tokens - 1];
    }

    process(n_tokens, &tickets);
  }
}

}  // namespace
}  // namespace llama

// LLMManager
namespace llama {

class LLMManager::Impl {
 public:
  std::unique_ptr<Client> CreateClient(const std::string& model, const std::string& name,
                                       const ClientConfig&, StreamCallback callback,
                                       OnFinishCallback on_finish);

 private:
  void RemoveClient(const std::string&);

  struct Server {
    std::shared_ptr<Backend> backend;
    std::unordered_set<std::string> clients;
    int n_clients = 0;
    uint32_t next_seq_id = 0;
  };
  std::mutex server_mutex_;
  std::unordered_map<std::string, Server> servers_;
};

inline std::unique_ptr<Client> LLMManager::Impl::CreateClient(const std::string& model,
                                                              const std::string& name,
                                                              const ClientConfig& config,
                                                              StreamCallback callback,
                                                              OnFinishCallback on_finish) {
  std::shared_ptr<Backend> backend;
  uint32_t seq_id;
  {
    std::lock_guard<std::mutex> lk(server_mutex_);
    auto it = servers_.find(model);
    if (it == servers_.end()) {
      BackendConfig config;
      config.model_path = model;
      auto backend = std::make_shared<Backend>(config);
      it = servers_.insert({model, Server{std::move(backend)}}).first;
    }
    auto& server = it->second;
    if (server.clients.find(name) != server.clients.end()) {
      return nullptr;
    }
    ++server.n_clients;
    seq_id = ++server.next_seq_id;
    backend = server.backend;
  }
  auto impl = std::make_shared<ClientImpl>();
  impl->seq_id = seq_id;
  impl->name = name;
  impl->model = model;
  impl->config = config;
  impl->callback = callback;
  impl->callback = callback ? callback : [](const std::string_view&) -> bool { return true; };
  impl->on_finish = on_finish ? on_finish : [](const std::string&) {};
  impl->sampler = create_sampler(config);
  // common_params_sampling param;
  // impl->smpl = common_sampler_init(backend->model(), param);
  impl->history = std::make_unique<History>();
  impl->backend = backend;
  impl->on_destruction = [this](const std::string& model) { this->RemoveClient(model); };
  return std::unique_ptr<Client>(new Client(impl));
}

inline void LLMManager::Impl::RemoveClient(const std::string& model) {
  std::lock_guard<std::mutex> lk(server_mutex_);
  auto it = servers_.find(model);
  if (it == servers_.end()) {
    return;
  }
  auto& server = it->second;
  if (--server.n_clients == 0) {
    servers_.erase(it);
  }
}

std::unique_ptr<Client> LLMManager::CreateClient(const std::string& model, const std::string& name,
                                                 const ClientConfig& config,
                                                 StreamCallback callback,
                                                 OnFinishCallback on_finish) {
  return impl_->CreateClient(model, name, config, callback, on_finish);
}

LLMManager::LLMManager() {
  llama_log_set([](ggml_log_level /*level*/, const char* /*text*/, void* /*user_data*/) {},
                nullptr);
  llama_backend_init();
  impl_ = std::make_unique<Impl>();
}

LLMManager::~LLMManager() { llama_backend_free(); }

}  // namespace llama

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
  ctx_params.n_ctx = 0;
  ctx_params.n_batch = 512;
  ctx_params.no_perf = false;
  ctx_params.n_threads = std::thread::hardware_concurrency();

  ctx_ = llama_init_from_model(model_, ctx_params);
  if (!ctx_) {
    throw std::runtime_error("上下文初始化失败");
  }
  n_ctx_ = llama_n_ctx(ctx_);
  sampler_ = create_sampler(config);

  worker_ = std::make_shared<std::thread>([this]() {
    while (true) {
      {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this] { return has_new_task_ || shutdown_; });
      }
      if (shutdown_) {
        break;
      }
      auto prompt = pending_prompt_;
      has_new_task_ = false;
      run(prompt);
      running_task_->set_value();
    }
  });
}

ClientSimple::~ClientSimple() {
  stop_ = true;
  shutdown_ = true;
  cond_.notify_one();
  llama_sampler_free(sampler_);
  llama_free(ctx_);
  llama_model_free(model_);
  llama_backend_free();
  worker_->join();
}

void ClientSimple::wait() {
  if (running_future_.valid()) {
    running_future_.wait();  // 等当前prompt完成
  }
}

void ClientSimple::commit(const std::string& prompt) {
  stop_ = true;
  wait();
  stop_ = false;
  pending_prompt_ = prompt;
  has_new_task_ = true;
  running_task_ = std::make_shared<std::promise<void>>();
  running_future_ = running_task_->get_future().share();
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
  on_finish_(response);
  return true;
}

void ClientSimple::clear() {
  stop_ = true;
  wait();
  stop_ = false;
}

}  // namespace llama
