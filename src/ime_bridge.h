#pragma once

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <queue>
#include <string>
#include <thread>
#include <unordered_map>

#include "copilot_plugin.h"

namespace rime {

class Context;

// Per-client 状态结构
struct ImeBridgeClientState {
  bool has_initial = false;    // 是否已记录初始状态
  bool initial_state = true;   // 第一次 set 时的 ascii_mode（整个会话保持不变）
  bool has_base = false;
  bool base = false;           // 每次 set cycle 的 base
  int depth = 0;               // set 嵌套层数
  bool current_target = true;  // 最近一次 set 的目标值
  std::chrono::steady_clock::time_point last_active;  // 最后活动时间
};

// 待处理的 action
struct ImeBridgePendingAction {
  enum Type { kNone, kSet, kRestore, kReset, kUnregister };
  Type type = kNone;
  std::string client_key;
  bool ascii = true;      // for kSet
  bool restore = true;    // for kReset
};

// 共享的 ImeBridge 服务器状态（跨所有 session 共享）
class ImeBridgeServer {
 public:
  struct Config {
    bool enable = true;
    std::string socket_path = "/tmp/rime_copilot_ime.sock";
    bool debug = false;
    int client_timeout_minutes = 30;
  };

  // ApplyAction 返回值
  struct ApplyResult {
    bool should_set = false;
    bool ascii_mode = true;
  };

  static ImeBridgeServer& Instance();

  void Start(const Config& config);
  void Stop();
  void AddRef();
  void Release();
  
  bool IsRunning() const { return running_.load(); }
  bool IsDebug() const { return config_.debug; }
  
  // 获取待处理的 actions（线程安全）
  std::queue<ImeBridgePendingAction> TakePendingActions();
  
  // 应用单个 action，返回需要设置的 ascii_mode（带状态跟踪）
  ApplyResult ApplyAction(const ImeBridgePendingAction& action, bool current_ascii);
  
  // 清理超时客户端
  void CleanupStaleClients();

 private:
  ImeBridgeServer() = default;
  ~ImeBridgeServer();

  void RunServer();
  void HandleConnection(int client_fd);
  void ProcessMessage(const std::string& message);

  void HandleSet(const std::string& client_key, bool ascii);
  void HandleRestore(const std::string& client_key);
  void HandleReset(const std::string& client_key, bool restore);
  void HandleUnregister(const std::string& client_key);
  void TouchClient(const std::string& client_key);

  static std::string MakeClientKey(const std::string& app, const std::string& instance);

  Config config_;
  int server_fd_ = -1;
  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> server_thread_;
  std::atomic<int> ref_count_{0};

  mutable std::mutex mutex_;
  std::unordered_map<std::string, ImeBridgeClientState> client_states_;
  std::string active_client_;
  std::queue<ImeBridgePendingAction> pending_actions_;
  std::chrono::steady_clock::time_point last_cleanup_;
};

// IME Bridge Processor（每个 session 一个实例，共享服务器）
class ImeBridge : public CopilotPlugin<ImeBridge> {
 public:
  explicit ImeBridge(const Ticket& ticket);
  ~ImeBridge();

  ProcessResult Process(const KeyEvent& key_event);

 private:
  void ApplyPendingActions(Context* ctx);

  ImeBridgeServer::Config config_;
  bool enabled_ = false;
};

}  // namespace rime


