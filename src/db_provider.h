#pragma once

#include <string>
#include <vector>

#include "copilot_db.h"
#include "provider.h"

namespace rime {

class DBProvider : public Provider {
 public:
  DBProvider(const std::shared_ptr<CopilotDb>& db, int max) : db_(db), max_candidates_(max) {}
  virtual ~DBProvider() = default;

  void Clear() override { candidates_.clear(); }
  bool Predict(const std::string& input) override;

  std::vector<::copilot::Entry> Retrive(int timeout_us) const override { return candidates_; }

 private:
  std::shared_ptr<CopilotDb> db_;
  std::vector<::copilot::Entry> candidates_;
  int max_candidates_ = -1;
};

inline bool DBProvider::Predict(const std::string& input) {
  candidates_.clear();
  const auto* candidates = db_->Lookup(input);
  if (!candidates) {
    return false;
  }
  uint32_t size = std::min<uint32_t>(candidates->size, static_cast<uint32_t>(max_candidates_));
  candidates_.resize(size);
  // LOG(INFO) << "DBProvider::Predict: " << input << ", candidates size: " << candidates->size;
  auto* it = candidates->begin();
  for (uint32_t i = 0; i < size; ++i, ++it) {
    auto text = db_->GetEntryText(*it);
    // LOG(INFO) << "DBProvider::Predict: [" << i << "]: " << text << ", weight: " << it->weight;
    candidates_[i] = {text, it->weight};
  }
  return true;
}

}  // namespace rime
