#include "raw_input_filter.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/filter.h>
#include <rime/schema.h>
#include <rime/ticket.h>
#include <rime/translation.h>

namespace rime {

class RawInputFilterTranslation : public PrefetchTranslation {
 public:
  RawInputFilterTranslation(an<Translation> translation, const std::string& input,
                            int page_size = 0)
      : PrefetchTranslation(translation), input_(input), page_size_(page_size) {
    DLOG(INFO) << "[RawInputFilter] input: '" << input << "' page_size: " << page_size;
  }

 protected:
  bool Replenish() override;

  std::string input_;
  bool inserted_ = false;
  int page_size_ = 0;
};

bool RawInputFilterTranslation::Replenish() {
  auto next = translation_->Peek();
  translation_->Next();
  if (!next) {
    return !cache_.empty();
  }
  if (inserted_) {
    cache_.push_back(next);
    return !cache_.empty();
  }
  if (next->start() > 0) {
    inserted_ = true;
    cache_.push_back(next);
    return !cache_.empty();
  }

  inserted_ = true;
  auto raw = New<SimpleCandidate>("raw", 0, input_.size(), input_);
  if (next->type() == "sentence") {
    cache_.push_back(raw);
    cache_.push_back(next);
    return true;
  }
  size_t n = (input_.size() + 1) / 2;
  for (int i = 0; i < page_size_ - 1; ++i) {
    DLOG(INFO) << "[CAND] " << i << ": '" << next->text() << "'|" << next->type() << "|"
               << next->start() << "|" << next->end() << "|" << next->quality();
    if (next->text() == input_) {
      cache_.push_back(next);
      return true;
    }
    if (next->end() < input_.size()) {
      cache_.push_back(raw);
      cache_.push_back(next);
      return true;
    }
    cache_.push_back(next);
    next = translation_->Peek();
    translation_->Next();
    if (!next) {
      break;
    }
  }
  cache_.push_back(raw);
  return true;
}

}  // namespace rime

namespace rime {

RawInputFilter::RawInputFilter(const Ticket& ticket) : Filter(ticket) {}

bool RawInputFilter::Convert(const an<Candidate>& original, CandidateQueue* result) {
  auto& text = original->text();
  result->push_back(New<ShadowCandidate>(original, "raw_input", text));
  return true;
}

an<Translation> RawInputFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  auto* ctx = engine_->context();
  const auto& input = ctx->input();
  if (!ctx || input.empty() || !candidates) {
    return translation;
  }

  return New<RawInputFilterTranslation>(translation, input, engine_->schema()->page_size());
}

}  // namespace rime
