#include "llm_scorer.h"

#include <llama.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <vector>

#include <glog/logging.h>

#include "scoring_form.h"  // BuildScoringContext, TokenizeScoringForm
#include "warm_cache.h"

// This is a port of tools/score_candidates.cc (read that file first -- in
// particular Tokenize/LogProbOf and the per-candidate scoring loop) with two
// changes the plan requires: candidates are scored as a batch instead of one
// at a time, and the prefilled context survives between calls instead of
// being rebuilt per request. See llm_scorer.h for the threading model.

namespace rime {

namespace {

// Same shape as llama::Backend::Tokenize (src/llm.cc) and the PoC's own
// Tokenize (tools/score_candidates.cc:44): add_special controls whether BOS
// is inserted. Every call site here passes false -- BOS never occurs in the
// training stream (see scoring_form.h), so this is used only to tokenize the
// pieces TokenizeScoringForm splits the context into around each EOS
// carrier, and to tokenize a candidate continuing it.
std::vector<llama_token> Tokenize(const llama_vocab* vocab, const std::string& text,
                                  bool add_special) {
  int n = -llama_tokenize(vocab, text.data(), (int)text.size(), nullptr, 0, add_special, true);
  std::vector<llama_token> toks(n > 0 ? n : 0);
  if (n > 0) {
    llama_tokenize(vocab, text.data(), (int)text.size(), toks.data(), n, add_special, true);
  }
  return toks;
}

// The log-softmax's normalizing constant for `logits` (length n_vocab): a
// numerically stable log-sum-exp, via the standard max-subtraction trick.
// This is the expensive half of LogProbOf below -- two full passes over
// n_vocab -- and it does NOT depend on the target token at all, only on
// `logits` itself. Split out so a caller scoring several targets against the
// SAME logits row (ScoreGroup's free first token, shared via
// ctx_last_logits_ across every candidate in a group) can compute it once
// instead of once per candidate: about 2.8ms of the ~29ms median latency was
// this sum, recomputed group_n times for an identical result. Identical
// arithmetic to the PoC's LogProbOf (tools/score_candidates.cc:56).
double LogSumExp(const float* logits, int32_t n_vocab) {
  float max_logit = logits[0];
  for (int32_t i = 1; i < n_vocab; ++i) {
    max_logit = std::max(max_logit, logits[i]);
  }
  double sum_exp = 0.0;
  for (int32_t i = 0; i < n_vocab; ++i) {
    sum_exp += std::exp((double)(logits[i] - max_logit));
  }
  return (double)max_logit + std::log(sum_exp);
}

// log P(target) once the row's log-sum-exp is already known -- O(1), no pass
// over n_vocab at all.
inline float LogProbGivenLogSumExp(const float* logits, llama_token target, double log_sum_exp) {
  return (float)((double)logits[target] - log_sum_exp);
}

// log P(target) under the categorical distribution defined by `logits`
// (length n_vocab), via a numerically stable log-softmax. For a caller
// scoring only one target against this row -- unlike ScoreGroup's shared
// ctx_last_logits_ case, every per-token decode call below scores a row
// unique to its own (candidate, position), so there is no redundancy to
// remove there.
float LogProbOf(const float* logits, int32_t n_vocab, llama_token target) {
  return LogProbGivenLogSumExp(logits, target, LogSumExp(logits, n_vocab));
}

constexpr llama_seq_id kCtxSeq = 0;
constexpr llama_seq_id kScratchSeqBase = 1;
// Headroom above the tuned default (LlmRerankOptions::top_n == 4, see
// rerank_llm.h): sizes the context's n_seq_max so the common case never
// needs a second group. A caller asking to score more than this still gets a
// correct answer -- Score() just runs ScoreGroup() again for the remainder --
// it costs an extra sequential decode round rather than failing.
constexpr int kMaxCandidates = 8;
// kNCtx is the TOTAL physical KV cache size handed to llama_init_from_model,
// shared across kNSeqMax sequences -- it is NOT the per-sequence budget.
// llama.cpp derives the real per-sequence limit as
// n_ctx_seq = pad_to_256(kNCtx / kNSeqMax) (llama-context.cpp:293-294), e.g.
// 4096/9 -> 512 here. Prefill's too-long guard must check that derived
// value (cached as n_ctx_seq_, read back via llama_n_ctx_seq() once the
// context exists), not kNCtx itself -- a guard against kNCtx admits contexts
// ~7x longer than a scratch sequence can actually hold, which fails inside
// llama_decode() instead of degrading to "no scores".
constexpr int kNCtx = 4096;
constexpr int kNBatch = 512;
// score_candidates.cc's own comment (line 149): left at the default of 1,
// every llama_decode() against a scratch sequence fails to find a KV slot.
constexpr uint32_t kNSeqMax = kMaxCandidates + 1;
// Headroom subtracted from n_ctx_seq_ for the too-long guard: only needs to
// cover one candidate's own token count, not the sum across candidates --
// each scratch sequence has its own independent n_ctx_seq-sized position
// budget, so candidates never compete with each other for it.
constexpr int kCandidateHeadroom = 64;

constexpr float kUnscoreable = -1e30f;

}  // namespace

struct LlmScorer::Impl {
  Impl(std::string model_path, LlmScorerOptions options)
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
    // Nothing to free if the model never loaded.
    if (loaded_.load(std::memory_order_acquire)) {
      llama_batch_free(batch_);
      llama_free(ctx_);
      llama_model_free(model_);
    }
    if (backend_inited_) {
      llama_backend_free();
    }
  }

  bool Loaded() const { return loaded_.load(std::memory_order_acquire); }

  bool IsWarm(const std::string& context) const {
    return warm_cache_.Lookup(context) == WarmCache::State::kHot;
  }

  // Cheap and synchronous: claims the WarmCache slot immediately (so a caller
  // that checks IsWarm() right after sees kWarming, not a stale kHot/kCold
  // from before this call), then hands the actual model work to the worker
  // thread and returns. Never blocks on the model.
  void WarmUp(const std::string& context) {
    if (context.empty()) {
      return;
    }
    if (warm_cache_.Lookup(context) == WarmCache::State::kHot) {
      // Already warm: posting the job anyway would re-decode the same
      // context for nothing (worse, a failed re-decode would break the KV
      // cache under a context the cache still calls hot -- see Prefill).
      // Task 5 adds call sites that fire on every commit and composition
      // start, so this is the common case there, not an edge case.
      return;
    }
    std::lock_guard<std::mutex> lock(queue_mutex_);
    // Same context already queued, or actively being prefilled by the
    // worker right now: posting it again would decode it a second time for
    // nothing. Worse than wasted work -- if the duplicate is picked up
    // right after the in-flight prefill Publishes, Prefill's seq_rm at the
    // top wipes kCtxSeq's KV state and re-decodes it, and a failure in that
    // redundant pass clears warmed_context_ (see Prefill's `ok == false`
    // branch) while WarmCache still reports kHot -- Score() then finds
    // warmed_context_ mismatched and returns no scores for a context the
    // cache claims is ready. Task 5 makes this the common case, not an edge
    // case: commit-time and composition-start warming both fire for the
    // same context within milliseconds of each other, while the first job
    // may still be in flight.
    if ((has_pending_ && pending_context_ == context) ||
        (has_active_ && active_context_ == context)) {
      return;
    }
    // Claiming WarmCache's single slot and queueing the job that will fill it
    // must be one atomic step, both guarded by queue_mutex_ -- NOT BeginWarm
    // before this lock, dedupe after. A call that lands on the dedupe branch
    // above must NOT have touched the slot: with BeginWarm run unconditionally
    // up front, a call for context A that is actively in flight (has_active_)
    // could still overwrite key_ away from whatever context B a different,
    // still-queued job is FOR, then dedupe out of re-queueing A -- leaving
    // WarmCache's key_ pointed at A while the KV cache ends up holding B once
    // B's Prefill() runs (its own Publish(B) is then dropped, key_ still
    // says A). Net effect: WarmCache reports A permanently hot while Score(A)
    // finds warmed_context_ mismatched and returns nothing forever -- worse
    // than the code this replaced, which was merely wasteful, not wrong. Only
    // the call that actually queues a job gets to claim the slot.
    warm_cache_.BeginWarm(context);
    pending_context_ = context;
    has_pending_ = true;
    queue_cv_.notify_one();
  }

  std::vector<CandidateScore> Score(const std::string& context,
                                    const std::vector<std::string>& candidates,
                                    ScoreTiming* timing) {
    if (warm_cache_.Lookup(context) != WarmCache::State::kHot) {
      return {};  // nothing ran, so every ScoreTiming field stays -1
    }
    // Timed separately from the work below because the two have opposite
    // fixes and the sum of them cannot tell you which you are looking at --
    // see ScoreTiming (scorer.h). The worker holds this same mutex for the
    // whole of a background prefill, and a prefill is queued on every commit
    // and every composition start, so a Score() arriving in that window waits
    // here rather than in anything llama.cpp does.
    const auto lock_t0 = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> lock(model_mutex_);
    const auto work_t0 = std::chrono::steady_clock::now();
    if (timing) {
      timing->lock_us =
          std::chrono::duration_cast<std::chrono::microseconds>(work_t0 - lock_t0).count();
    }
    // Records what actually happened even on the paths that return early, so
    // a `work_us` of ~0 is legible as "returned without scoring" rather than
    // as an impossibly fast scoring.
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
    // Re-check under the lock: the worker can start warming a *different*
    // context between the Lookup above and this thread taking the lock, in
    // which case ctx_/ctx_last_logits_ no longer belong to `context` even
    // though WarmCache briefly still reported it hot.
    if (warmed_context_ != context) {
      return {};
    }
    const int n = static_cast<int>(candidates.size());
    std::vector<CandidateScore> scores(n);
    // Summed across groups, and set to 0 before the loop rather than left at
    // -1: from here on a decode either happened or provably did not, and 0 is
    // the finding -- the whole single-token-candidate mode this exists to
    // measure reports exactly that.
    if (timing) {
      timing->n_decoded = 0;
    }
    for (int base = 0; base < n; base += kMaxCandidates) {
      const int group_n = std::min(kMaxCandidates, n - base);
      if (!ScoreGroup(candidates, base, group_n, &scores, timing)) {
        // A mid-group decode failure would otherwise leave the affected
        // candidates' sums truncated -- divided by their FULL n_tokens, a
        // shorter sum is a LESS negative (i.e. better-looking) score, so
        // this silently promotes exactly the candidate the failure hit.
        // Returning nothing here is always safer than returning a number
        // that is wrong in a favourable direction; the fallback chain
        // already treats an empty result as "consult the db instead".
        return {};
      }
    }
    return scores;
  }

 private:
  void WorkerLoop() {
    while (true) {
      std::string context;
      {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        queue_cv_.wait(lock, [this] { return has_pending_ || shutdown_; });
        if (shutdown_) {
          // A WarmUp() racing the destructor is not worth honoring: nothing
          // will read its result after this thread exits.
          return;
        }
        context = pending_context_;
        has_pending_ = false;
        // Marks the job as in flight for WarmUp()'s dedupe check above, for
        // the whole duration of Prefill() below -- not just while it sits in
        // the single-slot queue.
        active_context_ = context;
        has_active_ = true;
      }
      Prefill(context);
      {
        std::lock_guard<std::mutex> lock(queue_mutex_);
        has_active_ = false;
      }
    }
  }

  // Loads the model at most once, on the worker thread, on the first
  // WarmUp() the feature ever receives. A failure is logged once and
  // remembered forever -- callers read Loaded() to stop asking.
  bool EnsureLoaded() {
    if (loaded_.load(std::memory_order_acquire)) {
      return true;
    }
    if (load_failed_.load(std::memory_order_acquire)) {
      return false;
    }

    // Silence llama.cpp's own logging, same as llm.cc does for the prediction
    // provider. Without this every prefill writes graph/KV-buffer chatter to
    // the IME process's stderr -- harmless but unbounded, and it grew
    // substantially between llama.cpp b7820 and b10456.
    llama_log_set([](ggml_log_level /*level*/, const char* /*text*/, void* /*user_data*/) {},
                  nullptr);
    llama_backend_init();
    backend_inited_ = true;

    llama_model_params model_params = llama_model_default_params();
    model_params.n_gpu_layers = options_.n_gpu_layers;
    model_ = llama_model_load_from_file(model_path_.c_str(), model_params);
    if (!model_) {
      LOG(ERROR) << "[copilot] llm_scorer: failed to load model '" << model_path_ << "'";
      load_failed_.store(true, std::memory_order_release);
      return false;
    }
    vocab_ = llama_model_get_vocab(model_);
    n_vocab_ = llama_vocab_n_tokens(vocab_);

    llama_context_params ctx_params = llama_context_default_params();
    ctx_params.n_ctx = kNCtx;
    ctx_params.n_batch = kNBatch;
    ctx_params.n_seq_max = kNSeqMax;
    ctx_params.no_perf = true;
    // 0 means "whatever the machine reports", which is what this always did.
    // A configured value is taken as given -- including a value above the core
    // count, which llama.cpp handles and which is the caller's business, not
    // this function's to second-guess.
    ctx_params.n_threads = options_.n_threads > 0
                               ? static_cast<int32_t>(options_.n_threads)
                               : static_cast<int32_t>(std::thread::hardware_concurrency());
    ctx_params.n_threads_batch = ctx_params.n_threads;
    LOG(INFO) << "[copilot] llm_scorer: n_gpu_layers=" << options_.n_gpu_layers
              << " n_threads=" << ctx_params.n_threads;

    ctx_ = llama_init_from_model(model_, ctx_params);
    if (!ctx_) {
      LOG(ERROR) << "[copilot] llm_scorer: failed to create context for '" << model_path_ << "'";
      llama_model_free(model_);
      model_ = nullptr;
      load_failed_.store(true, std::memory_order_release);
      return false;
    }
    mem_ = llama_get_memory(ctx_);
    batch_ = llama_batch_init(kNBatch, /*embd=*/0, /*n_seq_max=*/1);
    // The real per-sequence position budget (see kNCtx's comment) -- read
    // back from llama.cpp rather than recomputed here, so this can never
    // drift from whatever llama_init_from_model actually derived.
    n_ctx_seq_ = llama_n_ctx_seq(ctx_);

    loaded_.store(true, std::memory_order_release);
    return true;
  }

  // Runs on the worker thread. Decodes `context` once into kCtxSeq and
  // records the last-token logits every candidate's first token will be
  // scored against, exactly as the PoC does at score_candidates.cc:219-244 --
  // except this context now stays live in the KV cache for Score() calls
  // that follow, instead of being torn down at the end of the function.
  void Prefill(const std::string& context) {
    if (!EnsureLoaded()) {
      // WarmCache is left at kWarming for this key forever; that is fine --
      // it reads the same as "never got hot" to every caller, which falls
      // back to the db path.
      return;
    }

    // add_special = false: BOS never occurs in the training stream (train.py
    // writes `sentence + EOS` repeated, so token 1 is neither input nor target
    // and its embedding sits at initialization). The carrier splice is what
    // turns the aligned string's 0x02 bytes into real EOS tokens --
    // llama_tokenize's parse_special would byte-fall-back on them. See
    // src/scoring_form.h.
    std::vector<llama_token> ctx_tokens = TokenizeScoringForm(
        context, llama_vocab_eos(vocab_),
        [this](const std::string& run) { return Tokenize(vocab_, run, /*add_special=*/false); });
    // Guard against n_ctx_seq_ (the real per-sequence budget llama.cpp
    // derived), not kNCtx (the total physical cache size) -- see kNCtx's
    // comment. Getting this wrong lets an over-long context pass here and
    // fail inside llama_decode() instead.
    if (ctx_tokens.empty() || (int)ctx_tokens.size() >= (int)n_ctx_seq_ - kCandidateHeadroom) {
      LOG(ERROR) << "[copilot] llm_scorer: context too long or empty (" << ctx_tokens.size()
                 << " of " << n_ctx_seq_ << " tokens), skipping warm-up";
      return;
    }

    bool ok = false;
    const int n_ctx_tok = static_cast<int>(ctx_tokens.size());
    {
      std::lock_guard<std::mutex> lock(model_mutex_);
      llama_memory_seq_rm(mem_, kCtxSeq, -1, -1);
      for (int i = 0; i < kMaxCandidates; ++i) {
        llama_memory_seq_rm(mem_, kScratchSeqBase + i, -1, -1);
      }

      ok = true;
      for (int i = 0; i < n_ctx_tok; i += kNBatch) {
        const int chunk = std::min(kNBatch, n_ctx_tok - i);
        batch_.n_tokens = chunk;
        for (int k = 0; k < chunk; ++k) {
          batch_.token[k] = ctx_tokens[i + k];
          batch_.pos[k] = i + k;
          batch_.n_seq_id[k] = 1;
          batch_.seq_id[k][0] = kCtxSeq;
          batch_.logits[k] = (i + k == n_ctx_tok - 1) ? 1 : 0;
        }
        if (llama_decode(ctx_, batch_) != 0) {
          LOG(ERROR) << "[copilot] llm_scorer: context decode failed for warm-up";
          ok = false;
          break;
        }
      }
      if (ok) {
        float* last = llama_get_logits_ith(ctx_, -1);
        ctx_last_logits_.assign(last, last + n_vocab_);
        n_ctx_tok_ = n_ctx_tok;
        warmed_context_ = context;
      } else {
        // The seq_rm above already discarded whatever kCtxSeq held, so
        // whether this was a fresh prefill or a redundant re-prefill of the
        // same key, warmed_context_ must not go on claiming a context that
        // is no longer actually in the KV cache -- Score() gates on exactly
        // this field. Never equals a real (non-empty) context, so every
        // future Score(context) call correctly reports empty rather than
        // scoring against a broken cache.
        warmed_context_.clear();
      }
    }
    // Published after model_mutex_ is released: a racing Score() that reads
    // WarmCache as hot right after this then blocks on model_mutex_ itself
    // (uncontended, since Prefill is done with it), rather than the two
    // mutexes needing to be held together.
    if (ok) {
      warm_cache_.Publish(context);
    }
  }

  // Scores candidates[base, base+group_n) against the context already held
  // in kCtxSeq. Must run with model_mutex_ held. This is the batching the
  // plan asks for: every candidate gets its own scratch sequence branched
  // off the prefilled context, and token position r (0-indexed, the second
  // token of the candidate onward) is decoded for every still-live candidate
  // in one llama_decode() call, not one per candidate.
  //
  // Returns false if a decode failed partway through the group. `out` is
  // left unwritten for this range in that case -- the caller must discard
  // the whole Score() result rather than keep the truncated sums, which
  // read as (incorrectly) favourable scores. See the call site's comment.
  bool ScoreGroup(const std::vector<std::string>& candidates, int base, int group_n,
                  std::vector<CandidateScore>* out, ScoreTiming* timing = nullptr) {
    std::vector<std::vector<llama_token>> cand_tokens(group_n);
    // float, not double: this accumulates at most a few tokens' worth of
    // per-candidate log-probabilities (already individually rounded through
    // float), not a 151936-term sum -- unlike LogSumExp's internal sum_exp,
    // there is no catastrophic-cancellation risk here to buy precision
    // against.
    std::vector<float> logprob_sum(group_n, 0.0f);
    // Every candidate's first token is scored off the SAME row --
    // ctx_last_logits_, the context's own last-token logits -- so the
    // log-sum-exp that normalizes it is computed once here rather than once
    // per candidate inside the loop below (see LogSumExp's comment).
    const double ctx_log_sum_exp = LogSumExp(ctx_last_logits_.data(), n_vocab_);
    for (int i = 0; i < group_n; ++i) {
      cand_tokens[i] = Tokenize(vocab_, candidates[base + i], /*add_special=*/false);
      const llama_seq_id seq = kScratchSeqBase + i;
      llama_memory_seq_rm(mem_, seq, -1, -1);
      if (cand_tokens[i].empty()) {
        (*out)[base + i] = CandidateScore{kUnscoreable, 0};
        continue;
      }
      // Branch the KV cache: copy the (already-prefilled) context into this
      // candidate's own scratch sequence.
      llama_memory_seq_cp(mem_, kCtxSeq, seq, -1, -1);
      // Every candidate's first token is free -- scored off the context's
      // own last-token logits, no decode needed (score_candidates.cc:262).
      logprob_sum[i] =
          LogProbGivenLogSumExp(ctx_last_logits_.data(), cand_tokens[i][0], ctx_log_sum_exp);
    }

    // ONE decode for every remaining token of every candidate, not one per
    // token position. Teacher-forced scoring knows all the tokens up front,
    // so this is the same shape as a prefill: within a batch, a token attends
    // to earlier positions of its own sequence, so a candidate's whole tail
    // can be submitted at once.
    //
    // The per-position version batched across candidates but not across
    // positions, so a group cost `max_len - 1` sequential llama_decode calls
    // at ~1.5-2ms of launch overhead each. Measured on this model: a
    // 10-character candidate drove p99 to 13.2ms against a 10ms budget, while
    // p50 sat at 5.7ms -- the tail was launch count, not arithmetic, which is
    // why Q8_0 quantization barely moved it (12.8% -> 7.8% over 10ms).
    //
    // `owner[k]` maps a batch slot back to its candidate: logits at slot k
    // predict that candidate's NEXT token, so slot k scores token[r+1] where
    // r is the position slot k carried.
    std::vector<int> owner;
    std::vector<int> next_index;
    owner.reserve(kNBatch);
    next_index.reserve(kNBatch);
    int32_t n_tok = 0;
    for (int i = 0; i < group_n; ++i) {
      const int len = (int)cand_tokens[i].size();
      for (int r = 0; r + 1 < len; ++r) {
        if (n_tok >= kNBatch) {
          LOG(ERROR) << "[copilot] llm_scorer: candidate batch overflow at " << n_tok;
          return false;
        }
        batch_.token[n_tok] = cand_tokens[i][r];
        batch_.pos[n_tok] = n_ctx_tok_ + r;
        batch_.n_seq_id[n_tok] = 1;
        batch_.seq_id[n_tok][0] = kScratchSeqBase + i;
        batch_.logits[n_tok] = 1;
        owner.push_back(i);
        next_index.push_back(r + 1);
        ++n_tok;
      }
    }
    if (timing) {
      timing->n_decoded += n_tok;
    }
    if (n_tok > 0) {
      batch_.n_tokens = n_tok;
      if (llama_decode(ctx_, batch_) != 0) {
        LOG(ERROR) << "[copilot] llm_scorer: candidate decode failed (" << n_tok << " tokens)";
        return false;
      }
      for (int32_t k = 0; k < n_tok; ++k) {
        const int i = owner[k];
        float* logits = llama_get_logits_ith(ctx_, k);
        logprob_sum[i] += LogProbOf(logits, n_vocab_, cand_tokens[i][next_index[k]]);
      }
    }

    for (int i = 0; i < group_n; ++i) {
      if (cand_tokens[i].empty()) {
        continue;  // already filled with the kUnscoreable sentinel above
      }
      (*out)[base + i] = CandidateScore{logprob_sum[i], static_cast<int>(cand_tokens[i].size())};
    }
    return true;
  }

  std::string model_path_;
  WarmCache warm_cache_;

  std::thread worker_;
  std::mutex queue_mutex_;
  std::condition_variable queue_cv_;
  std::string pending_context_;
  bool has_pending_ = false;
  // The context the worker is currently inside Prefill() for, distinct from
  // pending_context_ (queued, not yet started) -- see WarmUp()'s dedupe check
  // and WorkerLoop.
  std::string active_context_;
  bool has_active_ = false;
  bool shutdown_ = false;

  // Guards every field below, including all use of ctx_: held by the worker
  // for the duration of Prefill()'s decode, and by Score() for the duration
  // of scoring. Score() only runs when WarmCache reports hot, i.e. the
  // worker is idle, so this is uncontended in the normal path.
  mutable std::mutex model_mutex_;
  llama_model* model_ = nullptr;
  const llama_vocab* vocab_ = nullptr;
  llama_context* ctx_ = nullptr;
  llama_memory_t mem_ = nullptr;
  llama_batch batch_{};
  int32_t n_vocab_ = 0;
  uint32_t n_ctx_seq_ = 0;  // the real per-sequence budget; see kNCtx's comment
  int n_ctx_tok_ = 0;
  std::vector<float> ctx_last_logits_;
  std::string warmed_context_;

  std::atomic<bool> loaded_{false};
  std::atomic<bool> load_failed_{false};
  bool backend_inited_ = false;
  // Read only inside EnsureLoaded(), on the worker thread, before loaded_ is
  // published -- so it needs no synchronization of its own.
  LlmScorerOptions options_;
};

LlmScorer::LlmScorer(std::string model_path, LlmScorerOptions options)
    : impl_(std::make_unique<Impl>(std::move(model_path), options)) {}

LlmScorer::~LlmScorer() = default;

std::vector<CandidateScore> LlmScorer::Score(const std::string& context,
                                             const std::vector<std::string>& candidates,
                                             ScoreTiming* timing) {
  return impl_->Score(context, candidates, timing);
}

bool LlmScorer::IsWarm(const std::string& context) const { return impl_->IsWarm(context); }

void LlmScorer::WarmUp(const std::string& context) { impl_->WarmUp(context); }

bool LlmScorer::Loaded() const { return impl_->Loaded(); }

}  // namespace rime
