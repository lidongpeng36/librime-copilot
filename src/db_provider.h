#pragma once

#include <string>
#include <vector>

#include "copilot_db.h"
#include "history.h"
#include "provider.h"

namespace rime {

class DBProvider : public Provider {
 public:
  DBProvider(const std::shared_ptr<CopilotDb>& db,
             const std::shared_ptr<::copilot::History>& history, int max, uint32_t hints)
      : db_(db), history_(history) {
    if (max > 0) {
      max_candidates_ = max;
    }
    if (hints > 0) {
      max_hints_ = std::min<uint32_t>(max_hints_, hints);
    }
  }
  virtual ~DBProvider() = default;

  void Clear() override { candidates_.clear(); }
  bool Predict(const std::string& input) override;

  std::vector<::copilot::Entry> Retrive(int timeout_us) const override { return candidates_; }

 private:
  std::list<::copilot::Entry> Lookup(const std::string& input) const {
    std::list<::copilot::Entry> result;
    auto* candidates = db_->Lookup(input);
    if (!candidates) {
      return result;
    }
    uint32_t size = std::min(candidates->size, max_candidates_);
    auto* it = candidates->begin();
    for (uint32_t i = 0; i < size; ++i, ++it) {
      auto text = db_->GetEntryText(*it);
      result.push_back({text, it->weight, ::copilot::ProviderType::kDB});
    }
    return result;
  }
  std::shared_ptr<CopilotDb> db_;
  std::vector<::copilot::Entry> candidates_;
  uint32_t max_candidates_ = static_cast<uint32_t>(-1);
  uint32_t max_hints_ = 10;
  std::shared_ptr<::copilot::History> history_;
};

inline bool DBProvider::Predict(const std::string& input) {
  candidates_.clear();
  auto hist = history_->back();
  auto candidates = Lookup(hist);
  for (uint32_t i = 2; i < max_hints_; ++i) {
    auto curr = history_->get_chars(i);
    // LOG(INFO) << "DBProvider::Predict: " << i << ", curr: " << curr << ", hist: " << hist
    //           << " max_hints_: " << max_hints_;
    if (curr == hist) {
      // LOG(INFO) << "DBProvider::Predict: " << i << ", curr == hist";
      break;
    }
    auto hint_candidates = Lookup(curr);
    hist.swap(curr);
    candidates.splice(candidates.end(), hint_candidates);
  }
  if (candidates.empty()) {
    return false;
  }
  candidates.sort(
      [](const ::copilot::Entry& a, const ::copilot::Entry& b) { return a.weight > b.weight; });
  candidates_ = {
      candidates.begin(),
      std::next(candidates.begin(), std::min<uint32_t>(candidates.size(), max_candidates_))};
  return true;
}

}  // namespace rime
