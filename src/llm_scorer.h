#pragma once

// The model behind the Scorer seam (see scorer.h).
//
// One worker thread owns the llama_context for the lifetime of this object.
// WarmUp() posts a prefill job to it and returns immediately, never blocking
// the caller -- that is the whole point of the warm cache (warm_cache.h):
// the caller keeps typing while the model catches up in the background.
// Score() runs on the caller's thread and is serialized against the worker
// through the same mutex the worker holds while prefilling, so the two never
// touch the llama_context concurrently. In the normal path Score() only runs
// once WarmCache reports the context hot -- i.e. the worker is idle -- so
// that lock is uncontended.
//
// llama.cpp types stay out of this header, same as llm.h keeps them out for
// ClientImpl: everything here is forward-declared or hidden behind Impl, so
// including this file does not pull in <llama.h>.

#include <memory>
#include <string>
#include <vector>

#include "scorer.h"

namespace rime {

class LlmScorer : public Scorer {
 public:
  explicit LlmScorer(std::string model_path);
  ~LlmScorer() override;

  std::vector<CandidateScore> Score(const std::string& context,
                                    const std::vector<std::string>& candidates) override;
  bool IsWarm(const std::string& context) const override;
  void WarmUp(const std::string& context) override;

  // Whether the model is loaded and usable. Loading happens lazily, on the
  // first WarmUp() call, so this is false until then; on a load failure it
  // stays false forever (logged once, never retried) -- the fallback chain
  // (rerank_filter.cc) reads this to decide whether to score at all, but
  // NOT to decide whether to call WarmUp() -- see Scorer::Loaded().
  bool Loaded() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime
