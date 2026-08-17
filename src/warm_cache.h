#pragma once

// Which context, if any, is already prefilled.
//
// One slot, not a map: a second context means the first is stale, so there is
// no eviction policy to get wrong. The state machine is pure and lives here so
// test/warm_cache_test.cc can drive every transition without a model or a
// thread.
//
// Invalidation is not detected, it is implied: a mouse click, a window switch
// and an arrow key all change the context, which changes the key, which misses.

#include <mutex>
#include <string>

namespace rime {

class WarmCache {
 public:
  enum class State { kCold, kWarming, kHot };

  State Lookup(const std::string& key) const {
    if (key.empty()) {
      return State::kCold;  // no context is not a cacheable state
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (key != key_) {
      return State::kCold;
    }
    return hot_ ? State::kHot : State::kWarming;
  }

  // Claims the slot for `key`. Any previously prefilled context is discarded.
  void BeginWarm(const std::string& key) {
    if (key.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (key == key_ && hot_) {
      return;  // already there
    }
    key_ = key;
    hot_ = false;
  }

  // Marks the pending key prefilled. Ignored when the context moved on while
  // the forward pass was running -- publishing then would hand the filter a
  // cache belonging to text the user has already left behind.
  void Publish(const std::string& key) {
    if (key.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (key != key_) {
      return;
    }
    hot_ = true;
  }

 private:
  mutable std::mutex mutex_;
  std::string key_;
  bool hot_ = false;
};

}  // namespace rime
