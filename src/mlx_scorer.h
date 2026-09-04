#pragma once

// The same Scorer, on MLX instead of llama.cpp. Apple only.
//
// WHY IT EXISTS. The deployed cost of one decode-bearing scoring is ~9 ms
// after any idle over 50 ms, and of that, graph construction is 0.07 ms, the
// model mutex is 0, and KV-cache sizing has already been taken as far as
// llama.cpp's 256-cell floor allows. What is left is Metal command encoding,
// dispatch and synchronization, stretched ~5.7x by a downclocked core -- and
// the one mechanism identified that attacks it is issuing fewer dispatches.
// See docs/superpowers/specs/2026-09-04-scoring-latency-results.md.
//
// This is a SECOND BACKEND, not a replacement. llama.cpp stays: it is what
// Linux builds and what the prediction provider (llm.cc) uses, and it is the
// reference this one is verified against. Both are selected at runtime by
// `copilot/rerank/llm/backend` so the two can be compared on one machine
// rather than one build at a time.
//
// Three things about it are load-bearing and are easy to get wrong:
//
//   * RoPE rotates ADJACENT pairs (fast::rope with traditional=true), because
//     model.py trains that way to match llama.cpp's GGML_ROPE_TYPE_NORM.
//     HuggingFace's halves convention loads, runs, and returns entirely
//     plausible numbers that are wrong by up to 3.24 nats -- measured.
//
//   * The gguf is read directly. MLX's own loader converts ggml Q8_0 into its
//     packed 8-bit affine format (group_size 32), and the result agrees with
//     llama.cpp's logprobs to 0.0023 on the probe set. So this backend needs
//     NO new artifact: it reads the same file the schema already names, which
//     is what keeps a backend switch from becoming a deployment change.
//
//   * Tokenization comes from llama.cpp, loaded vocab-only. MLX ships no
//     tokenizer, and a reimplementation would be a second thing that can
//     disagree -- which would show up as a scoring difference and be blamed on
//     the model. vocab_only skips the weights, so this costs a vocab table.
//
// A missing mlx.metallib does NOT degrade: MLX throws std::out_of_range from
// deep inside its Metal device setup and the process terminates. Squirrel is
// that process. Loaded() is gated on an explicit check for that reason -- see
// mlx_scorer.cc's kMetallibNames.

#include <memory>
#include <string>
#include <vector>

#include "scorer.h"

namespace rime {

struct MlxScorerOptions {
  // copilot/rerank/llm/mlx_compile. mx::compile traces the scoring step once
  // and fuses it, which is the whole reason to be here -- measured worth 9-11%
  // of the score phase in the Python prototype. Defaults on; the switch exists
  // so the fusion's contribution stays separable from the backend's.
  bool compile = true;

  // copilot/rerank/llm/mlx_f16. Run the forward and the KV cache in float16
  // rather than float32.
  //
  // Not a free win and not a match for llama.cpp either, so it is a switch.
  // llama.cpp computes in F32 against an F16 KV cache; this runs the whole
  // body in f16, which is more aggressive. Measured: the score phase drops
  // ~20% (6.4 -> 5.2 ms in the deployed condition) because scoring is bound by
  // reading the KV cache, and halving it halves that read. The prefill barely
  // moves -- it is bound by reading weights, which are quantized either way.
  //
  // The cost is precision: agreement with llama.cpp's logprobs goes from
  // 0.0014 to 0.0187 nats. That is 2% of the 1.0-nat promotion threshold
  // (copilot/rerank/llm/margin), so it cannot flip a promotion the margin gate
  // would otherwise allow -- but it IS a divergence from the reference, and
  // anyone comparing the two backends' outputs should know which side of this
  // switch they are on. The final log-sum-exp stays in f32 regardless: an
  // 8573-wide reduction in f16 would throw away more than this buys.
  bool f16 = true;
};

class MlxScorer : public Scorer {
 public:
  explicit MlxScorer(std::string model_path, MlxScorerOptions options = {});
  ~MlxScorer() override;

  std::vector<CandidateScore> Score(const std::string& context,
                                    const std::vector<std::string>& candidates,
                                    ScoreTiming* timing) override;
  bool IsWarm(const std::string& context) const override;
  void WarmUp(const std::string& context) override;
  bool Loaded() const override;

 private:
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace rime
