#include "ime_bridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <chrono>
#include <set>
#include <thread>

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <nlohmann/json.hpp>

namespace rime {

using json = nlohmann::json;

namespace {

constexpr int kProtocolVersion = 1;
constexpr const char* kNamespace = "rime.ime";
constexpr size_t kMaxMessageSize = 4096;
// Upper bound on one accumulated JSON line; a client that never sends '\n'
// must not be able to grow the buffer without limit.
constexpr size_t kMaxPendingBytes = 1 << 20;  // 1 MiB
constexpr int kCleanupIntervalSeconds = 60;
constexpr int kAcceptRetryDelayMs = 100;

}  // namespace

// ============================================================================
// ImeBridgeServer implementation (singleton)
// ============================================================================

ImeBridgeServer& ImeBridgeServer::Instance() {
  static ImeBridgeServer instance;
  return instance;
}

ImeBridgeServer::~ImeBridgeServer() { Stop(); }

void ImeBridgeServer::AddRef() { ref_count_.fetch_add(1); }

void ImeBridgeServer::Release() {
  if (ref_count_.fetch_sub(1) == 1) {
    Stop();
  }
}

void ImeBridgeServer::Start(const Config& config) {
  std::lock_guard<std::mutex> lock(mutex_);

  if (running_.load()) {
    return;  // 已经在运行
  }

  state_.config_ = config;

  // 删除已存在的 socket 文件
  unlink(state_.config_.socket_path.c_str());

  // 创建 Unix Domain Socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    LOG(ERROR) << "[ImeBridge] Failed to create socket: " << strerror(errno);
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, state_.config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

  if (bind(server_fd_, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
    LOG(ERROR) << "[ImeBridge] Failed to bind socket: " << strerror(errno);
    close(server_fd_);
    server_fd_ = -1;
    return;
  }

  if (listen(server_fd_, 5) < 0) {
    LOG(ERROR) << "[ImeBridge] Failed to listen: " << strerror(errno);
    close(server_fd_);
    server_fd_ = -1;
    return;
  }

  running_.store(true);
  server_thread_ = std::make_unique<std::thread>([this]() { RunServer(); });

  LOG(INFO) << "[ImeBridge] Server started on " << state_.config_.socket_path;
}

void ImeBridgeServer::Stop() {
  // Same lock as Start(): without it a concurrent Start/Stop (schema reload
  // racing a Release()) could interleave on server_fd_ / server_thread_.
  std::lock_guard<std::mutex> lock(mutex_);

  if (!running_.load()) {
    return;
  }

  running_.store(false);

  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }
  server_thread_.reset();

  // The two blocks below hold conn_mutex_, NOT the mutex_ taken at the top of
  // this function; they are named conn_lock so the difference is visible, since
  // this function's whole correctness argument is which one is held where.
  //
  // shutdown() makes each connection thread's blocking read() return 0. Without
  // this they stay parked forever holding a reference to state_.
  {
    std::lock_guard<std::mutex> conn_lock(conn_mutex_);
    for (int fd : client_fds_) {
      shutdown(fd, SHUT_RDWR);
    }
  }
  {
    std::unique_lock<std::mutex> conn_lock(conn_mutex_);
    if (!conn_cv_.wait_for(conn_lock, std::chrono::seconds(2),
                           [this] { return live_conns_ == 0; })) {
      LOG(WARNING) << "[ImeBridge] " << live_conns_ << " connection thread(s) did not exit in time";
    }
  }

  unlink(state_.config_.socket_path.c_str());

  LOG(INFO) << "[ImeBridge] Server stopped.";
}

void ImeBridgeServer::RunServer() {
  while (running_.load()) {
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      const int err = errno;
      if (err == EINTR) {
        continue;
      }
      if (!running_.load()) {
        break;
      }
      // Back off instead of spinning: a persistent failure (EMFILE when the
      // process is out of descriptors, or a dead listening socket) would
      // otherwise burn a core and flood the log.
      LOG(WARNING) << "[ImeBridge] Accept failed: " << strerror(err);
      std::this_thread::sleep_for(std::chrono::milliseconds(kAcceptRetryDelayMs));
      continue;
    }
    // Handle each client on its own thread so multiple Neovim instances can
    // keep long-lived connections concurrently. Register the fd first so
    // Stop() can shut it down and unblock the read().
    {
      std::lock_guard<std::mutex> lock(conn_mutex_);
      client_fds_.insert(client_fd);
      ++live_conns_;
    }
    std::thread([this, client_fd]() {
      HandleConnection(client_fd);
      {
        std::lock_guard<std::mutex> lock(conn_mutex_);
        client_fds_.erase(client_fd);
        --live_conns_;
        // Notified under the lock on purpose: Stop() may return and destroy
        // this server the moment it sees the count hit zero, so the last thing
        // this thread touches on `this` has to happen before that can be
        // observed.
        conn_cv_.notify_all();
      }
      close(client_fd);
    }).detach();
  }
}

void ImeBridgeServer::HandleConnection(int client_fd) {
  char buffer[kMaxMessageSize];
  std::string message;
  // Which client keys this connection carries. One connection is normally one
  // client, but the protocol allows several, so track the set.
  std::set<std::string> keys;

  auto note = [&](const std::string& key) {
    if (!key.empty() && keys.insert(key).second) {
      state_.RetainClientConnection(key);
    }
  };

  while (true) {
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
      break;
    }
    message.append(buffer, static_cast<size_t>(n));

    size_t pos;
    while ((pos = message.find('\n')) != std::string::npos) {
      std::string line = message.substr(0, pos);
      message = message.substr(pos + 1);

      if (!line.empty()) {
        note(state_.ProcessMessage(line));
      }
    }

    if (message.size() > kMaxPendingBytes) {
      LOG(WARNING) << "[ImeBridge] Dropping oversized message (" << message.size()
                   << " bytes without a newline)";
      message.clear();
    }
  }

  if (!message.empty()) {
    note(state_.ProcessMessage(message));
  }

  for (const auto& key : keys) {
    state_.ReleaseClientConnection(key);
  }
}

std::string ImeBridgeState::ProcessMessage(const std::string& message) {
  try {
    auto j = json::parse(message);

    int version = j.value("v", 0);
    if (version != kProtocolVersion) {
      LOG(WARNING) << "[ImeBridge] Unsupported protocol version: " << version;
      return "";
    }

    std::string ns = j.value("ns", "");
    if (ns != kNamespace) {
      LOG(WARNING) << "[ImeBridge] Unknown namespace: " << ns;
      return "";
    }

    std::string type = j.value("type", "");
    if (type != "ascii") {
      LOG(WARNING) << "[ImeBridge] Unknown type: " << type;
      return "";
    }

    auto src = j.value("src", json::object());
    std::string app = src.value("app", "unknown");
    std::string instance = src.value("instance", "default");
    std::string client_key = MakeClientKey(app, instance);

    auto data = j.value("data", json::object());
    std::string action = data.value("action", "");

    if (config_.debug) {
      LOG(INFO) << "[ImeBridge] Received: client=" << client_key << ", action=" << action;
    }

    TouchClient(client_key);

    if (action == "set") {
      bool ascii = data.value("ascii", true);
      bool stack = data.value("stack", true);
      HandleSet(client_key, ascii, stack);
    } else if (action == "restore") {
      HandleRestore(client_key);
    } else if (action == "reset") {
      bool restore = data.value("restore", true);
      HandleReset(client_key, restore);
    } else if (action == "unregister") {
      HandleUnregister(client_key);
    } else if (action == "context") {
      std::string before = data.value("before", "");
      std::string after = data.value("after", "");
      HandleContext(client_key, before, after);
    } else if (action == "clear_context") {
      HandleClearContext(client_key);
    } else if (action == "activate") {
      HandleActivate(client_key);
    } else if (action == "deactivate") {
      HandleDeactivate(client_key);
    } else if (action == "ping") {
      if (config_.debug) {
        LOG(INFO) << "[ImeBridge] Ping received from " << client_key;
      }
    } else {
      LOG(WARNING) << "[ImeBridge] Unknown action: " << action;
    }

    return client_key;

  } catch (const json::exception& e) {
    LOG(ERROR) << "[ImeBridge] JSON parse error: " << e.what();
    return "";
  }
}

std::string ImeBridgeState::MakeClientKey(const std::string& app, const std::string& instance) {
  return app + ":" + instance;
}

void ImeBridgeState::TouchClient(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = client_states_.find(client_key);
  if (it != client_states_.end()) {
    it->second.last_active = std::chrono::steady_clock::now();
  }
}

void ImeBridgeState::RetainClientConnection(const std::string& client_key) {
  if (client_key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  ++conn_refs_[client_key];
}

void ImeBridgeState::ReleaseClientConnection(const std::string& client_key) {
  if (client_key.empty()) {
    return;
  }
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = conn_refs_.find(client_key);
  if (it == conn_refs_.end()) {
    return;
  }
  if (--it->second > 0) {
    return;  // the client is reconnecting; another connection is still live
  }
  conn_refs_.erase(it);

  // The client vanished without saying goodbye (kill -9, ssh tunnel dropped).
  // Put ascii_mode back where we found it, otherwise it stays stuck in English
  // until the user notices and switches by hand. If the client did exit
  // cleanly its state is already erased and ApplyAction will no-op.
  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kReset;
  action.client_key = client_key;
  action.restore = true;
  // We cannot tell "gone" from "reconnecting" here; ApplyAction runs later, on
  // Rime's next key event, and by then conn_refs_ can answer that. Mark it so
  // it knows this reset is ours and may be dropped.
  action.synthesized = true;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] Last connection closed for " << client_key << "; synthesized reset";
  }
}

void ImeBridgeState::HandleSet(const std::string& client_key, bool ascii, bool stack) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto& state = client_states_[client_key];
  state.last_active = std::chrono::steady_clock::now();
  state.current_target = ascii;

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kSet;
  action.client_key = client_key;
  action.ascii = ascii;
  action.stack = stack;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleSet: client=" << client_key << ", ascii=" << ascii
              << ", stack=" << stack << ", depth=" << state.depth
              << ", queue_size=" << pending_actions_.size();
  }
}

void ImeBridgeState::HandleRestore(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = client_states_.find(client_key);
  if (it == client_states_.end() || it->second.depth == 0) {
    if (config_.debug) {
      LOG(INFO) << "[ImeBridge] HandleRestore: no state to restore for " << client_key;
    }
    return;
  }

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kRestore;
  action.client_key = client_key;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleRestore: client=" << client_key
              << ", depth=" << it->second.depth << ", queue_size=" << pending_actions_.size();
  }
}

void ImeBridgeState::HandleReset(const std::string& client_key, bool restore) {
  std::lock_guard<std::mutex> lock(mutex_);

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kReset;
  action.client_key = client_key;
  action.restore = restore;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleReset: client=" << client_key << ", restore=" << restore;
  }
}

void ImeBridgeState::HandleUnregister(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kUnregister;
  action.client_key = client_key;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleUnregister: client=" << client_key;
  }
}

void ImeBridgeState::HandleContext(const std::string& client_key, const std::string& before,
                                   const std::string& after) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = client_states_[client_key];
  state.char_before = before;
  state.char_after = after;
  state.context_valid = true;
  state.context_time = std::chrono::steady_clock::now();
  state.last_active = std::chrono::steady_clock::now();
  active_client_ = client_key;

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleContext: client=" << client_key << ", before='" << before
              << "', after='" << after << "'";
  }
}

void ImeBridgeState::HandleClearContext(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = client_states_.find(client_key);
  if (it != client_states_.end()) {
    it->second.context_valid = false;
    it->second.char_before.clear();
    it->second.char_after.clear();
    it->second.last_active = std::chrono::steady_clock::now();
    if (active_client_ == client_key) {
      active_client_.clear();
    }
  }

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleClearContext: client=" << client_key;
  }
}

void ImeBridgeState::HandleActivate(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto& state = client_states_[client_key];
  state.last_active = std::chrono::steady_clock::now();
  active_client_ = client_key;
  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleActivate: client=" << client_key;
  }
}

void ImeBridgeState::HandleDeactivate(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = client_states_.find(client_key);
  if (it != client_states_.end()) {
    it->second.last_active = std::chrono::steady_clock::now();
  }
  if (active_client_ == client_key) {
    active_client_.clear();
  }
  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleDeactivate: client=" << client_key;
  }
}

std::queue<ImeBridgePendingAction> ImeBridgeState::TakePendingActions() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::queue<ImeBridgePendingAction> result;
  std::swap(result, pending_actions_);
  return result;
}

std::optional<SurroundingText> ImeBridgeState::GetActiveContext() {
  std::lock_guard<std::mutex> lock(mutex_);

  if (active_client_.empty()) {
    return std::nullopt;
  }

  auto it = client_states_.find(active_client_);
  if (it == client_states_.end()) {
    return std::nullopt;
  }
  if (!it->second.context_valid) {
    return std::nullopt;
  }
  if (config_.context_ttl_seconds > 0) {
    auto age = std::chrono::steady_clock::now() - it->second.context_time;
    if (age > std::chrono::seconds(config_.context_ttl_seconds)) {
      return std::nullopt;
    }
  }
  return SurroundingText{it->second.char_before, it->second.char_after, active_client_};
}

void ImeBridgeState::CleanupStaleClients() {
  auto now = std::chrono::steady_clock::now();
  auto timeout = std::chrono::minutes(config_.client_timeout_minutes);

  std::vector<std::string> stale_clients;

  std::lock_guard<std::mutex> lock(mutex_);
  // Throttle under the same lock that guards last_cleanup_: this runs from the
  // input thread on every key event.
  if (now - last_cleanup_ < std::chrono::seconds(kCleanupIntervalSeconds)) {
    return;
  }
  last_cleanup_ = now;

  for (const auto& [key, state] : client_states_) {
    if (now - state.last_active > timeout) {
      stale_clients.push_back(key);
    }
  }

  for (const auto& key : stale_clients) {
    if (config_.debug) {
      LOG(INFO) << "[ImeBridge] Removing stale client: " << key;
    }
    client_states_.erase(key);
    if (active_client_ == key) {
      active_client_.clear();
    }
  }
}

ImeBridgeState::ApplyResult ImeBridgeState::ApplyAction(const ImeBridgePendingAction& action,
                                                        bool current_ascii) {
  ApplyResult result;
  result.should_set = false;

  std::lock_guard<std::mutex> lock(mutex_);

  switch (action.type) {
    case ImeBridgePendingAction::kSet: {
      auto& state = client_states_[action.client_key];

      // 在第一次 set 时保存初始状态（整个会话只保存一次）
      if (!state.has_initial) {
        state.initial_state = current_ascii;
        state.has_initial = true;
        if (config_.debug) {
          LOG(INFO) << "[ImeBridge] ApplyAction kSet: saved initial_state=" << state.initial_state;
        }
      }

      // 如果是 stack=true (默认行为)，则更新 depth 和 base
      if (action.stack) {
        // 如果是当前 cycle 的第一次 set，记录为 base
        if (state.depth == 0) {
          state.base = current_ascii;
          state.has_base = true;
          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] ApplyAction kSet: saved base=" << state.base;
          }
        }
        state.depth++;
      } else {
        if (config_.debug) {
          LOG(INFO) << "[ImeBridge] ApplyAction kSet: non-stack set, skipping flow control";
        }
      }

      result.should_set = true;
      result.ascii_mode = action.ascii;

      if (config_.debug) {
        LOG(INFO) << "[ImeBridge] ApplyAction kSet: ascii=" << action.ascii
                  << ", base=" << state.base << ", depth=" << state.depth;
      }
      break;
    }

    case ImeBridgePendingAction::kRestore: {
      auto it = client_states_.find(action.client_key);
      if (it != client_states_.end() && it->second.depth > 0) {
        it->second.depth--;

        if (it->second.depth == 0 && it->second.has_base) {
          result.should_set = true;
          result.ascii_mode = it->second.base;
          it->second.has_base = false;

          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] ApplyAction kRestore: restored to base=" << it->second.base;
          }
        } else {
          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] ApplyAction kRestore: depth=" << it->second.depth;
          }
        }
      }
      break;
    }

    case ImeBridgePendingAction::kReset: {
      // A reset we synthesized on disconnect is moot if the client has a live
      // connection again by now: the tunnel merely blipped, and applying it
      // would yank the user back to initial_state (typically English)
      // mid-composition *and* erase has_initial/base/depth, so nothing could
      // put them back. resync() deliberately does not re-assert ascii_mode.
      // conn_refs_ is guarded by mutex_, which we already hold: read it
      // directly rather than going through Retain/ReleaseClientConnection,
      // which would deadlock on this non-recursive mutex.
      if (action.synthesized) {
        auto conn = conn_refs_.find(action.client_key);
        if (conn != conn_refs_.end() && conn->second > 0) {
          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] ApplyAction kReset: client " << action.client_key
                      << " reconnected; skipping synthesized reset";
          }
          break;
        }
      }
      auto it = client_states_.find(action.client_key);
      if (it != client_states_.end()) {
        // reset(true) 恢复到初始状态（Neovim 启动时的状态）
        if (action.restore && it->second.has_initial) {
          result.should_set = true;
          result.ascii_mode = it->second.initial_state;

          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] ApplyAction kReset: restored to initial_state="
                      << it->second.initial_state;
          }
        }
        client_states_.erase(it);
      }
      break;
    }

    case ImeBridgePendingAction::kUnregister: {
      auto it = client_states_.find(action.client_key);
      if (it != client_states_.end()) {
        client_states_.erase(it);
        if (config_.debug) {
          LOG(INFO) << "[ImeBridge] ApplyAction kUnregister: client=" << action.client_key;
        }
      }
      break;
    }

    default:
      break;
  }

  return result;
}

// ============================================================================
// ImeBridge implementation (per-session processor)
// ============================================================================

ImeBridge::ImeBridge(const Ticket& ticket) : CopilotPlugin<ImeBridge>(ticket) {
  // 从配置读取参数
  if (auto* config = engine_->schema()->config()) {
    config->GetBool("copilot/ime_bridge/enable", &config_.enable);
    config->GetString("copilot/ime_bridge/socket_path", &config_.socket_path);
    config->GetBool("copilot/ime_bridge/debug", &config_.debug);
    config->GetInt("copilot/ime_bridge/client_timeout_minutes", &config_.client_timeout_minutes);
    config->GetInt("copilot/ime_bridge/context_ttl_seconds", &config_.context_ttl_seconds);
  }

  if (config_.enable) {
    enabled_ = true;
    auto& server = ImeBridgeServer::Instance();
    server.AddRef();
    server.Start(config_);
    if (engine_) {
      // server.RegisterContext(engine_->context()); // Removed
    }
  }

  LOG(INFO) << "[ImeBridge] Initialized. enable=" << config_.enable
            << ", socket_path=" << config_.socket_path;
}

ImeBridge::~ImeBridge() {
  if (enabled_) {
    // auto& server = ImeBridgeServer::Instance();
    // if (engine_) {
    //   server.UnregisterContext(engine_->context()); // Removed
    // }
    ImeBridgeServer::Instance().Release();
  }
  LOG(INFO) << "[ImeBridge] Destroyed.";
}

ProcessResult ImeBridge::Process(const KeyEvent& key_event) {
  if (!engine_ || !enabled_) {
    return kNoop;
  }

  auto* ctx = engine_->context();
  if (ctx) {
    ApplyPendingActions(ctx);
  }
  // Self-throttled to once per kCleanupIntervalSeconds; this is the only call
  // site, so without it client_timeout_minutes did nothing at all.
  ImeBridgeServer::Instance().CleanupStaleClients();

  return kNoop;
}

void ImeBridge::ApplyPendingActions(Context* ctx) {
  if (!ctx) {
    return;
  }

  auto& server = ImeBridgeServer::Instance();

  auto actions = server.TakePendingActions();

  while (!actions.empty()) {
    auto action = actions.front();
    actions.pop();

    bool current_ascii = ctx->get_option("ascii_mode");
    auto result = server.ApplyAction(action, current_ascii);

    if (result.should_set) {
      ctx->set_option("ascii_mode", result.ascii_mode);
      if (server.IsDebug()) {
        LOG(INFO) << "[ImeBridge] Applied: set ascii_mode=" << result.ascii_mode;
      }
    }
  }
}

}  // namespace rime
