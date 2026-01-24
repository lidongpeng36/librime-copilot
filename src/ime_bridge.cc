#include "ime_bridge.h"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <nlohmann/json.hpp>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>

namespace rime {

using json = nlohmann::json;

namespace {

constexpr int kProtocolVersion = 1;
constexpr const char* kNamespace = "rime.ime";
constexpr size_t kMaxMessageSize = 4096;
constexpr int kCleanupIntervalSeconds = 60;

}  // namespace

// ============================================================================
// ImeBridgeServer implementation (singleton)
// ============================================================================

ImeBridgeServer& ImeBridgeServer::Instance() {
  static ImeBridgeServer instance;
  return instance;
}

ImeBridgeServer::~ImeBridgeServer() {
  Stop();
}

void ImeBridgeServer::AddRef() {
  ref_count_.fetch_add(1);
}

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

  config_ = config;
  last_cleanup_ = std::chrono::steady_clock::now();

  // 删除已存在的 socket 文件
  unlink(config_.socket_path.c_str());

  // 创建 Unix Domain Socket
  server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (server_fd_ < 0) {
    LOG(ERROR) << "[ImeBridge] Failed to create socket: " << strerror(errno);
    return;
  }

  struct sockaddr_un addr;
  memset(&addr, 0, sizeof(addr));
  addr.sun_family = AF_UNIX;
  strncpy(addr.sun_path, config_.socket_path.c_str(), sizeof(addr.sun_path) - 1);

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

  LOG(INFO) << "[ImeBridge] Server started on " << config_.socket_path;
}

void ImeBridgeServer::Stop() {
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

  unlink(config_.socket_path.c_str());

  LOG(INFO) << "[ImeBridge] Server stopped.";
}

void ImeBridgeServer::RunServer() {
  while (running_.load()) {
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (running_.load()) {
        LOG(WARNING) << "[ImeBridge] Accept failed: " << strerror(errno);
      }
      continue;
    }

    HandleConnection(client_fd);
    close(client_fd);
  }
}

void ImeBridgeServer::HandleConnection(int client_fd) {
  char buffer[kMaxMessageSize];
  std::string message;

  while (true) {
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
      break;
    }
    buffer[n] = '\0';
    message += buffer;

    size_t pos;
    while ((pos = message.find('\n')) != std::string::npos) {
      std::string line = message.substr(0, pos);
      message = message.substr(pos + 1);

      if (!line.empty()) {
        ProcessMessage(line);
      }
    }
  }

  if (!message.empty()) {
    ProcessMessage(message);
  }
}

void ImeBridgeServer::ProcessMessage(const std::string& message) {
  try {
    auto j = json::parse(message);

    int version = j.value("v", 0);
    if (version != kProtocolVersion) {
      LOG(WARNING) << "[ImeBridge] Unsupported protocol version: " << version;
      return;
    }

    std::string ns = j.value("ns", "");
    if (ns != kNamespace) {
      LOG(WARNING) << "[ImeBridge] Unknown namespace: " << ns;
      return;
    }

    std::string type = j.value("type", "");
    if (type != "ascii") {
      LOG(WARNING) << "[ImeBridge] Unknown type: " << type;
      return;
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

    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_client_ = client_key;
    }
    TouchClient(client_key);

    if (action == "set") {
      bool ascii = data.value("ascii", true);
      HandleSet(client_key, ascii);
    } else if (action == "restore") {
      HandleRestore(client_key);
    } else if (action == "reset") {
      bool restore = data.value("restore", true);
      HandleReset(client_key, restore);
    } else if (action == "unregister") {
      HandleUnregister(client_key);
    } else if (action == "ping") {
      if (config_.debug) {
        LOG(INFO) << "[ImeBridge] Ping received from " << client_key;
      }
    } else {
      LOG(WARNING) << "[ImeBridge] Unknown action: " << action;
    }

  } catch (const json::exception& e) {
    LOG(ERROR) << "[ImeBridge] JSON parse error: " << e.what();
  }
}

std::string ImeBridgeServer::MakeClientKey(const std::string& app, const std::string& instance) {
  return app + ":" + instance;
}

void ImeBridgeServer::TouchClient(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);
  auto it = client_states_.find(client_key);
  if (it != client_states_.end()) {
    it->second.last_active = std::chrono::steady_clock::now();
  }
}

void ImeBridgeServer::HandleSet(const std::string& client_key, bool ascii) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto& state = client_states_[client_key];
  state.last_active = std::chrono::steady_clock::now();
  state.current_target = ascii;

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kSet;
  action.client_key = client_key;
  action.ascii = ascii;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleSet: client=" << client_key << ", ascii=" << ascii
              << ", depth=" << state.depth << ", queue_size=" << pending_actions_.size();
  }
}

void ImeBridgeServer::HandleRestore(const std::string& client_key) {
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

void ImeBridgeServer::HandleReset(const std::string& client_key, bool restore) {
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

void ImeBridgeServer::HandleUnregister(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  ImeBridgePendingAction action;
  action.type = ImeBridgePendingAction::kUnregister;
  action.client_key = client_key;
  pending_actions_.push(action);

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleUnregister: client=" << client_key;
  }
}

std::queue<ImeBridgePendingAction> ImeBridgeServer::TakePendingActions() {
  std::lock_guard<std::mutex> lock(mutex_);
  std::queue<ImeBridgePendingAction> result;
  std::swap(result, pending_actions_);
  return result;
}

void ImeBridgeServer::CleanupStaleClients() {
  auto now = std::chrono::steady_clock::now();
  
  // Check if enough time has passed since last cleanup
  if (now - last_cleanup_ < std::chrono::seconds(kCleanupIntervalSeconds)) {
    return;
  }
  last_cleanup_ = now;

  auto timeout = std::chrono::minutes(config_.client_timeout_minutes);

  std::vector<std::string> stale_clients;

  {
    std::lock_guard<std::mutex> lock(mutex_);
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
    }
  }
}

ImeBridgeServer::ApplyResult ImeBridgeServer::ApplyAction(
    const ImeBridgePendingAction& action, bool current_ascii) {
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
      
      // 如果是当前 cycle 的第一次 set，记录为 base
      if (state.depth == 0) {
        state.base = current_ascii;
        state.has_base = true;
        if (config_.debug) {
          LOG(INFO) << "[ImeBridge] ApplyAction kSet: saved base=" << state.base;
        }
      }
      state.depth++;
      
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
  }

  if (config_.enable) {
    enabled_ = true;
    auto& server = ImeBridgeServer::Instance();
    server.AddRef();
    server.Start(config_);
  }

  LOG(INFO) << "[ImeBridge] Initialized. enable=" << config_.enable
            << ", socket_path=" << config_.socket_path;
}

ImeBridge::~ImeBridge() {
  if (enabled_) {
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

  return kNoop;
}

void ImeBridge::ApplyPendingActions(Context* ctx) {
  if (!ctx) {
    return;
  }

  auto& server = ImeBridgeServer::Instance();
  server.CleanupStaleClients();

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

