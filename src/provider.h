#pragma once

#include <string>
#include <vector>

namespace copilot {
struct Entry {
  std::string text;
  double weight;
};
};  // namespace copilot

namespace rime {

class Provider {
 public:
  virtual ~Provider() = default;

  virtual void OnBackspace() {}
  virtual void Clear() {}
  virtual bool Predict(const std::string& input) = 0;

  virtual std::vector<::copilot::Entry> Retrive(int timeout_us) const = 0;

 protected:
  // std::vector<table::Entry> candidates_;
};
}  // namespace rime
