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

}  // namespace

ImeBridge::ImeBridge(const Ticket& ticket) : CopilotPlugin<ImeBridge>(ticket) {
  // 从配置读取参数
  if (auto* config = engine_->schema()->config()) {
    config->GetBool("copilot/ime_bridge/enable", &config_.enable);
    config->GetString("copilot/ime_bridge/socket_path", &config_.socket_path);
    config->GetBool("copilot/ime_bridge/debug", &config_.debug);
  }

  if (config_.enable) {
    Start();
  }

  LOG(INFO) << "[ImeBridge] Initialized. enable=" << config_.enable
            << ", socket_path=" << config_.socket_path;
}

ImeBridge::~ImeBridge() {
  Stop();
  LOG(INFO) << "[ImeBridge] Destroyed.";
}

void ImeBridge::Start() {
  if (running_.load()) {
    return;
  }

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

void ImeBridge::Stop() {
  if (!running_.load()) {
    return;
  }

  running_.store(false);

  // 关闭 server socket 以中断 accept
  if (server_fd_ >= 0) {
    shutdown(server_fd_, SHUT_RDWR);
    close(server_fd_);
    server_fd_ = -1;
  }

  if (server_thread_ && server_thread_->joinable()) {
    server_thread_->join();
  }
  server_thread_.reset();

  // 删除 socket 文件
  unlink(config_.socket_path.c_str());

  LOG(INFO) << "[ImeBridge] Server stopped.";
}

void ImeBridge::RunServer() {
  while (running_.load()) {
    int client_fd = accept(server_fd_, nullptr, nullptr);
    if (client_fd < 0) {
      if (running_.load()) {
        LOG(WARNING) << "[ImeBridge] Accept failed: " << strerror(errno);
      }
      continue;
    }

    // 简单处理：同步读取一行 JSON
    HandleConnection(client_fd);
    close(client_fd);
  }
}

void ImeBridge::HandleConnection(int client_fd) {
  char buffer[kMaxMessageSize];
  std::string message;

  while (true) {
    ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
      break;
    }
    buffer[n] = '\0';
    message += buffer;

    // 处理每一行 JSON
    size_t pos;
    while ((pos = message.find('\n')) != std::string::npos) {
      std::string line = message.substr(0, pos);
      message = message.substr(pos + 1);

      if (!line.empty()) {
        ProcessMessage(line);
      }
    }
  }

  // 处理最后没有换行的消息
  if (!message.empty()) {
    ProcessMessage(message);
  }
}

void ImeBridge::ProcessMessage(const std::string& message) {
  try {
    auto j = json::parse(message);

    // 验证协议
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

    // 解析 source
    auto src = j.value("src", json::object());
    std::string app = src.value("app", "unknown");
    std::string instance = src.value("instance", "default");
    std::string client_key = MakeClientKey(app, instance);

    // 解析 data
    auto data = j.value("data", json::object());
    std::string action = data.value("action", "");

    if (config_.debug) {
      LOG(INFO) << "[ImeBridge] Received: client=" << client_key << ", action=" << action;
    }

    // 更新 active client
    {
      std::lock_guard<std::mutex> lock(mutex_);
      active_client_ = client_key;
    }

    if (action == "set") {
      bool ascii = data.value("ascii", true);
      HandleSet(client_key, ascii);
    } else if (action == "restore") {
      HandleRestore(client_key);
    } else if (action == "reset") {
      bool restore = data.value("restore", true);
      HandleReset(client_key, restore);
    } else if (action == "ping") {
      // ping 不需要特殊处理
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

std::string ImeBridge::MakeClientKey(const std::string& app, const std::string& instance) {
  return app + ":" + instance;
}

void ImeBridge::HandleSet(const std::string& client_key, bool ascii) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto& state = client_states_[client_key];

  // 设置 pending action
  pending_action_.type = ImeBridgePendingAction::kSet;
  pending_action_.client_key = client_key;
  pending_action_.ascii = ascii;

  // 更新 state（depth 在 ApplyPendingAction 时更新）
  state.current_target = ascii;

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleSet: client=" << client_key << ", ascii=" << ascii
              << ", depth=" << state.depth;
  }
}

void ImeBridge::HandleRestore(const std::string& client_key) {
  std::lock_guard<std::mutex> lock(mutex_);

  auto it = client_states_.find(client_key);
  if (it == client_states_.end() || it->second.depth == 0) {
    if (config_.debug) {
      LOG(INFO) << "[ImeBridge] HandleRestore: no state to restore for " << client_key;
    }
    return;
  }

  pending_action_.type = ImeBridgePendingAction::kRestore;
  pending_action_.client_key = client_key;

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleRestore: client=" << client_key
              << ", depth=" << it->second.depth;
  }
}

void ImeBridge::HandleReset(const std::string& client_key, bool restore) {
  std::lock_guard<std::mutex> lock(mutex_);

  pending_action_.type = ImeBridgePendingAction::kReset;
  pending_action_.client_key = client_key;
  pending_action_.restore = restore;

  if (config_.debug) {
    LOG(INFO) << "[ImeBridge] HandleReset: client=" << client_key << ", restore=" << restore;
  }
}

void ImeBridge::HandlePing(int client_fd) {
  // TODO: 返回状态信息
}

bool ImeBridge::ApplyPendingAction(Context* ctx) {
  if (!ctx) {
    return false;
  }

  ImeBridgePendingAction action;
  {
    std::lock_guard<std::mutex> lock(mutex_);
    if (pending_action_.type == ImeBridgePendingAction::kNone) {
      return false;
    }
    action = pending_action_;
    pending_action_.type = ImeBridgePendingAction::kNone;
  }

  bool current_ascii = ctx->get_option("ascii_mode");

  switch (action.type) {
    case ImeBridgePendingAction::kSet: {
      std::lock_guard<std::mutex> lock(mutex_);
      auto& state = client_states_[action.client_key];

      // 如果是第一次 set，记录 base
      if (state.depth == 0) {
        state.base = current_ascii;
        state.has_base = true;
      }
      state.depth++;

      ctx->set_option("ascii_mode", action.ascii);

      if (config_.debug) {
        LOG(INFO) << "[ImeBridge] Applied set: ascii=" << action.ascii << ", base=" << state.base
                  << ", depth=" << state.depth;
      }
      break;
    }

    case ImeBridgePendingAction::kRestore: {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = client_states_.find(action.client_key);
      if (it != client_states_.end() && it->second.depth > 0) {
        it->second.depth--;

        if (it->second.depth == 0 && it->second.has_base) {
          ctx->set_option("ascii_mode", it->second.base);
          it->second.has_base = false;

          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] Applied restore: restored to base=" << it->second.base;
          }
        } else {
          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] Applied restore: depth=" << it->second.depth;
          }
        }
      }
      break;
    }

    case ImeBridgePendingAction::kReset: {
      std::lock_guard<std::mutex> lock(mutex_);
      auto it = client_states_.find(action.client_key);
      if (it != client_states_.end()) {
        if (action.restore && it->second.depth > 0 && it->second.has_base) {
          ctx->set_option("ascii_mode", it->second.base);

          if (config_.debug) {
            LOG(INFO) << "[ImeBridge] Applied reset: restored to base=" << it->second.base;
          }
        }
        client_states_.erase(it);
      }
      break;
    }

    default:
      break;
  }

  return true;
}

ProcessResult ImeBridge::Process(const KeyEvent& key_event) {
  if (!engine_) {
    return kNoop;
  }

  auto* ctx = engine_->context();
  if (ctx) {
    ApplyPendingAction(ctx);
  }

  return kNoop;
}

}  // namespace rime
