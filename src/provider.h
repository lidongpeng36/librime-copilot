#pragma once

#include <ostream>
#include <string>
#include <vector>

namespace copilot {

enum struct ProviderType : uint8_t {
  kLLM = 0,
  kDB = 1,
};

static inline std::ostream& operator<<(std::ostream& os, ProviderType type) {
  switch (type) {
    case ProviderType::kLLM:
      return os << "LLM";
    case ProviderType::kDB:
      return os << "DB";
    default:
      return os << "Unknown";
  }
}

struct Entry {
  std::string text;
  double weight;
  ProviderType type = ProviderType::kDB;
};

static inline std::ostream& operator<<(std::ostream& os, const Entry& entry) {
  return os << "Entry{text: '" << entry.text << "'|" << entry.weight << "|" << entry.type << "}";
}

};  // namespace copilot

namespace rime {

class Provider {
 public:
  virtual ~Provider() = default;

  virtual void OnBackspace() {}
  virtual void Clear() {}
  virtual int Rank() const { return -1; }
  virtual bool Predict(const std::string& input) = 0;

  virtual std::vector<::copilot::Entry> Retrive(int timeout_us) const = 0;

 protected:
  // std::vector<table::Entry> candidates_;
};
}  // namespace rime
