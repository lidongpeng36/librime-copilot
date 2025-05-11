#pragma once

#include <rime/engine.h>
#include <rime/filter.h>

namespace rime {

template <typename T>
struct TranslationCreator {
  an<Translation> operator()(const an<Translation>& translation, const Engine* engine);
};

template <typename...>
class ChainFilter;

template <typename T, typename... Ts>
class ChainFilter<T, Ts...> : public ChainFilter<Ts...> {
 public:
  explicit ChainFilter(const Ticket& ticket) : ChainFilter<Ts...>(ticket) {}
  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override {
    auto* ctx = this->engine_->context();
    if (!ctx || !candidates) {
      return translation;
    }
    return DoApply(translation, candidates, this->engine_);
  }

  an<Translation> DoApply(an<Translation> translation, CandidateList* candidates,
                          const Engine* engine) {
    return ChainFilter<Ts...>::DoApply(TranslationCreator<T>()(translation, engine), candidates,
                                       engine);
  }

 private:
};

template <typename T>
class ChainFilter<T> : public Filter {
 public:
  using Filter::Filter;  // Inherit constructor
  an<Translation> Apply(an<Translation> translation, CandidateList* candidates) override {
    auto* ctx = engine_->context();
    if (!ctx || !candidates) {
      return translation;
    }
    return DoApply(translation, candidates, this->engine_);
  }
  an<Translation> DoApply(an<Translation> translation, CandidateList* candidates,
                          const Engine* engine) {
    return TranslationCreator<T>()(translation, engine);
  }
};

}  // namespace rime

namespace rime {

class AutoSpacerFilterTranslation;
class RawInputFilterTranslation;

using AutoSpacerFilter = ChainFilter<AutoSpacerFilterTranslation>;
using RawInputFilter = ChainFilter<RawInputFilterTranslation>;
using CopilotFilter = ChainFilter<RawInputFilterTranslation, AutoSpacerFilterTranslation>;

}  // namespace rime
