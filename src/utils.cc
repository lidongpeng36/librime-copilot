#include "utils.h"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <utility>
#include <vector>

#ifndef __APPLE__
#include <condition_variable>
#include <thread>
#endif

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#elif __linux__
#include <fstream>
#endif

namespace copilot {
// Returns true if connected to AC power, false if on battery
bool IsACPowerConnected() {
#ifdef _WIN32
  SYSTEM_POWER_STATUS status;
  if (GetSystemPowerStatus(&status)) {
    return status.ACLineStatus == 1;
  }
  return false;

#elif __APPLE__
  // Every IOPS call here can return NULL (no power source, e.g. a desktop Mac,
  // or a transient query failure); CFArrayGetCount/CFRelease(NULL) would crash.
  CFTypeRef power_info = IOPSCopyPowerSourcesInfo();
  if (!power_info) {
    return true;  // can't tell: assume AC rather than disabling the LLM
  }
  CFArrayRef power_sources = IOPSCopyPowerSourcesList(power_info);
  if (!power_sources) {
    CFRelease(power_info);
    return true;
  }

  bool is_ac_power = true;  // no battery listed == desktop == on AC
  if (CFArrayGetCount(power_sources) > 0) {
    CFDictionaryRef power_source =
        IOPSGetPowerSourceDescription(power_info, CFArrayGetValueAtIndex(power_sources, 0));
    if (power_source) {
      CFStringRef power_state =
          (CFStringRef)CFDictionaryGetValue(power_source, CFSTR(kIOPSPowerSourceStateKey));
      is_ac_power = power_state && (CFStringCompare(power_state, CFSTR(kIOPSACPowerValue), 0) ==
                                    kCFCompareEqualTo);
    }
  }

  CFRelease(power_sources);
  CFRelease(power_info);
  return is_ac_power;

#elif __linux__
  std::ifstream file("/sys/class/power_supply/AC/online");
  if (!file.is_open()) {
    return false;  // Assume battery if unable to read
  }
  int status = 0;
  file >> status;
  return status == 1;

#else
  // Unsupported platform
  return false;
#endif
}

}  // namespace copilot

namespace copilot {
class PowerMonitor {
 public:
  static PowerMonitor& Instance();

  // 注册电源变化回调, 返回可用于注销的 token
  PowerChangeToken RegisterCallback(std::function<void(bool /*is_ac_power*/)> callback);
  void UnregisterCallback(PowerChangeToken token);

 private:
  PowerMonitor();
  ~PowerMonitor();

  void StopMonitoring();
  void NotifyCallbacks(bool is_ac_power);

#if defined(__APPLE__)
  void StartMacOSMonitor();
  static void MacOSPowerChangeCallback(void* context);
#else
  void StartMonitoring();
  void PollingLoop();
#endif

  std::vector<std::pair<PowerChangeToken, std::function<void(bool)>>> callbacks_;
  PowerChangeToken next_token_ = 1;
  std::mutex callback_mutex_;
  std::atomic<bool> last_power_state_;
#if defined(__APPLE__)
  CFRunLoopSourceRef source_ = nullptr;
#else
  std::atomic<bool> running_{false};
  std::thread monitor_thread_;
  std::condition_variable cond_;
  std::mutex mutex_;
#endif
};

PowerMonitor& PowerMonitor::Instance() {
  static PowerMonitor instance;
  return instance;
}

PowerMonitor::PowerMonitor() : last_power_state_(IsACPowerConnected()) {
#if defined(__APPLE__)
  StartMacOSMonitor();
#else
  StartMonitoring();
#endif
}

PowerMonitor::~PowerMonitor() { StopMonitoring(); }

PowerChangeToken PowerMonitor::RegisterCallback(std::function<void(bool)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  PowerChangeToken token = next_token_++;
  callbacks_.emplace_back(token, std::move(callback));
  return token;
}

void PowerMonitor::UnregisterCallback(PowerChangeToken token) {
  if (token == 0) {
    return;
  }
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callbacks_.erase(std::remove_if(callbacks_.begin(), callbacks_.end(),
                                  [token](const auto& entry) { return entry.first == token; }),
                   callbacks_.end());
}

void PowerMonitor::NotifyCallbacks(bool is_ac_power) {
  // Copy under the lock, then invoke unlocked: a callback that unregisters
  // itself (or registers another) would otherwise deadlock on callback_mutex_.
  std::vector<std::function<void(bool)>> callbacks;
  {
    std::lock_guard<std::mutex> lock(callback_mutex_);
    callbacks.reserve(callbacks_.size());
    for (const auto& [token, cb] : callbacks_) {
      callbacks.push_back(cb);
    }
  }
  for (const auto& cb : callbacks) {
    cb(is_ac_power);
  }
}

void PowerMonitor::StopMonitoring() {
#if defined(__APPLE__)
  if (source_) {
    CFRunLoopRemoveSource(CFRunLoopGetCurrent(), source_, kCFRunLoopDefaultMode);
    CFRelease(source_);
    source_ = nullptr;
  }
#else
  running_ = false;
  cond_.notify_all();  // 唤醒等待的线程以便及时退出

  if (monitor_thread_.joinable()) {
    monitor_thread_.join();
  }
#endif
}

#ifndef __APPLE__
void PowerMonitor::StartMonitoring() {
  if (running_.exchange(true)) return;
  monitor_thread_ = std::thread(&PowerMonitor::PollingLoop, this);
}

void PowerMonitor::PollingLoop() {
  while (running_) {
    bool current_state = IsACPowerConnected();
    if (current_state != last_power_state_) {
      last_power_state_ = current_state;
      NotifyCallbacks(current_state);
    }
    std::unique_lock<std::mutex> lock(mutex_);
    cond_.wait_for(lock, std::chrono::seconds(5), [this]() { return !running_; });
  }
}
#else
void PowerMonitor::StartMacOSMonitor() {
  source_ = IOPSNotificationCreateRunLoopSource(MacOSPowerChangeCallback, nullptr);
  if (source_) {
    CFRunLoopAddSource(CFRunLoopGetCurrent(), source_, kCFRunLoopDefaultMode);
  }
}

void PowerMonitor::MacOSPowerChangeCallback(void*) {
  auto& instance = PowerMonitor::Instance();
  bool current_state = IsACPowerConnected();
  if (current_state != instance.last_power_state_) {
    instance.last_power_state_ = current_state;
    instance.NotifyCallbacks(current_state);
  }
}
#endif

PowerChangeToken RegisterPowerChange(std::function<void(bool /* is_ac_power */)> callback) {
  return PowerMonitor::Instance().RegisterCallback(std::move(callback));
}

void UnregisterPowerChange(PowerChangeToken token) {
  PowerMonitor::Instance().UnregisterCallback(token);
}

}  // namespace copilot
