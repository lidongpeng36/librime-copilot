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

// Where the model runs, and with how many threads. Both were hard-coded until
// tools/bench_scorer.cc made them measurable, and both defaults below are the
// values that were hard-coded -- a schema setting neither behaves as it always
// has. Measured on an M4 they are also the right defaults, but on core time,
// NOT on latency: 1.93 ms of CPU per scoring on the GPU against 14.79 on four
// CPU threads (7.7x), while the two are within a millisecond of each other on
// score p50 and both sit well inside the p99 < 10 ms budget. See the tool's
// header for the full table and for the two timing traps that made earlier
// measurements claim a latency winner in each direction.
struct LlmScorerOptions {
  // copilot/rerank/llm/n_gpu_layers. 99 offloads everything (Metal on Apple,
  // CUDA where built); 0 keeps the whole model on the CPU. Worth turning off
  // on a machine with no usable GPU -- not otherwise: measured, CPU-only buys
  // a fraction of a millisecond of latency for 7.7x the core time.
  int n_gpu_layers = 99;
  // copilot/rerank/llm/n_threads. 0 means hardware_concurrency(). Measured
  // inert while the layers are on the GPU, and decisive once they are not: on
  // a 4-P-core M4, 8 threads is worse than 4 on BOTH latency and core time,
  // because the extra threads land on efficiency cores -- and
  // hardware_concurrency() there is 10. So: leave it at 0 unless
  // n_gpu_layers is 0, and then set it to the performance-core count.
  int n_threads = 0;
};

class LlmScorer : public Scorer {
 public:
  explicit LlmScorer(std::string model_path, LlmScorerOptions options = {});
  ~LlmScorer() override;

  std::vector<CandidateScore> Score(const std::string& context,
                                    const std::vector<std::string>& candidates,
                                    ScoreTiming* timing) override;
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
