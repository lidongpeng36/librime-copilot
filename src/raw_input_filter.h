#pragma once

#include <rime/filter.h>

namespace rime {
class RawInputFilter : public Filter {
 public:
  explicit RawInputFilter(const Ticket& ticket);
  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override;
  bool Convert(const an<Candidate>& original, CandidateQueue* result);

 private:
};
}  // namespace rime
