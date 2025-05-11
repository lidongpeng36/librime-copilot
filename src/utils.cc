#include "utils.h"

#include <atomic>
#include <mutex>

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
  CFTypeRef power_info = IOPSCopyPowerSourcesInfo();
  CFArrayRef power_sources = IOPSCopyPowerSourcesList(power_info);

  bool is_ac_power = false;
  if (CFArrayGetCount(power_sources) > 0) {
    CFDictionaryRef power_source =
        IOPSGetPowerSourceDescription(power_info, CFArrayGetValueAtIndex(power_sources, 0));
    if (power_source) {
      CFStringRef power_state =
          (CFStringRef)CFDictionaryGetValue(power_source, CFSTR(kIOPSPowerSourceStateKey));
      is_ac_power =
          (CFStringCompare(power_state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo);
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

  // 注册电源变化回调
  void RegisterCallback(std::function<void(bool /*is_ac_power*/)> callback);

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

  std::vector<std::function<void(bool)>> callbacks_;
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

void PowerMonitor::RegisterCallback(std::function<void(bool)> callback) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  callbacks_.emplace_back(std::move(callback));
}

void PowerMonitor::NotifyCallbacks(bool is_ac_power) {
  std::lock_guard<std::mutex> lock(callback_mutex_);
  for (const auto& cb : callbacks_) {
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

void RegisterPowerChange(std::function<void(bool /* is_ac_power */)> callback) {
  PowerMonitor::Instance().RegisterCallback(std::move(callback));
}

}  // namespace copilot
