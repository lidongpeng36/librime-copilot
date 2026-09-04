#include "mlx_scorer.h"

#include <dlfcn.h>

#include <ggml.h>
#include <gguf.h>
#include <llama.h>
#include <mlx/mlx.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>

#include <glog/logging.h>

#include "scoring_form.h"  // BuildScoringContext, TokenizeScoringForm
#include "warm_cache.h"

namespace mx = mlx::core;

namespace rime {

namespace {

// Where MLX looks for its Metal kernels, relative to the binary that loaded
// libmlx (ml-explore/mlx#2061). Checked BEFORE any MLX call, because a miss is
// not a degradation: MLX throws std::out_of_range from inside its device setup
// and nothing catches it, so the input method process dies. Every other
// failure on this path -- no model, a load error, a cold cache -- falls back to
// the db re-ranker and logs. This one has to as well.
constexpr const char* kMetallibNames[] = {"mlx.metallib", "Resources/mlx.metallib"};

// Q8_0's block. ggml quantizes in blocks of 32 and MLX's gguf loader carries
// that through as its affine group size; the scales tensor's width is the
// authority (see ReadGroupSize) and this is only the expectation it is checked
// against.
constexpr int kExpectedGroupSize = 32;
constexpr int kBits = 8;

std::optional<std::string> FindMetallib() {
  Dl_info info;
  if (dladdr(reinterpret_cast<const void*>(&FindMetallib), &info) == 0 || !info.dli_fname) {
    return std::nullopt;
  }
  const std::filesystem::path self(info.dli_fname);
  for (const char* name : kMetallibNames) {
    auto candidate = self.parent_path() / name;
    if (std::filesystem::exists(candidate)) {
      return candidate.string();
    }
  }
  return std::nullopt;
}

// The model's shape, read from the gguf rather than hard-coded: a retrain that
// changes depth or width must not silently score against the wrong geometry.
struct Shape {
  int n_layers = 0;
  int d_model = 0;
  int n_heads = 0;
  int n_kv_heads = 0;
  int head_dim = 0;
  float rms_eps = 1e-5f;
  float rope_theta = 10000.0f;
  int group_size = kExpectedGroupSize;
};

// A quantized linear, with its scales and biases already found. Resolved once
// at load rather than looked up per forward: the naive version built
// `"blk." + std::to_string(i) + ".attn_q.weight"` and hashed it, six times per
// layer, on every forward -- 60 string allocations and 60 hash lookups of host
// work inside the timed path, which is exactly the kind of per-call CPU cost
// a downclocked core multiplies by ~5.7. llama.cpp has no equivalent, so
// leaving it in would have measured as a property of MLX.
struct QLinear {
  mx::array w;
  mx::array scales;
  mx::array biases;
};

struct Layer {
  mx::array attn_norm;
  mx::array ffn_norm;
  QLinear q, k, v, o, gate, up, down;
};

}  // namespace

struct MlxScorer::Impl {
  Impl(std::string model_path, MlxScorerOptions options)
      : model_path_(std::move(model_path)), options_(options) {
    worker_ = std::thread([this] { WorkerLoop(); });
  }

  ~Impl() {
    {
      std::lock_guard<std::mutex> lock(queue_mutex_);
      shutdown_ = true;
    }
    queue_cv_.notify_one();
    worker_.join();
    if (vocab_model_) {
      llama_model_free(vocab_model_);
    }
  }

  bool Loaded() const { return loaded_.load(std::memory_order_acquire); }

  bool IsWarm(const std::string& context) const {
    return warm_cache_.Lookup(context) == WarmCache::State::kHot;
  }

  // Identical contract to LlmScorer::WarmUp -- claims the slot, hands the work
  // to the worker, never blocks. The dedupe and the ordering against
  // BeginWarm are the same and for the same reasons; see llm_scorer.cc, whose
  // comment on that ordering describes a failure this would reproduce exactly.
  void WarmUp(const std::string& context) {
    if (context.empty()) {
      return;
    }
    if (warm_cache_.Lookup(context) == WarmCache::State::kHot) {
      return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    if ((has_pending_ && pending_context_ == context) ||
        (has_active_ && active_context_ == context)) {
      return;
    }
    warm_cache_.BeginWarm(context);
    pending_context_ = context;
    has_pending_ = true;
    queue_cv_.notify_one();
  }

  std::vector<CandidateScore> Score(const std::string& context,
                                    const std::vector<std::string>& candidates,
                                    ScoreTiming* timing) {
    if (warm_cache_.Lookup(context) != WarmCache::State::kHot) {
      return {};
    }
    const auto lock_t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(model_mutex_);
    const auto work_t0 = std::chrono::steady_clock::now();
    if (timing) {
      timing->lock_us =
          std::chrono::duration_cast<std::chrono::microseconds>(work_t0 - lock_t0).count();
    }
    struct WorkTimer {
      ScoreTiming* out;
      std::chrono::steady_clock::time_point t0;
      ~WorkTimer() {
        if (out) {
          out->work_us = std::chrono::duration_cast<std::chrono::microseconds>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        }
      }
    } work_timer{timing, work_t0};
    if (warmed_context_ != context || !cache_k_.has_value()) {
      return {};
    }
    return ScoreGroup(candidates, timing);
  }

 private:
  // ---- tokenization -------------------------------------------------------

  std::vector<llama_token> Tokenize(const std::string& text, bool add_special) const {
    int n = -llama_tokenize(vocab_, text.data(), (int)text.size(), nullptr, 0, add_special, true);
    std::vector<llama_token> tokens(n > 0 ? n : 0);
    if (n > 0) {
      llama_tokenize(vocab_, text.data(), (int)text.size(), tokens.data(), n, add_special, true);
    }
    return tokens;
  }

  // ---- the forward --------------------------------------------------------

  mx::array QuantMatmul(const mx::array& x, const QLinear& l) const {
    return mx::quantized_matmul(x, l.w, l.scales, l.biases,
                                /*transpose=*/true, shape_.group_size, kBits);
  }

  const mx::array& W(const std::string& name) const { return weights_.at(name); }

  QLinear Resolve(const std::string& name) const {
    return QLinear{W(name + ".weight"), W(name + ".scales"), W(name + ".biases")};
  }

  // One transformer forward. `offset` is the position the first token sits at,
  // which is 0 for the prefill and the context length for the candidate step;
  // it feeds RoPE and is why the two share this function rather than diverging.
  //
  // `cache` non-null means the candidate step: the prefix keys and values are
  // BROADCAST to the candidate batch rather than copied per candidate. That is
  // the whole of llama.cpp's per-candidate `seq_rm` plus `seq_cp` -- which walk
  // cells, and which made n_ctx_seq a linear term in its cost -- collapsing
  // into a view.
  std::pair<mx::array, std::vector<std::pair<mx::array, mx::array>>> Forward(
      const mx::array& tokens, int offset,
      const std::vector<std::pair<mx::array, mx::array>>* cache, bool last_only = false) const {
    const int batch = tokens.shape(0);
    const int t = tokens.shape(1);
    mx::array x = mx::take(*embed_, mx::reshape(tokens, {batch * t}), 0);
    x = mx::reshape(x, {batch, t, shape_.d_model});

    // "causal" rather than a materialized triangular mask: it selects MLX's
    // fused attention kernel, which is the point of being on this backend at
    // all -- a hand-built mask array is an extra allocation, an extra input to
    // the graph, and it opts out of the fusion.
    //
    // Empty in the candidate step, where t == 1 per candidate and the whole
    // prefix is in the past by construction, so there is nothing to mask.
    const std::string mask_mode = (t > 1) ? "causal" : "";

    std::vector<std::pair<mx::array, mx::array>> new_cache;
    new_cache.reserve(shape_.n_layers);
    const float scale = 1.0f / std::sqrt(static_cast<float>(shape_.head_dim));
    for (int i = 0; i < shape_.n_layers; ++i) {
      const Layer& L = layers_[i];
      mx::array h = mx::fast::rms_norm(x, L.attn_norm, shape_.rms_eps);
      auto heads = [&](const mx::array& v, int n) {
        return mx::transpose(mx::reshape(v, {batch, t, n, shape_.head_dim}), {0, 2, 1, 3});
      };
      mx::array q = heads(QuantMatmul(h, L.q), shape_.n_heads);
      mx::array k = heads(QuantMatmul(h, L.k), shape_.n_kv_heads);
      mx::array v = heads(QuantMatmul(h, L.v), shape_.n_kv_heads);
      // traditional = true: adjacent pairs, matching GGML_ROPE_TYPE_NORM and
      // tools/rime_train/model.py. See the header -- the other convention is
      // wrong by up to 3.24 nats and says nothing about it.
      q = mx::fast::rope(q, shape_.head_dim, /*traditional=*/true, shape_.rope_theta, 1.0f, offset);
      k = mx::fast::rope(k, shape_.head_dim, /*traditional=*/true, shape_.rope_theta, 1.0f, offset);
      if (cache) {
        const mx::array& ck = (*cache)[i].first;
        const mx::array& cv = (*cache)[i].second;
        mx::array bk = ck, bv = cv;
        if (ck.shape(0) != batch) {
          auto s = ck.shape();
          s[0] = batch;
          bk = mx::broadcast_to(ck, s);
          bv = mx::broadcast_to(cv, s);
        }
        k = mx::concatenate({bk, k}, 2);
        v = mx::concatenate({bv, v}, 2);
      }
      new_cache.emplace_back(k, v);
      mx::array o = mx::fast::scaled_dot_product_attention(q, k, v, scale, mask_mode);
      o = mx::reshape(mx::transpose(o, {0, 2, 1, 3}), {batch, t, shape_.d_model});
      x = mx::add(x, QuantMatmul(o, L.o));
      mx::array y = mx::fast::rms_norm(x, L.ffn_norm, shape_.rms_eps);
      mx::array gate = QuantMatmul(y, L.gate);
      mx::array silu = mx::multiply(gate, mx::sigmoid(gate));
      x = mx::add(x, QuantMatmul(mx::multiply(silu, QuantMatmul(y, L.up)), L.down));
    }
    // Every position but the last, dropped BEFORE the output projection. The
    // prefill wants one row of logits out of 64 and was computing all 64 and
    // then slicing -- 281 M MAC against 4, i.e. 13% of the whole prefill spent
    // on rows immediately thrown away. llama.cpp never paid it: it marks one
    // position with `batch_.logits[k] = (k == last)` and ggml skips the rest.
    // The candidate step passes false, because there every submitted position's
    // logits predict that candidate's next token.
    if (last_only && t > 1) {
      x = mx::slice(x, {0, t - 1, 0}, {batch, t, shape_.d_model});
    }
    x = mx::fast::rms_norm(x, *out_norm_, shape_.rms_eps);
    return {QuantMatmul(x, *out_proj_), std::move(new_cache)};
  }

  // The prefill as one traced function. Same argument as CandidateTail(): the
  // cost being attacked is Metal command encoding and dispatch, and fusion is
  // what reduces the dispatch count -- leaving the longer of the two forwards
  // uncompiled would be leaving the mechanism half applied.
  //
  // Flattened output because mx::compile speaks vector<array>: [logits, k0, v0,
  // ..., kN, vN]. mx::compile keys its own cache on input shapes, so a context
  // of a different length re-traces rather than silently reusing a graph built
  // for another length.
  const std::function<std::vector<mx::array>(const std::vector<mx::array>&)>& PrefillFn() {
    if (!prefill_fn_) {
      auto body = [this](const std::vector<mx::array>& in) -> std::vector<mx::array> {
        auto [logits, cache] = Forward(in[0], 0, nullptr, /*last_only=*/true);
        std::vector<mx::array> out{logits};
        out.reserve(1 + 2 * cache.size());
        for (auto& kv : cache) {
          out.push_back(kv.first);
          out.push_back(kv.second);
        }
        return out;
      };
      prefill_fn_ = options_.compile ? mx::compile(body) : body;
    }
    return *prefill_fn_;
  }

  // ---- prefill and scoring ------------------------------------------------

  bool Prefill(const std::string& context) {
    auto tokens = TokenizeScoringForm(
        context, llama_vocab_eos(vocab_),
        [this](const std::string& piece) { return Tokenize(piece, /*add_special=*/false); });
    if (tokens.empty()) {
      return false;
    }
    std::vector<int> ids(tokens.begin(), tokens.end());
    mx::array in(ids.data(), {1, (int)ids.size()}, mx::int32);
    auto flat = PrefillFn()({in});
    const mx::array& logits = flat[0];
    std::vector<std::pair<mx::array, mx::array>> cache;
    cache.reserve((flat.size() - 1) / 2);
    for (size_t i = 1; i + 1 < flat.size(); i += 2) {
      cache.emplace_back(flat[i], flat[i + 1]);
    }
    // The whole point of the cache: it stays resident so Score() decodes only
    // the candidate tails. Evaluated here, on the worker, so the caller's
    // Score() never pays for it.
    // No slice here: PrefillFn asked for last_only, so `logits` is already the
    // single row this wants. Slicing again would index past the end.
    ctx_last_logits_ = mx::squeeze(logits);
    std::vector<mx::array> to_eval{*ctx_last_logits_};
    for (auto& kv : cache) {
      to_eval.push_back(kv.first);
      to_eval.push_back(kv.second);
    }
    mx::eval(to_eval);
    cache_k_ = std::move(cache);
    n_ctx_tokens_ = (int)ids.size();
    // The traced body reads cache_k_ and n_ctx_tokens_ through `this`, so a
    // graph traced against the PREVIOUS prefill would keep scoring the context
    // the user has already left. Dropping it here costs one re-trace per warm
    // -- warms are seconds apart -- and is the difference between a cache and
    // a bug that returns confident numbers about the wrong text.
    tail_fn_.reset();
    return true;
  }

  std::vector<CandidateScore> ScoreGroup(const std::vector<std::string>& candidates,
                                         ScoreTiming* timing) {
    const int n = (int)candidates.size();
    std::vector<CandidateScore> out(n);
    std::vector<std::vector<llama_token>> toks(n);
    int max_tail = 0;
    for (int i = 0; i < n; ++i) {
      toks[i] = Tokenize(candidates[i], /*add_special=*/false);
      if (toks[i].empty()) {
        out[i] = CandidateScore{-1e30f, 0};
        continue;
      }
      max_tail = std::max(max_tail, (int)toks[i].size() - 1);
    }
    if (timing) {
      timing->n_decoded = 0;
    }

    // Every candidate's FIRST token is free, scored off the context's own last
    // row -- the same identity llama.cpp's ScoreGroup uses, and the reason 44%
    // of live scorings decode nothing at all. Done on the GPU here rather than
    // over a copied-back logits row.
    const mx::array& last = *ctx_last_logits_;
    const mx::array ctx_lse = mx::logsumexp(last, -1, true);
    std::vector<int> first_ids(n, 0);
    for (int i = 0; i < n; ++i) {
      first_ids[i] = toks[i].empty() ? 0 : (int)toks[i][0];
    }
    mx::array first_idx(first_ids.data(), {n}, mx::int32);
    mx::array total = mx::subtract(mx::take(last, first_idx, 0), ctx_lse);

    if (max_tail > 0) {
      // One forward for every remaining token of every candidate, batched
      // across candidates. Tails shorter than max_tail are padded and their
      // padded positions are masked out of the sum below rather than
      // submitted separately -- a second forward would be a second dispatch
      // round, which is the cost this backend exists to avoid.
      std::vector<int> tail(n * max_tail, 0);
      std::vector<float> keep(n * max_tail, 0.0f);
      std::vector<int> targets(n * max_tail, 0);
      for (int i = 0; i < n; ++i) {
        for (int r = 0; r + 1 < (int)toks[i].size(); ++r) {
          tail[i * max_tail + r] = (int)toks[i][r];
          targets[i * max_tail + r] = (int)toks[i][r + 1];
          keep[i * max_tail + r] = 1.0f;
          if (timing) {
            timing->n_decoded += 1;
          }
        }
      }
      mx::array tail_in(tail.data(), {n, max_tail}, mx::int32);
      mx::array tgt(targets.data(), {n, max_tail, 1}, mx::int32);
      mx::array mask(keep.data(), {n, max_tail}, mx::float32);
      total = mx::add(total, CandidateTail()({tail_in, tgt, mask})[0]);
    }

    // ONE materialization, N floats to the host. llama.cpp copies whole logit
    // rows back and runs the log-softmax on the CPU; keeping it on the GPU is
    // not a shortcut, it is what the unified-memory design makes natural.
    mx::eval(total);
    const float* scores = total.data<float>();
    for (int i = 0; i < n; ++i) {
      if (toks[i].empty()) {
        continue;
      }
      out[i] = CandidateScore{scores[i], (int)toks[i].size()};
    }
    return out;
  }

  // The candidate step as one traced function, so MLX can fuse it.
  //
  // This is the mechanism the whole backend exists for. The residual cost the
  // llama.cpp path could not shed is Metal command encoding and dispatch, and
  // fusion is what reduces the number of dispatches; leaving it out would make
  // this an ordinary reimplementation and would have measured as one. In the
  // Python prototype it was worth 9-11% of the score phase.
  //
  // Built lazily and cached, keyed on nothing: mx::compile does its own
  // shape-keyed caching internally, so a context of a different length
  // re-traces on its own rather than silently reusing a graph built for
  // another shape. The KV cache is captured by reference through `this` and
  // read inside the traced body, which is what lets the prefix stay resident
  // between scorings instead of being an input to copy.
  const std::function<std::vector<mx::array>(const std::vector<mx::array>&)>& CandidateTail() {
    if (!tail_fn_) {
      auto body = [this](const std::vector<mx::array>& in) -> std::vector<mx::array> {
        const mx::array& tail_in = in[0];
        const mx::array& tgt = in[1];
        const mx::array& mask = in[2];
        auto [logits, _] = Forward(tail_in, n_ctx_tokens_, &(*cache_k_));
        mx::array lse = mx::logsumexp(logits, -1, true);
        mx::array picked = mx::squeeze(mx::take_along_axis(logits, tgt, -1), -1);
        return {mx::sum(mx::multiply(mx::subtract(picked, mx::squeeze(lse, -1)), mask), 1)};
      };
      tail_fn_ = options_.compile ? mx::compile(body) : body;
    }
    return *tail_fn_;
  }

  // ---- loading and the worker --------------------------------------------

  // ggml Q8_0 -> MLX's packed 8-bit affine, and the whole reason this function
  // exists rather than a call to mx::load_gguf.
  //
  // MLX is built here with MLX_BUILD_GGUF=OFF, because its gguf support vendors
  // a second ggml and exports 31 gguf_*/ggml_* symbols that collide with
  // llama.cpp's 1150. Linked together, whichever comes first on the link line
  // wins, and when MLX won, llama.cpp's own model loader called MLX's
  // incompatible gguf_get_key and died with SIGBUS. Removing the symbols is
  // the fix; reading the file through llama.cpp's gguf API -- already linked,
  // and the same one LlmScorer uses, so the two backends cannot disagree about
  // what is in the file -- is the consequence.
  //
  // The conversion is exact, not approximate. ggml Q8_0 stores, per block of
  // 32, one f16 scale `d` and 32 int8 quants with value = q * d and no zero
  // point. MLX's affine form is value = q * scale + bias over uint8, so
  // q_mlx = q_ggml + 128, scale = d, bias = -128 * d. Verified end to end:
  // logprobs agree with score_candidates to 0.0023 nats, which is the
  // rounding Q8_0 itself carries.
  static bool ConvertQ8_0(const uint8_t* src, int64_t rows, int64_t cols,
                          std::vector<uint32_t>* packed, std::vector<float>* scales,
                          std::vector<float>* biases) {
    constexpr int kBlock = 32;
    if (cols % kBlock != 0) {
      return false;
    }
    const int64_t groups = cols / kBlock;
    packed->assign(static_cast<size_t>(rows * cols / 4), 0u);
    scales->assign(static_cast<size_t>(rows * groups), 0.0f);
    biases->assign(static_cast<size_t>(rows * groups), 0.0f);
    for (int64_t r = 0; r < rows; ++r) {
      for (int64_t g = 0; g < groups; ++g) {
        // sizeof(block_q8_0) == 2 + 32; laid out as the f16 scale then the
        // quants. Read by offset rather than by casting to ggml's struct: this
        // translation unit is C++20 and must not depend on ggml-common.h,
        // which is internal to llama.cpp's build.
        const uint8_t* block = src + (r * groups + g) * (2 + kBlock);
        uint16_t half_bits = 0;
        std::memcpy(&half_bits, block, sizeof(half_bits));
        const float d = HalfToFloat(half_bits);
        (*scales)[static_cast<size_t>(r * groups + g)] = d;
        (*biases)[static_cast<size_t>(r * groups + g)] = -128.0f * d;
        for (int i = 0; i < kBlock; ++i) {
          const int8_t q = static_cast<int8_t>(block[2 + i]);
          const uint32_t byte = static_cast<uint32_t>(static_cast<uint8_t>(q + 128));
          const int64_t flat = r * cols + g * kBlock + i;
          (*packed)[static_cast<size_t>(flat / 4)] |= byte << (8 * (flat % 4));
        }
      }
    }
    return true;
  }

  static float HalfToFloat(uint16_t h) {
    // No <arm_neon.h> and no _Float16: this has to compile as plain C++20 in a
    // TU that already juggles two toolchains' headers.
    const uint32_t sign = static_cast<uint32_t>(h & 0x8000u) << 16;
    const uint32_t exp = (h >> 10) & 0x1Fu;
    const uint32_t mant = h & 0x3FFu;
    uint32_t bits;
    if (exp == 0) {
      if (mant == 0) {
        bits = sign;
      } else {
        int e = -1;
        uint32_t m = mant;
        do {
          ++e;
          m <<= 1;
        } while ((m & 0x400u) == 0);
        bits = sign | ((127 - 15 - e) << 23) | ((m & 0x3FFu) << 13);
      }
    } else if (exp == 0x1Fu) {
      bits = sign | 0x7F800000u | (mant << 13);
    } else {
      bits = sign | ((exp + 127 - 15) << 23) | (mant << 13);
    }
    float out;
    std::memcpy(&out, &bits, sizeof(out));
    return out;
  }

  // The model, read through llama.cpp's gguf API and converted into the arrays
  // MLX's quantized_matmul wants. See ConvertQ8_0 for why this is not
  // mx::load_gguf.
  //
  // One file, one reader, for both backends: LlmScorer opens the same path with
  // the same library, so the two cannot disagree about what is in it. That
  // matters more than it sounds -- the whole point of running them side by side
  // is that a difference in their output is a difference in the SCORER.
  bool LoadGgufWeights() {
    ggml_context* meta_ctx = nullptr;
    gguf_init_params params{};
    params.no_alloc = false;  // let ggml allocate and load the tensor data
    params.ctx = &meta_ctx;
    gguf_context* gguf = gguf_init_from_file(model_path_.c_str(), params);
    if (!gguf) {
      LOG(ERROR) << "[copilot] mlx_scorer: gguf_init_from_file failed for " << model_path_;
      return false;
    }
    struct Closer {
      gguf_context* g;
      ggml_context* c;
      ~Closer() {
        if (g) gguf_free(g);
        if (c) ggml_free(c);
      }
    } closer{gguf, meta_ctx};

    auto key_u32 = [&](const char* k, int fallback) {
      const int64_t id = gguf_find_key(gguf, k);
      return id < 0 ? fallback : static_cast<int>(gguf_get_val_u32(gguf, id));
    };
    auto key_f32 = [&](const char* k, float fallback) {
      const int64_t id = gguf_find_key(gguf, k);
      return id < 0 ? fallback : gguf_get_val_f32(gguf, id);
    };
    shape_.n_layers = key_u32("llama.block_count", 0);
    shape_.d_model = key_u32("llama.embedding_length", 0);
    shape_.n_heads = key_u32("llama.attention.head_count", 0);
    shape_.n_kv_heads = key_u32("llama.attention.head_count_kv", shape_.n_heads);
    shape_.rms_eps = key_f32("llama.attention.layer_norm_rms_epsilon", 1e-5f);
    shape_.rope_theta = key_f32("llama.rope.freq_base", 10000.0f);
    if (shape_.n_layers <= 0 || shape_.d_model <= 0 || shape_.n_heads <= 0) {
      LOG(ERROR) << "[copilot] mlx_scorer: gguf metadata is missing the model shape";
      return false;
    }
    shape_.head_dim = shape_.d_model / shape_.n_heads;
    shape_.group_size = kExpectedGroupSize;

    // Over the gguf's OWN tensor list, not over everything in the ggml
    // context: loading with no_alloc=false also creates a synthetic
    // "GGUF tensor data binary blob" of type i8 holding the raw bytes, and
    // walking the context hands you that first. It is not a model tensor and
    // rejecting it as an unsupported dtype -- which is what the type check
    // below did -- fails the load with a message about re-exporting the model.
    const int64_t n_tensors = gguf_get_n_tensors(gguf);
    for (int64_t ti = 0; ti < n_tensors; ++ti) {
      const std::string name = gguf_get_tensor_name(gguf, ti);
      ggml_tensor* t = ggml_get_tensor(meta_ctx, name.c_str());
      if (!t) {
        LOG(ERROR) << "[copilot] mlx_scorer: " << name
                   << " is in the gguf index but not in "
                      "the loaded context";
        return false;
      }
      // ggml's ne[] is [cols, rows, ...] -- the fastest-varying dimension
      // first, the opposite of the row-major [rows, cols] MLX takes. Getting
      // this backwards transposes every matrix and produces a model that runs
      // and is wrong, which is the failure mode this file already carries a
      // RoPE warning about.
      const int64_t cols = t->ne[0];
      const int64_t rows = t->ne[1] > 0 ? t->ne[1] : 1;
      if (t->type == GGML_TYPE_F32) {
        std::vector<float> v(static_cast<size_t>(ggml_nelements(t)));
        std::memcpy(v.data(), t->data, v.size() * sizeof(float));
        weights_.emplace(name, mx::array(v.data(), {static_cast<int>(v.size())}, mx::float32));
        continue;
      }
      if (t->type != GGML_TYPE_Q8_0) {
        LOG(ERROR) << "[copilot] mlx_scorer: " << name << " is " << ggml_type_name(t->type)
                   << "; this backend reads F32 and Q8_0 only. Re-export with"
                      " `rime-train export --dtype q8_0`, or use backend: llama.";
        return false;
      }
      std::vector<uint32_t> packed;
      std::vector<float> scales, biases;
      if (!ConvertQ8_0(static_cast<const uint8_t*>(t->data), rows, cols, &packed, &scales,
                       &biases)) {
        LOG(ERROR) << "[copilot] mlx_scorer: " << name << " has " << cols
                   << " columns, not a multiple of the 32-element Q8_0 block";
        return false;
      }
      const int r = static_cast<int>(rows), c = static_cast<int>(cols);
      const int g = c / kExpectedGroupSize;
      // The gguf tensor is already called `blk.0.attn_q.weight`; the packed
      // data keeps that name and the two side tensors replace the suffix.
      // Same layout mx::load_gguf produced (`output.scales`,
      // `blk.9.ffn_down.biases`), so QLinear::Resolve is unchanged and the
      // f32 norms, whose gguf names already end in `.weight`, still resolve.
      static const std::string kSuffix = ".weight";
      const std::string base =
          (name.size() > kSuffix.size() &&
           name.compare(name.size() - kSuffix.size(), kSuffix.size(), kSuffix) == 0)
              ? name.substr(0, name.size() - kSuffix.size())
              : name;
      weights_.emplace(base + ".weight", mx::array(packed.data(), {r, c / 4}, mx::uint32));
      weights_.emplace(base + ".scales",
                       mx::astype(mx::array(scales.data(), {r, g}, mx::float32), mx::float16));
      weights_.emplace(base + ".biases",
                       mx::astype(mx::array(biases.data(), {r, g}, mx::float32), mx::float16));
    }
    LOG(INFO) << "[copilot] mlx_scorer: read " << weights_.size() << " arrays from " << model_path_;
    return true;
  }

  bool EnsureLoaded() {
    if (loaded_.load(std::memory_order_acquire)) {
      return true;
    }
    if (load_failed_.load(std::memory_order_acquire)) {
      return false;
    }
    auto metallib = FindMetallib();
    if (!metallib) {
      LOG(ERROR) << "[copilot] mlx_scorer: mlx.metallib not found next to the plugin. "
                    "MLX would abort the process rather than fail, so this backend is "
                    "disabled; copy mlx.metallib beside librime-copilot.dylib, or set "
                    "copilot/rerank/llm/backend to llama.";
      load_failed_.store(true, std::memory_order_release);
      return false;
    }
    llama_model_params mp = llama_model_default_params();
    // The tokenizer and nothing else -- no weights are read, so this is a
    // vocab table rather than a second copy of the model in memory.
    mp.vocab_only = true;
    vocab_model_ = llama_model_load_from_file(model_path_.c_str(), mp);
    if (!vocab_model_) {
      LOG(ERROR) << "[copilot] mlx_scorer: failed to read the vocab from " << model_path_;
      load_failed_.store(true, std::memory_order_release);
      return false;
    }
    vocab_ = llama_model_get_vocab(vocab_model_);

    try {
      if (!LoadGgufWeights()) {
        load_failed_.store(true, std::memory_order_release);
        return false;
      }
      if (shape_.n_layers <= 0 || shape_.d_model <= 0 || shape_.n_heads <= 0) {
        LOG(ERROR) << "[copilot] mlx_scorer: gguf metadata is missing the model shape";
        load_failed_.store(true, std::memory_order_release);
        return false;
      }
      // The embedding is read as a dense table once rather than dequantized on
      // every forward: it is one row per token, and a gather is cheaper than a
      // quantized matmul against a one-hot.
      embed_ = mx::astype(
          mx::dequantize(weights_.at("token_embd.weight"), weights_.at("token_embd.scales"),
                         weights_.at("token_embd.biases"), shape_.group_size, kBits),
          options_.f16 ? mx::float16 : mx::float32);
      // float16 throughout, and this cast is what makes it so. mx::dequantize
      // already returns f16, but the RMSNorm weights are F32 in the gguf --
      // export.py keeps every 1-D tensor F32 because ggml aborts on a
      // mixed-type binary op -- and multiplying f16 activations by an f32
      // weight promotes the WHOLE forward to f32, including the KV cache.
      // llama.cpp's KV cache is F16 (llama_context_default_params' type_k and
      // type_v), so leaving this alone gives the MLX arm twice the KV traffic
      // of the arm it is being compared against -- and the prefill, the one
      // phase that writes a full context of KV, pays for it.
      const bool f16 = options_.f16;
      auto half = [f16](const mx::array& a) {
        return f16 ? mx::astype(a, mx::float16) : mx::astype(a, mx::float32);
      };
      layers_.reserve(shape_.n_layers);
      for (int i = 0; i < shape_.n_layers; ++i) {
        const std::string p = "blk." + std::to_string(i);
        layers_.push_back(Layer{half(W(p + ".attn_norm.weight")), half(W(p + ".ffn_norm.weight")),
                                Resolve(p + ".attn_q"), Resolve(p + ".attn_k"),
                                Resolve(p + ".attn_v"), Resolve(p + ".attn_output"),
                                Resolve(p + ".ffn_gate"), Resolve(p + ".ffn_up"),
                                Resolve(p + ".ffn_down")});
      }
      out_norm_ = half(W("output_norm.weight"));
      out_proj_ = Resolve("output");
      std::vector<mx::array> warm{*embed_, *out_norm_};
      for (const auto& l : layers_) {
        warm.push_back(l.attn_norm);
        warm.push_back(l.ffn_norm);
      }
      mx::eval(warm);
    } catch (const std::exception& e) {
      LOG(ERROR) << "[copilot] mlx_scorer: failed to load " << model_path_ << ": " << e.what();
      load_failed_.store(true, std::memory_order_release);
      return false;
    }
    LOG(INFO) << "[copilot] mlx_scorer: loaded " << model_path_ << " (" << shape_.n_layers
              << " layers, d=" << shape_.d_model << ", group=" << shape_.group_size
              << ", compile=" << options_.compile << ", f16=" << options_.f16 << ")";
    loaded_.store(true, std::memory_order_release);
    return true;
  }

  void WorkerLoop() {
    while (true) {
      std::string context;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return has_pending_ || shutdown_; });
        if (shutdown_) {
          return;
        }
        context = std::move(pending_context_);
        has_pending_ = false;
        active_context_ = context;
        has_active_ = true;
      }
      if (EnsureLoaded()) {
        std::lock_guard<std::mutex> lock(model_mutex_);
        if (Prefill(context)) {
          warmed_context_ = context;
        } else {
          warmed_context_.clear();
        }
      }
      bool ok = !warmed_context_.empty() && warmed_context_ == context;
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        has_active_ = false;
        active_context_.clear();
      }
      if (ok) {
        // Published after model_mutex_ is released, the same ordering
        // llm_scorer.cc uses and for the same reason.
        warm_cache_.Publish(context);
      }
    }
  }

  std::string model_path_;
  MlxScorerOptions options_;

  llama_model* vocab_model_ = nullptr;
  const llama_vocab* vocab_ = nullptr;
  std::unordered_map<std::string, mx::array> weights_;
  std::optional<mx::array> embed_;
  std::vector<Layer> layers_;
  std::optional<mx::array> out_norm_;
  std::optional<QLinear> out_proj_;
  Shape shape_;

  mutable std::mutex model_mutex_;
  std::string warmed_context_;
  std::optional<mx::array> ctx_last_logits_;
  std::optional<std::vector<std::pair<mx::array, mx::array>>> cache_k_;
  int n_ctx_tokens_ = 0;
  std::optional<std::function<std::vector<mx::array>(const std::vector<mx::array>&)>> tail_fn_;
  std::optional<std::function<std::vector<mx::array>(const std::vector<mx::array>&)>> prefill_fn_;

  WarmCache warm_cache_;
  std::atomic<bool> loaded_{false};
  std::atomic<bool> load_failed_{false};

  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  bool shutdown_ = false;
  bool has_pending_ = false;
  bool has_active_ = false;
  std::string pending_context_;
  std::string active_context_;
};

MlxScorer::MlxScorer(std::string model_path, MlxScorerOptions options)
    : impl_(std::make_unique<Impl>(std::move(model_path), options)) {}

MlxScorer::~MlxScorer() = default;

std::vector<CandidateScore> MlxScorer::Score(const std::string& context,
                                             const std::vector<std::string>& candidates,
                                             ScoreTiming* timing) {
  return impl_->Score(context, candidates, timing);
}

bool MlxScorer::IsWarm(const std::string& context) const { return impl_->IsWarm(context); }

void MlxScorer::WarmUp(const std::string& context) { impl_->WarmUp(context); }

bool MlxScorer::Loaded() const { return impl_->Loaded(); }

}  // namespace rime
