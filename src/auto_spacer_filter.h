#pragma once

#include <rime/filter.h>

namespace rime {
class AutoSpacerFilter : public Filter {
 public:
  explicit AutoSpacerFilter(const Ticket& ticket);
  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override;
  bool Convert(const an<Candidate>& original, CandidateQueue* result);

 private:
  bool is_en_ = true;
};
}  // namespace rime
