#pragma once

#include <functional>
#include <string>
#include <type_traits>

#include <rime/processor.h>

namespace rime {

using CopilotCallback = std::function<void(const std::string&)>;

namespace traits {
template <typename, typename T>
struct HasProcessWithOutputImpl : std::false_type {};

template <typename C>
struct HasProcessWithOutputImpl<
    C, std::void_t<decltype(std::declval<C>().Process(
           std::declval<const KeyEvent&>(), std::declval<std::string*>()))>> : std::true_type {};

template <typename T>
using HasProcessWithOutput = HasProcessWithOutputImpl<T, void>;

template <typename, typename T>
struct HasProcessWithoutOutputImpl : std::false_type {};

template <typename C>
struct HasProcessWithoutOutputImpl<
    C, std::void_t<decltype(std::declval<C>().Process(std::declval<const KeyEvent&>()))>>
    : std::true_type {};

template <typename T>
using HasProcessWithoutOutput = HasProcessWithoutOutputImpl<T, void>;
}  // namespace traits

template <typename T>
class CopilotPlugin : public Processor {
 public:
  CopilotPlugin(const Ticket& ticket, CopilotCallback on_accept = nullptr,
                CopilotCallback on_noop = nullptr)
      : Processor(ticket), on_accept_(on_accept), on_noop_(on_noop) {}

  ProcessResult ProcessKeyEvent(const KeyEvent& key_event) override {
    if constexpr (traits::HasProcessWithOutput<T>::value) {
      std::string data;
      auto ret = static_cast<T*>(this)->Process(key_event, &data);
      if (ret == kAccepted && on_accept_) {
        on_accept_(data);
      } else if (ret == kNoop && on_noop_) {
        on_noop_(data);
      }
      return ret;
    } else if constexpr (traits::HasProcessWithoutOutput<T>::value) {
      return static_cast<T*>(this)->Process(key_event);
    } else {
      return kNoop;
    }
  }

 private:
  CopilotCallback on_accept_;
  CopilotCallback on_noop_;
};

template <typename T>
class CopilotPluginComponent : public CopilotPlugin<T>::Component {
 public:
  T* Create(const Ticket& ticket) override { return new T(ticket); }
};

}  // namespace rime
