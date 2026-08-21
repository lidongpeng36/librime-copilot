//
// What one re-ranking costs, on THIS machine: latency and the CPU/GPU split.
//
// This exists because the number that matters is not measurable anywhere else.
// `score_candidates` (read it first) decodes candidates one token at a time and
// measures 199 ms for this model -- 40x the deployed path. The deployed path is
// `LlmScorer` (src/llm_scorer.cc), which prefills the context once into
// sequence 0, `seq_cp`s it into a scratch sequence per candidate, and submits
// every remaining candidate token in ONE `llama_decode`. That code lives inside
// the plugin, behind a Rime engine, and could not be timed without one.
//
// So this tool is a deliberate second copy of that shape -- the batch geometry
// below must stay in step with `LlmScorer::Prefill` and `LlmScorer::ScoreGroup`
// or its numbers describe nothing. It is not a port of the plugin: no warm
// cache, no worker thread, no fallback chain, because none of those change the
// arithmetic and all of them would need the engine back.
//
// Two knobs are the reason it was written, both hard-coded in llm_scorer.cc
// when it was: n_gpu_layers = 99 and n_threads = hardware_concurrency().
//
// EVERY PHASE MUST END WHERE THE DEPLOYED PHASE ENDS, and on Metal that is
// llama_get_logits_ith(), not llama_decode(). The decode returns before the GPU
// has finished; reading the logits is what synchronizes. Both phases here end
// on a real read-back for that reason -- the prefill on its last-token logits,
// the score phase on the per-slot logits it then runs a full n_vocab
// log-softmax over, exactly as ScoreGroup does.
//
// This has been got wrong twice, in both directions, and each time the tool
// reported a confident conclusion that was an artifact of where the clock
// stopped:
//
//   * v1 split prefill from score BETWEEN the prefill's decode and its logits
//     read, charging the prefill's GPU wait to score. It reported the CPU path
//     beating the GPU on score p99 by more than 2x.
//   * v2 fixed that but ended the SCORE phase at llama_decode(), reading no
//     candidate logits at all. The candidate decode's GPU wait was deferred out
//     of the score window and into the NEXT iteration's prefill -- flattering
//     the GPU on score, penalising it on prefill, which were exactly the two
//     headline conclusions it produced. The per-token log-softmax was missing
//     from score latency and from `cpu ms/iter` in both arms as well, and it
//     submitted `len` tokens per candidate where ScoreGroup submits `len - 1`.
//
// Measured on an M4 (4 P-cores, 6 E-cores, 10 GPU cores), rime40m-q8, 32-char
// context, 4 candidates, 1000 iterations:
//
//   gpu_layers  threads   score p50/p99    prefill p50/p99   cpu ms/iter
//   99          10        2.55 / 5.47      2.76 / 5.41       1.93
//   0           4         1.89 / 4.36      2.57 / 5.45      14.79
//   0           8         1.98 / 5.32      2.64 / 5.29      29.34
//   0           2         2.03 / 4.45      4.43 / 7.56      11.97
//
// Read that as three separate findings:
//
//   * On THIS model, at this size, latency does not choose between the two.
//     CPU-only is in fact slightly faster on score (p50 1.89 ms against 2.55,
//     p99 4.36 against 5.47) and the prefill is a tie (2.57 against 2.76). Both
//     arms sit well inside the p99 < 10 ms budget the score number is about;
//     the prefill runs on LlmScorer's worker thread and never blocks a
//     keystroke. v2's "the GPU wins score p50 outright and the CPU halves the
//     prefill" was the deferred GPU wait, not a property of either path.
//   * Core time is what chooses, and it is not close: 1.93 ms/iteration on the
//     GPU against 14.79 on four CPU threads, 7.7x. That is why n_gpu_layers
//     defaults to 99 -- a power argument, not a latency one. It is also why
//     CPU-only remains a real setting rather than a fallback: it costs core
//     time and buys a fraction of a millisecond, which is the right trade only
//     on a machine with no usable GPU.
//   * --threads is inert while the layers are on the GPU (10, 4 and 2 all
//     measure score p50 2.53-2.55 / prefill 2.76 there) and decisive once they
//     are not. 8 is WORSE than 4 on a 4-P-core machine, on both latency and
//     core time, because the extra threads land on efficiency cores -- and 8 is
//     below the 10 that hardware_concurrency() would have picked.
//
// Run it on the machine in question and read the lines it prints.
//
//   bench_scorer --model <gguf> [--iters N] [--threads N] [--gpu-layers N]
//                [--context-chars N] [--candidates N]
//
#include <llama.h>

#include <sys/resource.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

// llm_scorer.cc's constants of the same names. kNBatch also sizes the batch
// arrays below (llama_batch_init), so every write into them must be bounded by
// it -- production has that guard on the candidate loop and chunks its prefill;
// here `--context-chars` and `--candidates` are user input and would otherwise
// walk straight off the end.
constexpr int kNBatch = 512;
constexpr int kMaxCandidates = 8;
constexpr int kNCtx = 4096;
constexpr int kNSeqMax = kMaxCandidates + 1;
// Same headroom llm_scorer.cc's Prefill keeps below the per-sequence budget for
// the candidate tokens that follow the context.
constexpr int kCandidateHeadroom = 64;

double NowMs() {
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// User+system CPU time for the whole process. The difference between this and
// wall time is the whole point of the GPU/CPU comparison: the GPU path waits
// (low CPU, work happening elsewhere and drawing power elsewhere), the CPU path
// spins every thread (high CPU, no GPU at all).
double CpuMs() {
  struct rusage usage;
  getrusage(RUSAGE_SELF, &usage);
  return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}

// Same shape as LlmScorer's own Tokenize (src/llm_scorer.cc): add_special
// controls whether BOS is inserted -- true for the context, false for a
// candidate continuing it.
std::vector<llama_token> Tokenize(const llama_vocab* vocab, const std::string& text,
                                  bool add_special) {
  int n = -llama_tokenize(vocab, text.data(), (int)text.size(), nullptr, 0, add_special, true);
  std::vector<llama_token> tokens(n > 0 ? n : 0);
  if (n > 0) {
    llama_tokenize(vocab, text.data(), (int)text.size(), tokens.data(), n, add_special, true);
  }
  return tokens;
}

// The log-softmax normalizer, computed once per context row exactly as
// LlmScorer::ScoreGroup does -- it is two full passes over n_vocab and does not
// depend on the target token, so hoisting it out of the candidate loop is worth
// real milliseconds and must be mirrored here or the CPU line is inflated.
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

// log P(target) for one decoded row, exactly as LlmScorer::LogProbOf does it.
// This is a FULL log-softmax per scored token and it is not optional here: it
// is what the deployed path pays per candidate token, and -- on Metal -- the
// llama_get_logits_ith() that feeds it is also what synchronizes the GPU. A
// bench that skips it defers the candidate decode's GPU wait into the next
// iteration's prefill and drops the softmax out of both the score latency and
// the CPU line.
float LogProbOf(const float* logits, int32_t n_vocab, llama_token target) {
  return (float)((double)logits[target] - LogSumExp(logits, n_vocab));
}

double Percentile(const std::vector<double>& sorted, double q) {
  if (sorted.empty()) {
    return 0.0;
  }
  size_t i = (size_t)(sorted.size() * q);
  return sorted[std::min(i, sorted.size() - 1)];
}

// Han text to build a context of the requested length from. Content is
// irrelevant to latency -- token count is what the prefill pays for -- but real
// characters keep the tokenizer on the path it takes in production (one token
// per character, no byte fallback).
const char* kFiller =
    "这个功能的能耗到底有多大我们先把它测出来然后再决定要不要在电池上继续开着"
    "输入法的候选窗口本来就在渲染所以显卡在这些时刻已经是醒着的并不需要额外唤醒";

std::string BuildContext(int chars) {
  std::string out;
  const std::string filler = kFiller;
  // UTF-8, 3 bytes per Han character here; walk codepoints rather than bytes.
  while ((int)out.size() / 3 < chars) {
    out += filler;
  }
  // Trim to exactly `chars` characters, on a codepoint boundary.
  size_t end = 0;
  for (int i = 0; i < chars && end < out.size(); ++i) {
    end += 3;
  }
  return out.substr(0, end);
}

// Candidates the length the re-ranker actually sees: `top_n` is 4 and the
// window is words, not sentences, so 2-3 characters each.
std::vector<std::string> BuildCandidates(int n) {
  static const char* kPool[] = {"务必", "无比", "五笔", "舞弊", "无臂", "吴璧", "无笔", "芜鄙"};
  std::vector<std::string> out;
  for (int i = 0; i < n; ++i) {
    out.push_back(kPool[i % (int)(sizeof(kPool) / sizeof(kPool[0]))]);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path;
  int iters = 500;
  int n_threads = (int)std::thread::hardware_concurrency();
  int n_gpu_layers = 99;
  int context_chars = 32;  // copilot/rerank/llm/context_chars
  int n_candidates = 4;    // copilot/rerank/llm/top_n
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "--model") && i + 1 < argc) {
      model_path = argv[++i];
    } else if (!strcmp(argv[i], "--iters") && i + 1 < argc) {
      iters = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--threads") && i + 1 < argc) {
      n_threads = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--gpu-layers") && i + 1 < argc) {
      n_gpu_layers = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--context-chars") && i + 1 < argc) {
      context_chars = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--candidates") && i + 1 < argc) {
      n_candidates = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: %s --model <gguf> [--iters N] [--threads N]\n", argv[0]);
      fprintf(stderr, "       [--gpu-layers N] [--context-chars N] [--candidates N]\n");
      return 2;
    }
  }
  if (model_path.empty()) {
    fprintf(stderr, "--model is required\n");
    return 2;
  }
  // kMaxCandidates in llm_scorer.cc: above this the plugin runs a second
  // sequential group, which this tool does not model.
  if (n_candidates < 1 || n_candidates > kMaxCandidates) {
    fprintf(stderr, "--candidates must be 1..%d (kMaxCandidates in llm_scorer.cc)\n",
            kMaxCandidates);
    return 2;
  }
  if (iters < 1) {
    fprintf(stderr, "--iters must be >= 1\n");
    return 2;
  }
  if (context_chars < 1) {
    fprintf(stderr, "--context-chars must be >= 1\n");
    return 2;
  }

  // Same as the plugin: llama.cpp's own chatter is not part of the measurement.
  llama_log_set([](ggml_log_level, const char*, void*) {}, nullptr);
  llama_backend_init();

  llama_model_params model_params = llama_model_default_params();
  model_params.n_gpu_layers = n_gpu_layers;
  llama_model* model = llama_model_load_from_file(model_path.c_str(), model_params);
  if (!model) {
    fprintf(stderr, "failed to load %s\n", model_path.c_str());
    llama_backend_free();
    return 1;
  }
  const llama_vocab* vocab = llama_model_get_vocab(model);
  const int32_t n_vocab = llama_vocab_n_tokens(vocab);

  // Every one of these mirrors llm_scorer.cc's constants; see kNCtx's comment
  // there for why n_seq_max is candidates+1 and why n_ctx is the TOTAL cache.
  llama_context_params ctx_params = llama_context_default_params();
  ctx_params.n_ctx = kNCtx;
  ctx_params.n_batch = kNBatch;
  ctx_params.n_seq_max = kNSeqMax;
  ctx_params.no_perf = true;
  ctx_params.n_threads = n_threads;
  ctx_params.n_threads_batch = n_threads;
  llama_context* ctx = llama_init_from_model(model, ctx_params);
  if (!ctx) {
    fprintf(stderr, "failed to create context\n");
    llama_model_free(model);
    llama_backend_free();
    return 1;
  }
  llama_memory_t mem = llama_get_memory(ctx);
  llama_batch batch = llama_batch_init(kNBatch, 0, kNSeqMax);

  const std::string context = BuildContext(context_chars);
  const std::vector<std::string> candidates = BuildCandidates(n_candidates);

  // Reject an over-long context here rather than overrunning the batch arrays
  // inside the loop. Production chunks its prefill across kNBatch-sized batches
  // and guards the total against the derived per-sequence budget
  // (llama_n_ctx_seq, NOT kNCtx -- see llm_scorer.cc's kNCtx comment); this
  // tool submits the context in one batch, so kNBatch binds first.
  {
    const std::vector<llama_token> probe = Tokenize(vocab, context, /*add_special=*/true);
    const int n_ctx_seq = (int)llama_n_ctx_seq(ctx);
    const int limit = std::min(kNBatch, n_ctx_seq - kCandidateHeadroom);
    if (probe.empty() || (int)probe.size() > limit) {
      fprintf(stderr, "--context-chars %d tokenizes to %d tokens; must be 1..%d\n", context_chars,
              (int)probe.size(), limit);
      llama_batch_free(batch);
      llama_free(ctx);
      llama_model_free(model);
      llama_backend_free();
      return 2;
    }
  }

  std::vector<double> whole, prefill_ms, score_ms;
  whole.reserve(iters);
  prefill_ms.reserve(iters);
  score_ms.reserve(iters);
  std::vector<float> ctx_logits((size_t)n_vocab);
  // See the score phase: this is what keeps the log-softmax alive.
  double score_checksum = 0.0;

  const double cpu_start = CpuMs();
  const double wall_start = NowMs();
  for (int it = 0; it < iters; ++it) {
    const double t_begin = NowMs();

    // --- Prefill: what WarmUp() posts to the worker thread. Repeated every
    // iteration on purpose. In production it is amortized across the segments
    // sharing a context, but it is also re-run on every commit and every
    // composition start (rerank_filter.cc's warming call sites), so roughly one
    // prefill per one score is the honest live ratio, not zero.
    std::vector<llama_token> ctx_tokens = Tokenize(vocab, context, /*add_special=*/true);
    llama_memory_seq_rm(mem, 0, -1, -1);
    for (int s = 1; s <= n_candidates; ++s) {
      llama_memory_seq_rm(mem, s, -1, -1);
    }
    batch.n_tokens = (int)ctx_tokens.size();
    for (int k = 0; k < batch.n_tokens; ++k) {
      batch.token[k] = ctx_tokens[k];
      batch.pos[k] = k;
      batch.n_seq_id[k] = 1;
      batch.seq_id[k][0] = 0;
      batch.logits[k] = (k == batch.n_tokens - 1) ? 1 : 0;
    }
    if (llama_decode(ctx, batch) != 0) {
      fprintf(stderr, "prefill decode failed\n");
      return 1;
    }
    const float* last = llama_get_logits_ith(ctx, -1);
    ctx_logits.assign(last, last + n_vocab);
    const double t_prefilled = NowMs();

    // --- ScoreGroup: what Score() runs on the caller's thread. THIS is the
    // number the p99 < 10 ms budget is about; the prefill above already
    // happened in the background by the time a real segment gets here.
    const double ctx_log_sum_exp = LogSumExp(ctx_logits.data(), n_vocab);
    std::vector<std::vector<llama_token>> cand_tokens(candidates.size());
    std::vector<float> logprob_sum(candidates.size(), 0.0f);
    // owner[k] maps a batch slot back to its candidate, next_index[k] to the
    // token that slot's logits predict -- the same two arrays ScoreGroup keeps,
    // and the reason the read-back loop below can stay a single pass.
    std::vector<int> owner;
    std::vector<int> next_index;
    owner.reserve(kNBatch);
    next_index.reserve(kNBatch);
    batch.n_tokens = 0;
    for (size_t i = 0; i < candidates.size(); ++i) {
      cand_tokens[i] = Tokenize(vocab, candidates[i], /*add_special=*/false);
      const llama_seq_id seq = 1 + (llama_seq_id)i;
      llama_memory_seq_rm(mem, seq, -1, -1);
      if (cand_tokens[i].empty()) {
        continue;
      }
      llama_memory_seq_cp(mem, 0, seq, -1, -1);
      // The first token is free: scored off the context's own last-token
      // logits, which ctx_log_sum_exp already normalizes. No decode for it.
      logprob_sum[i] = (float)((double)ctx_logits[cand_tokens[i][0]] - ctx_log_sum_exp);
      // Every remaining token of every candidate goes into the SAME batch.
      // `r + 1 < len`, not `r < len`: the LAST token of a candidate predicts
      // nothing that is being scored, so ScoreGroup never submits it. Sending
      // it anyway would decode len tokens where production decodes len - 1.
      const int len = (int)cand_tokens[i].size();
      for (int r = 0; r + 1 < len; ++r) {
        if (batch.n_tokens >= kNBatch) {
          fprintf(stderr, "candidate batch overflow at %d (kNBatch %d)\n", batch.n_tokens, kNBatch);
          return 1;
        }
        const int k = batch.n_tokens++;
        batch.token[k] = cand_tokens[i][r];
        batch.pos[k] = (int)ctx_tokens.size() + r;
        batch.n_seq_id[k] = 1;
        batch.seq_id[k][0] = seq;
        batch.logits[k] = 1;
        owner.push_back((int)i);
        next_index.push_back(r + 1);
      }
    }
    if (batch.n_tokens > 0) {
      if (llama_decode(ctx, batch) != 0) {
        fprintf(stderr, "candidate decode failed\n");
        return 1;
      }
      // The half the first version of this tool left out. Both halves matter:
      // llama_get_logits_ith() is the Metal synchronization point, and
      // LogProbOf is a full n_vocab log-softmax per scored token.
      for (int k = 0; k < batch.n_tokens; ++k) {
        const int i = owner[k];
        const float* logits = llama_get_logits_ith(ctx, k);
        logprob_sum[i] += LogProbOf(logits, n_vocab, cand_tokens[i][next_index[k]]);
      }
    }
    const double t_end = NowMs();
    // Consumed below, printed at the end: without a live reader the compiler is
    // free to delete the log-softmax the score phase exists to measure.
    for (size_t i = 0; i < candidates.size(); ++i) {
      score_checksum += logprob_sum[i];
    }

    prefill_ms.push_back(t_prefilled - t_begin);
    score_ms.push_back(t_end - t_prefilled);
    whole.push_back(t_end - t_begin);
  }
  const double wall_total = NowMs() - wall_start;
  const double cpu_total = CpuMs() - cpu_start;

  std::sort(whole.begin(), whole.end());
  std::sort(prefill_ms.begin(), prefill_ms.end());
  std::sort(score_ms.begin(), score_ms.end());

  printf("model %s  iters %d  threads %d  gpu_layers %d  context %d chars  candidates %d\n",
         model_path.c_str(), iters, n_threads, n_gpu_layers, context_chars, n_candidates);
  printf("score   (the p99<10ms budget): p50 %.2f ms  p99 %.2f ms\n", Percentile(score_ms, 0.50),
         Percentile(score_ms, 0.99));
  printf("prefill (background warm-up):  p50 %.2f ms  p99 %.2f ms\n", Percentile(prefill_ms, 0.50),
         Percentile(prefill_ms, 0.99));
  printf("both together:                 p50 %.2f ms  p99 %.2f ms\n", Percentile(whole, 0.50),
         Percentile(whole, 0.99));
  // cpu/wall well below 1 means the work is on the GPU and this line is NOT the
  // energy story -- pair it with `sudo powermetrics --samplers cpu_power,gpu_power`
  // while this runs. Above 1 it is the whole energy story: that many core-
  // milliseconds per scoring, on the CPU, and no GPU involved at all.
  printf("cpu time: %.2f ms per iteration  (cpu/wall %.2f)\n", cpu_total / iters,
         cpu_total / wall_total);
  // Not a result -- the mean log-probability the scoring produced, printed so
  // the arithmetic above has a reader and cannot be optimized away. Constant
  // across runs of the same model and context; a change means the score phase
  // stopped mirroring ScoreGroup.
  printf("mean candidate logprob: %.4f  (checksum, not a measurement)\n",
         score_checksum / ((double)iters * (double)n_candidates));

  llama_batch_free(batch);
  llama_free(ctx);
  llama_model_free(model);
  llama_backend_free();
  return 0;
}
