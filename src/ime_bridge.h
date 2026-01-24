#pragma once

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include "copilot_plugin.h"

namespace rime {

class Context;

// Per-client 状态结构
struct ImeBridgeClientState {
  bool has_base = false;
  bool base = false;           // 第一次 set 之前的 ascii_mode
  int depth = 0;               // set 嵌套层数
  bool current_target = true;  // 最近一次 set 的目标值
};

// 待处理的 action
struct ImeBridgePendingAction {
  enum Type { kNone, kSet, kRestore, kReset };
  Type type = kNone;
  std::string client_key;
  bool ascii = true;      // for kSet
  bool restore = true;    // for kReset
};

// IME Bridge IPC 服务
// 作为独立的 Processor 实现，类似 AutoSpacer
class ImeBridge : public CopilotPlugin<ImeBridge> {
 public:
  struct Config {
    bool enable = true;
    std::string socket_path = "/tmp/rime_copilot_ime.sock";
    bool debug = false;
  };

  explicit ImeBridge(const Ticket& ticket);
  ~ImeBridge();

  // Processor interface
  ProcessResult Process(const KeyEvent& key_event);

  // 启动/停止 IPC 服务
  void Start();
  void Stop();

  bool IsRunning() const { return running_.load(); }

 private:
  // IPC 后台线程
  void RunServer();
  void HandleConnection(int client_fd);
  void ProcessMessage(const std::string& message);

  // Action 处理
  void HandleSet(const std::string& client_key, bool ascii);
  void HandleRestore(const std::string& client_key);
  void HandleReset(const std::string& client_key, bool restore);
  void HandlePing(int client_fd);

  // 应用 pending action 到 context
  bool ApplyPendingAction(Context* ctx);

  // 获取 client key
  static std::string MakeClientKey(const std::string& app, const std::string& instance);

  Config config_;
  int server_fd_ = -1;
  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> server_thread_;

  // 线程安全的状态管理
  mutable std::mutex mutex_;
  std::unordered_map<std::string, ImeBridgeClientState> client_states_;
  std::string active_client_;
  ImeBridgePendingAction pending_action_;
};

}  // namespace rime
