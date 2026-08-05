#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <queue>
#include <set>
#include <string>
#include <thread>
#include <unordered_map>

#include "copilot_plugin.h"
#include "imk_client.h"

namespace rime {

class Context;

// Per-client 状态结构
struct ImeBridgeClientState {
  bool has_initial = false;   // 是否已记录初始状态
  bool initial_state = true;  // 第一次 set 时的 ascii_mode（整个会话保持不变）
  bool has_base = false;
  bool base = false;                                  // 每次 set cycle 的 base
  int depth = 0;                                      // set 嵌套层数
  bool current_target = true;                         // 最近一次 set 的目标值
  std::chrono::steady_clock::time_point last_active;  // 最后活动时间

  // Surrounding text context
  std::string char_before;
  std::string char_after;
  bool context_valid = false;
  std::chrono::steady_clock::time_point context_time;  // when char_before/after were pushed
};

// 待处理的 action
struct ImeBridgePendingAction {
  enum Type { kNone, kSet, kRestore, kReset, kUnregister, kActivate, kDeactivate };
  Type type = kNone;
  std::string client_key;
  bool ascii = true;    // for kSet
  bool stack = true;    // for kSet: if true, increment depth and save base
  bool restore = true;  // for kReset
  // for kReset: true when the server made this up because the last connection
  // for the client went away, false when the client actually asked for it. A
  // synthesized reset is moot once the client is back, an explicit one is not.
  bool synthesized = false;
};

// Socket-independent ImeBridge state machine: client registry, active-owner
// tracking, pending ascii-mode actions. Default-constructible and free of any
// socket/thread, so it can be unit-tested directly.
class ImeBridgeState {
 public:
  struct Config {
    bool enable = true;
    std::string socket_path = "/tmp/rime_copilot_ime.sock";
    bool debug = false;
    int client_timeout_minutes = 30;
    // A context older than this is treated as absent. Guards against a client
    // that lost focus without the terminal reporting it still owning the
    // surrounding text. 0 disables the check.
    int context_ttl_seconds = 60;
    // Identity announced to every client the moment it connects, so a client
    // reached through an ssh tunnel can tell *which* machine's IME it is about
    // to drive. Empty means "derive it": gethostname() truncated at the first
    // dot, which is what ssh's %L expands to -- the value a remote client gets
    // via `SetEnv LC_RIME_IME_HOST=%L`. Override only if the two disagree.
    std::string host_id;
  };
  struct ApplyResult {
    bool should_set = false;
    bool ascii_mode = true;
  };

  ImeBridgeState() = default;

  // Parse one JSON-Lines message and dispatch to the handlers below. Returns
  // the client key the message was attributed to ("app:instance"), or "" when
  // the message was malformed or not ours. HandleConnection uses the return
  // value to learn which clients a given connection is carrying.
  std::string ProcessMessage(const std::string& message);

  void HandleSet(const std::string& client_key, bool ascii, bool stack = true);
  void HandleRestore(const std::string& client_key);
  void HandleReset(const std::string& client_key, bool restore);
  void HandleUnregister(const std::string& client_key);
  void HandleContext(const std::string& client_key, const std::string& before,
                     const std::string& after);
  void HandleClearContext(const std::string& client_key);
  void HandleActivate(const std::string& client_key);
  void HandleDeactivate(const std::string& client_key);
  void TouchClient(const std::string& client_key);

  // Per-connection refcount for a client key. A reconnecting client briefly has
  // two live connections, so only the last one going away means it is really
  // gone — at which point we synthesize a reset(restore) so a killed client
  // cannot leave ascii_mode stuck.
  void RetainClientConnection(const std::string& client_key);
  void ReleaseClientConnection(const std::string& client_key);

  std::optional<SurroundingText> GetActiveContext();
  std::queue<ImeBridgePendingAction> TakePendingActions();
  ApplyResult ApplyAction(const ImeBridgePendingAction& action, bool current_ascii);
  void CleanupStaleClients();

  static std::string MakeClientKey(const std::string& app, const std::string& instance);

  // This machine's short hostname, or config_.host_id when set. Cached: it is
  // read once per connection and gethostname() is a syscall.
  const std::string& HostId() const;

  // The greeting written to a client the instant it connects, newline included.
  // A remote client compares data.host against the machine its ssh session came
  // from, and refuses a tunnel that leads somewhere else -- which is the only
  // thing standing between two laptops sharing one remote account and one of
  // them silently driving the other's input method.
  std::string BuildHello() const;

  Config config_;

 private:
  mutable std::mutex mutex_;
  mutable std::string host_id_cache_;
  mutable bool host_id_cached_ = false;
  std::unordered_map<std::string, ImeBridgeClientState> client_states_;
  std::string active_client_;
  std::unordered_map<std::string, int> conn_refs_;
  std::queue<ImeBridgePendingAction> pending_actions_;
  std::chrono::steady_clock::time_point last_cleanup_;
};

// 共享的 ImeBridge 服务器状态（跨所有 session 共享）
class ImeBridgeServer {
 public:
  using Config = ImeBridgeState::Config;
  using ApplyResult = ImeBridgeState::ApplyResult;

  static ImeBridgeServer& Instance();

  // Public so tests can construct a non-singleton server on a temp socket.
  ImeBridgeServer() = default;
  ~ImeBridgeServer();

  void Start(const Config& config);
  void Stop();
  void AddRef();
  void Release();

  bool IsRunning() const { return running_.load(); }
  bool IsDebug() const { return state_.config_.debug; }

  // Number of connection threads currently alive. Exposed so tests can assert
  // Stop() actually drained them.
  int live_connections() const {
    std::lock_guard<std::mutex> lock(conn_mutex_);
    return live_conns_;
  }

  // 获取活跃客户端的上下文信息（线程安全）
  std::optional<SurroundingText> GetActiveContext() { return state_.GetActiveContext(); }

  // 获取待处理的 actions（线程安全）
  std::queue<ImeBridgePendingAction> TakePendingActions() { return state_.TakePendingActions(); }

  // 应用单个 action，返回需要设置的 ascii_mode（带状态跟踪）
  ImeBridgeState::ApplyResult ApplyAction(const ImeBridgePendingAction& action,
                                          bool current_ascii) {
    return state_.ApplyAction(action, current_ascii);
  }

  // 清理超时客户端
  void CleanupStaleClients() { state_.CleanupStaleClients(); }

 private:
  void RunServer();
  void HandleConnection(int client_fd);

  ImeBridgeState state_;
  mutable std::mutex mutex_;  // guards the socket lifecycle (Start/Stop) only
  int server_fd_ = -1;
  std::atomic<bool> running_{false};
  std::unique_ptr<std::thread> server_thread_;
  std::atomic<int> ref_count_{0};

  // Connection registry. Deliberately guarded by its own mutex: Stop() holds
  // mutex_ while waiting for these threads, and they must never need mutex_ to
  // finish, or the two would deadlock.
  mutable std::mutex conn_mutex_;
  std::condition_variable conn_cv_;
  std::set<int> client_fds_;
  int live_conns_ = 0;
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
