#pragma once

// The seam between re-ranking and the model.
//
// One virtual call, no llama.cpp types, no Rime types. Everything above this
// line is testable without loading 500 MB of weights, and the tests inject a
// fake -- which is the only reason the decision logic has real coverage.

#include <string>
#include <vector>

namespace rime {

struct CandidateScore {
  float logprob = 0.0f;  // summed log-probability of the candidate's tokens
  int n_tokens = 0;      // as tokenized by the model, not characters
};

class Scorer {
 public:
  virtual ~Scorer() = default;

  // Scores each candidate as a continuation of `context`. Returns a vector the
  // same length as `candidates`. An implementation that cannot score (no model,
  // cold cache) returns an empty vector rather than fabricating numbers.
  virtual std::vector<CandidateScore> Score(const std::string& context,
                                            const std::vector<std::string>& candidates) = 0;

  // Whether `context` is already prefilled and scoring it would be cheap.
  virtual bool IsWarm(const std::string& context) const = 0;

  // Prefill `context` in the background. Returns immediately.
  virtual void WarmUp(const std::string& context) = 0;

  // Whether this scorer is currently able to produce real scores at all --
  // distinct from IsWarm(), which is about one specific context. A caller
  // gates *scoring* on this, not on whether to attempt WarmUp(): for an
  // implementation that loads lazily (LlmScorer), WarmUp() is what makes this
  // go true in the first place, so gating WarmUp() itself behind Loaded()
  // would mean it could never become true.
  virtual bool Loaded() const = 0;
};

}  // namespace rime
