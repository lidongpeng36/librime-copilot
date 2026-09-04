#pragma once

// The seam between re-ranking and the model.
//
// One virtual call, no llama.cpp types, no Rime types. Everything above this
// line is testable without loading 500 MB of weights, and the tests inject a
// fake -- which is the only reason the decision logic has real coverage.

#include <cstdint>
#include <string>
#include <vector>

namespace rime {

struct CandidateScore {
  float logprob = 0.0f;  // summed log-probability of the candidate's tokens
  int n_tokens = 0;      // as tokenized by the model, not characters
};

// Where one Score() call's wall time went. Every field is -1 until an
// implementation fills it, so "not measured" is distinguishable from a
// measured zero -- which matters for all three: an uncontended lock, a batch
// that needed no decode, and a call that never ran are different findings.
//
// It exists because the single number this used to report could not answer the
// question it was collected for. Live telemetry put scoring at p50 11.1ms
// where tools/bench_scorer.cc reports 2.1ms for the same batch geometry on the
// same machine, and the gap was unattributable: the timer wrapped the whole
// Score() call, INCLUDING its wait on the mutex the background prefill holds.
// Warms fire on every commit and every composition start, i.e. milliseconds
// before the Apply that then wants to score, so "the lock was held" was never
// an idle hypothesis -- it just had no way to be confirmed or killed.
//
// `n_decoded` is here for the same reason and answers the other half. Scoring
// cost is bimodal on exactly one thing: whether the batch needs a
// llama_decode at all. Every candidate's FIRST token is scored off the
// prefill's own last logits, so a window of single-token candidates submits
// nothing and costs ~0.18ms, while anything longer costs one decode and
// ~11ms. That was inferred from candidate character counts in the log, which
// is a proxy -- a character is not a token, and the log names only three of
// the scored candidates. This records the fact.
struct ScoreTiming {
  int64_t lock_us = -1;  // waiting for the model lock, i.e. for a background
                         // prefill to finish
  int64_t work_us = -1;  // inside it: tokenize, KV branch, decode, log-softmax
  int n_decoded = -1;    // candidate tokens submitted to llama_decode across
                         // every group; 0 means no decode ran at all
};

class Scorer {
 public:
  virtual ~Scorer() = default;

  // Scores each candidate as a continuation of `context`. Returns a vector the
  // same length as `candidates`. An implementation that cannot score (no model,
  // cold cache) returns an empty vector rather than fabricating numbers.
  //
  // `timing` may be null. When it is not, an implementation fills what it can
  // and leaves the rest at -1; a caller must not read an unfilled field as a
  // zero. Deliberately an out-parameter with no default argument rather than a
  // defaulted one: a default on a virtual is bound statically, so a caller
  // holding a Scorer* and one holding an LlmScorer* could silently disagree
  // about it. There is one caller (rerank_filter.cc) and one implementation,
  // so passing it explicitly costs nothing.
  virtual std::vector<CandidateScore> Score(const std::string& context,
                                            const std::vector<std::string>& candidates,
                                            ScoreTiming* timing) = 0;

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
