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
//                [--context-chars N] [--candidates N] [--candidate-chars N]
//                [--idle-ms N] [--idle-spin] [--pre-spin-us N]
//                [--pre-spin-threads N] [--json] [--qos]
//
// THE TABLE ABOVE MEASURES ONE MODE UNDER ONE CONDITION, and until 2026-09-04
// nothing said so. Live telemetry then put the deployed scorer at p50 11.1 ms
// where this tool reports 2.11 ms for what was believed to be the same work on
// the same machine. Two properties of the loop below are why, and both are now
// knobs:
//
//   * `--candidate-chars` (default 2, the pool this always used). Scoring cost
//     is bimodal on ONE thing -- whether the batch reaches llama_decode at all.
//     Every candidate's first token is scored off the prefill's own last
//     logits, so a window whose candidates are all single tokens submits
//     nothing and costs ~0.18 ms live, against ~11 ms for one that decodes.
//     44% of real scorings are in the cheap mode. This tool could not produce
//     it, so every number it has ever printed describes the expensive half.
//     Read `tokens decoded/iter` in the output: THAT, not the character count,
//     is which mode was measured -- a real two-character word is often one
//     token, and then nothing decodes.
//
//   * `--idle-ms` (default 0). This loop runs iterations back to back, which
//     is the one condition under which a cold-GPU or graph-rebuild cost cannot
//     show up. Production scores once per keystroke with human-scale gaps. The
//     sleep goes BETWEEN the prefill and the score, not between iterations,
//     because that is where the deployed gap is: LlmScorer's worker prefills
//     when a commit or composition start queues a warm, and Score() runs on a
//     later keystroke. So the score phase -- the number under study -- is the
//     first GPU work after the idle, exactly as it is live. An earlier draft
//     of this that slept between iterations would have charged the wake-up to
//     the prefill and reported the score as unaffected.
//
// WHAT `--idle-ms` TURNED OUT TO BE MEASURING: the CPU clock, and it is most
// of the deployed latency. Swept on an M4 Pro, 64-char context, 60 iterations
// per point:
//
//   idle before the score    0 ms   20 ms   50 ms   100 ms   250 ms
//   score p50                2.30    8.19    9.91    10.20    10.28
//   prefill p50              2.93    6.48    9.37     9.49     9.59
//
// The decay completes within 20-50 ms and saturates there. Real typing gaps
// are 150 ms and up, so PRODUCTION IS ALWAYS IN THE SATURATED COLUMN.
//
// Matching that against live needs the geometry live actually runs, and it is
// not the table above. The dylib in Squirrel.app predates the kNCtx 4096 ->
// 2304 change, so every telemetry line was written at n_ctx_seq 512. Re-run
// there (--n-ctx 4096 --n-seq-max 9): hot 2.17/2.36 ms, after a 100 ms idle
// 12.54/12.61 ms, against a live v7 `work_us` p50 of 10.3 ms -- between the
// two and near the cold end, which is right, because Apply() runs per live
// segment on every keystroke and a multi-segment composition scores several
// times back to back. An earlier draft compared that live 10.3 against the
// 10.20 in the 256 table and called it agreement to a tenth of a millisecond;
// that was two different geometries agreeing by coincidence. Check which
// commit the running dylib contains before pairing any live number with a
// bench one -- mtime cannot answer it.
//
// That settles the open question this tool was accused of: "production is ~5x
// bench_scorer for the same work" was true and had two suspects, the
// model_mutex_ wait and the batch geometry. Telemetry v7 killed the first
// (`lock_us` 0 across 97 scorings, max 0) and this kills the second. There is
// no gap left to explain: the tool's old figures were the hot column, and
// nothing before --idle-ms could print the other one.
//
// THREE MITIGATIONS WERE MEASURED AND ALL THREE FAILED. Recorded because the
// idea is the obvious one and will otherwise be had again:
//
//   * `--qos` (QOS_CLASS_USER_INTERACTIVE) does NOTHING: 10.02 against 10.01.
//     So this is frequency, not core placement -- a scheduler hint cannot buy
//     it, only actual work can.
//   * `--idle-spin` burns the whole gap and recovers half of it (score p50
//     5.01/5.05, p99 11.6 -> 5.5) at 101.8 ms of CPU per iteration, i.e. a
//     permanently occupied core. It is the DISCRIMINATOR that proves the cost
//     is the clock, and it is not a mode anyone should ship or benchmark in.
//   * `--pre-spin-us` / `--pre-spin-threads` are the shippable form of the
//     same idea: burst briefly right before the scoring, triggered live by the
//     keystroke itself (Copilot::ProcessKeyEvent runs ahead of the filter's
//     Apply). Measured at --idle-ms 100, two repeats per point:
//
//       pre-spin   score p50       keystroke p50    cpu ms/iter
//       0 us       10.26 / 10.13   10.26 / 10.13     9.2
//       1000 us    10.11 / 10.17   11.11 / 11.17    10.3
//       5000 us     9.91 /  9.26   14.91 / 14.26    13.5
//       10000 us    8.93 /  9.20   18.93 / 19.20    17.3
//
//     `keystroke p50` is the spin plus the score -- what a user would actually
//     pay, since the burst is added latency and not a substitute for the work.
//     It rises monotonically. There is no crossover, and there cannot be one:
//     the TOTAL recoverable is 5.3 ms (10.26 down to the 5.0 that --idle-spin
//     reaches), so any burst longer than 5.3 ms loses by arithmetic alone --
//     and at 5 ms the measured recovery is 0.5 ms. Loading more cores does not
//     change it either (8 threads for 5 ms: score 9.68/9.47 against 10.09/9.70
//     at one thread, inside the 0.3 ms round-to-round spread, for 2.1x the CPU).
//     The ramp needs tens of milliseconds of load whatever the core count, and
//     a keystroke offers microseconds of warning.
//
// What is left, therefore, is not a scheduling trick but less CPU-side work
// per scoring: the penalty is paid on command encoding and dispatch, so it
// scales with how much of that there is. That is also the most likely
// mechanism behind the MLX backend's measured -48.7% -- which was measured at
// exactly this idle condition, and so is a comparison of two downclocked runs
// rather than an artifact of one.
//
// `--json` prints one machine-readable object instead of the prose, which is
// what tools/bench_matrix.py consumes to make before/after comparisons that
// survive a llama.cpp bump or a constant change. Prefer it to eyeballing two
// runs: the whole reason that Python driver is committed rather than ad-hoc is
// the same reason compare_rerank.py is -- a measurement nobody can re-run
// cannot be re-examined when the next one contradicts it.
//
#include <llama.h>

#include <sys/resource.h>
#ifdef __APPLE__
#include <pthread/qos.h>
#endif

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

// Written by --idle-spin's busy-wait and by nothing else; volatile so the
// compiler cannot delete the loop whose whole purpose is to keep a core busy.
volatile long long spin_sink = 0;

// llm_scorer.cc's constants of the same names. kNBatch also sizes the batch
// arrays below (llama_batch_init), so every write into them must be bounded by
// it -- production has that guard on the candidate loop and chunks its prefill;
// here `--context-chars` and `--candidates` are user input and would otherwise
// walk straight off the end.
// Defaults, no longer hard limits: --n-batch, --n-ctx, --n-seq-max and
// --n-ubatch override each of these. They exist as knobs because the cost this
// tool measures turned out to be dominated by the FIXED price of issuing one
// llama_decode -- 1.57 ms hot, 10.20 ms after an idle, against 0.13/0.54 ms
// per decoded token -- and every one of these constants sizes per-call CPU
// work (graph build, KV bookkeeping across n_seq_max sequences, compute
// buffers) rather than arithmetic. Whether shrinking them moves that fixed
// price is the question; they are deliberately NOT changed here, only made
// measurable.
constexpr int kNBatch = 512;
constexpr int kMaxCandidates = 8;
constexpr int kNCtx = 2304;  // llm_scorer.cc's, and must stay equal to it
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
// window is words, not sentences, so 1-4 characters each.
//
// REAL words at every length, not sliced filler, and that is the point rather
// than decoration. What decides whether the score phase decodes is the TOKEN
// count, and a real word is frequently one token where an arbitrary run of the
// same characters is never fewer than one per character. Slicing kFiller would
// have produced candidates that always decode, i.e. the exact mode this tool
// was already stuck in.
//
// The two-character pool is unchanged and is still the default, so every
// number in the table at the top of this file remains reproducible.
std::vector<std::string> BuildCandidates(int n, int chars) {
  static const char* kPool1[] = {"的", "一", "是", "不", "了", "人", "我", "在"};
  static const char* kPool2[] = {"务必", "无比", "五笔", "舞弊", "无臂", "吴璧", "无笔", "芜鄙"};
  static const char* kPool3[] = {"计算机", "输入法", "候选词", "显示器",
                                 "处理器", "存储器", "编译器", "解释器"};
  static const char* kPool4[] = {"人工智能", "机器学习", "自然语言", "输入方法",
                                 "候选窗口", "神经网络", "深度学习", "卷积网络"};
  const char* const* pool = kPool2;
  switch (chars) {
    case 1:
      pool = kPool1;
      break;
    case 2:
      pool = kPool2;
      break;
    case 3:
      pool = kPool3;
      break;
    case 4:
      pool = kPool4;
      break;
    default:
      pool = kPool2;
      break;
  }
  std::vector<std::string> out;
  for (int i = 0; i < n; ++i) {
    out.push_back(pool[i % 8]);
  }
  return out;
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path;
  int iters = 500;
  int n_threads = (int)std::thread::hardware_concurrency();
  int n_gpu_layers = 99;
  int context_chars = 32;   // copilot/rerank/llm/context_chars
  int n_candidates = 4;     // copilot/rerank/llm/top_n
  int candidate_chars = 2;  // characters per candidate; see BuildCandidates
  int idle_ms = 0;          // idle inserted between prefill and score
  bool json = false;
  bool qos = false;            // raise this thread's QoS class (Apple only)
  bool idle_spin = false;      // burn the gap instead of sleeping through it
  int pre_spin_us = 0;         // busy-wait this long AFTER the idle, before scoring
  int pre_spin_threads = 1;    // how many cores the pre-spin loads
  int n_ctx = kNCtx;           // llm_scorer.cc's kNCtx
  int n_seq_max = kNSeqMax;    // llm_scorer.cc's kNSeqMax
  int n_batch = kNBatch;       // llm_scorer.cc's kNBatch
  int n_ubatch = 0;            // 0 = leave llama.cpp's default (512) alone
  int scores_per_prefill = 1;  // see the loop; 1 preserves the old behaviour
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
    } else if (!strcmp(argv[i], "--candidate-chars") && i + 1 < argc) {
      candidate_chars = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--idle-ms") && i + 1 < argc) {
      idle_ms = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--json")) {
      json = true;
    } else if (!strcmp(argv[i], "--qos")) {
      qos = true;
    } else if (!strcmp(argv[i], "--idle-spin")) {
      idle_spin = true;
    } else if (!strcmp(argv[i], "--pre-spin-us") && i + 1 < argc) {
      pre_spin_us = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--pre-spin-threads") && i + 1 < argc) {
      pre_spin_threads = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--n-ctx") && i + 1 < argc) {
      n_ctx = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--n-seq-max") && i + 1 < argc) {
      n_seq_max = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--n-batch") && i + 1 < argc) {
      n_batch = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--n-ubatch") && i + 1 < argc) {
      n_ubatch = atoi(argv[++i]);
    } else if (!strcmp(argv[i], "--scores-per-prefill") && i + 1 < argc) {
      scores_per_prefill = atoi(argv[++i]);
    } else {
      fprintf(stderr, "usage: %s --model <gguf> [--iters N] [--threads N]\n", argv[0]);
      fprintf(stderr,
              "       [--gpu-layers N] [--context-chars N] [--candidates N]\n"
              "       [--candidate-chars 1..4] [--idle-ms N] [--idle-spin] [--pre-spin-us N]\n"
              "       [--json] [--qos]\n"
              "       [--n-ctx N] [--n-seq-max N] [--n-batch N] [--n-ubatch N]\n"
              "       [--scores-per-prefill N]\n");
      return 2;
    }
  }
  if (model_path.empty()) {
    fprintf(stderr, "--model is required\n");
    return 2;
  }
  // kMaxCandidates in llm_scorer.cc: above this the plugin runs a second
  // sequential group, which this tool does not model.
  if (candidate_chars < 1 || candidate_chars > 4) {
    fprintf(stderr, "--candidate-chars must be 1..4 (BuildCandidates has a pool per length)\n");
    return 1;
  }
  if (idle_ms < 0) {
    fprintf(stderr, "--idle-ms must not be negative\n");
    return 1;
  }
  if (n_ctx < 256 || n_seq_max < 1 || n_batch < 1 || n_ubatch < 0) {
    fprintf(stderr, "--n-ctx must be >= 256, --n-seq-max/--n-batch >= 1, --n-ubatch >= 0\n");
    return 1;
  }
  if (n_seq_max < n_candidates + 1) {
    // kCtxSeq plus one scratch sequence per candidate. Below that, every
    // candidate decode fails to find a KV slot -- score_candidates.cc hit
    // exactly this with the default of 1.
    fprintf(stderr, "--n-seq-max must be at least --candidates + 1 (%d)\n", n_candidates + 1);
    return 1;
  }
  if (scores_per_prefill < 1) {
    fprintf(stderr, "--scores-per-prefill must be at least 1\n");
    return 1;
  }
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
  // Why this flag exists. Everything in the table at the top of this file was
  // measured with --idle-ms 0, and an idle of as little as 50 ms costs ~6x on
  // BOTH arms -- 2.12 -> 12.50 ms of score latency with the layers on the GPU,
  // 1.63 -> 13.41 without, and process CPU time rises with it (1.75 -> 11.21,
  // 13.34 -> 58.34 ms/iteration). A cost that lands on the CPU arm too is not
  // Metal's, and one that ignores --threads (1, 2, 4 and 12 all measure ~12.4
  // ms after an idle) is not the thread pool's. What is left is the scheduler:
  // a thread that has just slept is demoted, and the work runs on an
  // efficiency core at a low clock until the ramp catches up.
  //
  // That is not an artifact of this tool. It is the deployed condition -- an
  // IME scores once per keystroke, with human-scale gaps -- and it is why live
  // telemetry reads p50 11.1 ms where this file's table says 2.11.
  //
  // So: does asking for a high QoS class buy it back? Apple only, and guarded,
  // because tools/ builds on Linux CI too.
  if (qos) {
#ifdef __APPLE__
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
    fprintf(stderr, "--qos is Apple-only; ignored\n");
#endif
  }
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
  ctx_params.n_ctx = n_ctx;
  ctx_params.n_batch = n_batch;
  ctx_params.n_seq_max = n_seq_max;
  // Left untouched at 0: llama.cpp's own default is 512, and setting it
  // explicitly to that is not the same as not setting it if a future version
  // changes the default. The knob is here to try SMALLER.
  if (n_ubatch > 0) {
    ctx_params.n_ubatch = n_ubatch;
  }
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
  llama_batch batch = llama_batch_init(n_batch, 0, n_seq_max);

  const std::string context = BuildContext(context_chars);
  const std::vector<std::string> candidates = BuildCandidates(n_candidates, candidate_chars);

  // Reject an over-long context here rather than overrunning the batch arrays
  // inside the loop. Production chunks its prefill across kNBatch-sized batches
  // and guards the total against the derived per-sequence budget
  // (llama_n_ctx_seq, NOT kNCtx -- see llm_scorer.cc's kNCtx comment); this
  // tool submits the context in one batch, so BOTH bounds apply and the min
  // below is what enforces them. With these constants it is the per-sequence
  // budget that binds, not kNBatch: n_ctx_seq is pad_to_256(2304/9) = 256, so
  // the limit is 256 - 64 = 192 against kNBatch's 512. Change kNCtx, kNSeqMax
  // or kCandidateHeadroom and the other term can take over -- which is why the
  // guard is a min and not a single comparison.
  //
  // 192 rather than the 448 this said before 2026-09-04, and that is a real
  // narrowing of what this TOOL will accept: --context-chars much above ~190
  // now refuses instead of measuring. It is not a narrowing of the deployed
  // path, whose context is clamped to kMaxSurroundingPrefixChars = 64
  // (src/surrounding_source.h) -- three times under the bound. The knobs
  // --n-ctx and --n-seq-max exist to explore past it.
  {
    // No BOS: it never appears in the training stream (train.py writes
    // `sentence + EOS` repeated), and the deployed path dropped it on the
    // 2026-08-23 branch (src/scoring_form.h). Deliberately NOT run through
    // AlignToTrainingForm either -- `context` here is synthetic, built to a
    // known character count for timing, and alignment would change the token
    // count this probe is deliberately controlling.
    const std::vector<llama_token> probe = Tokenize(vocab, context, /*add_special=*/false);
    const int n_ctx_seq = (int)llama_n_ctx_seq(ctx);
    const int limit = std::min(n_batch, n_ctx_seq - kCandidateHeadroom);
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
  double slept_ms = 0.0;
  // Summed across iterations, reported as a per-iteration mean. This is the
  // `n_decoded` the deployed path now records per scoring (telemetry v7), and
  // it is what says WHICH of the two cost modes a run measured -- a
  // character count does not, because a real multi-character word is often a
  // single token and then nothing is submitted at all.
  long long decoded_tokens = 0;
  // The score phase split by whether a prefill immediately preceded it. Only
  // consecutive scorings can hit llama.cpp's single-slot graph reuse, so the
  // gap between these two is what that reuse is worth -- the one number this
  // tool could not produce while the ratio was fixed at 1:1.
  std::vector<double> score_first_ms;
  std::vector<double> score_next_ms;
  // --pre-spin-us: what the spin actually cost, and what a keystroke would
  // therefore pay end to end. Kept as two vectors rather than one sum because
  // the question has two halves and they move in opposite directions -- the
  // spin is pure added latency, the score is what the ramp buys back, and only
  // `keystroke_ms` says whether the trade is positive.
  std::vector<double> spin_ms;
  std::vector<double> keystroke_ms;
  long long scorings = 0;
  for (int it = 0; it < iters; ++it) {
    const double t_begin = NowMs();

    // --- Prefill: what WarmUp() posts to the worker thread. One per
    // --scores-per-prefill scorings, which defaults to 1 -- the ratio this
    // tool has always assumed, on the grounds that a warm fires on every
    // commit and every composition start.
    //
    // That assumption is worth being able to break, and 1 is the pessimistic
    // end of it. WITHIN a composition the scoring context does not change --
    // it is the surrounding text plus the confirmed prefix, and neither moves
    // while the user adds letters to the current segment -- so one prefill
    // really does serve several keystrokes, each scoring a different candidate
    // list. That case also happens to be the only one where llama.cpp's graph
    // reuse can fire: gf_res_prev is a SINGLE slot keyed on ubatch shape
    // (llama-graph.h, allow_reuse), so alternating prefill and score misses it
    // every time -- measured, LLAMA_GRAPH_REUSE_DISABLE=1 changes score p50 by
    // 0.004 ms, i.e. reuse is currently doing nothing at all. Consecutive
    // scorings of the same shape are the only way to find out what it is
    // worth.
    // Same reasoning as the probe above: no BOS, and no AlignToTrainingForm
    // since this context is synthetic and its token count is what is being
    // measured.
    std::vector<llama_token> ctx_tokens = Tokenize(vocab, context, /*add_special=*/false);
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

    for (int sp = 0; sp < scores_per_prefill; ++sp) {
      // --- The gap production has and this loop did not. Between the prefill
      // and the score, not between iterations: LlmScorer's worker prefills when
      // a commit or a composition start queues a warm, and Score() runs on a
      // later keystroke, so the score phase is the first GPU work after the
      // idle. Sleeping between iterations instead would charge the wake-up to
      // the NEXT prefill and leave the score -- the number under study --
      // looking unaffected.
      //
      // Outside both timed spans by construction: t_prefilled is taken above it
      // and t_score_begin below, so neither phase contains the sleep. The wall
      // clock does contain it, which is why `slept_ms` is accumulated and
      // subtracted before the cpu/wall ratio is formed -- left in, a run with
      // --idle-ms would report a ratio that says "the work is on the GPU" purely
      // because the process spent most of its wall time asleep.
      if (idle_ms > 0) {
        const double t_sleep_begin = NowMs();
        if (idle_spin) {
          // The discriminator, not a mode anyone should benchmark in. Same
          // elapsed time, but this thread never yields, so whatever the
          // scheduler and the DVFS governor do to an idle core does not happen.
          // If the post-idle penalty survives this, it is time-based (a GPU
          // power state, memory eviction) and keeping a CPU busy could not
          // recover it; if it vanishes, the cost is the CPU clock and "keep
          // something warm before scoring" becomes an option worth pricing.
          while (NowMs() - t_sleep_begin < (double)idle_ms) {
            spin_sink = spin_sink + 1;
          }
        } else {
          std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
        }
        slept_ms += NowMs() - t_sleep_begin;
      }
      // --- The pre-spin. --idle-spin above proves the ceiling by never letting
      // the core go idle at all, which costs a whole core and can never ship.
      // This asks the shippable version of the same question: given that a
      // keystroke arrives with the clock already decayed, does a SHORT burst of
      // work immediately before the model runs bring it back?
      //
      // Deliberately AFTER the idle and OUTSIDE the score span. In production
      // the trigger would be the keystroke itself -- Copilot::ProcessKeyEvent
      // runs in the processor chain, ahead of the filter's Apply -- so the spin
      // is latency the user pays on top of the scoring, not instead of it. A
      // measurement that hid it inside the score phase would report a free win.
      //
      // --pre-spin-threads exists because one thread may only ramp one core.
      // The helpers are spawned per iteration rather than kept in a pool on
      // purpose: a pool of parked threads is itself a wake-up signal, and would
      // make the measured cost of the burst smaller than the shipped version
      // could ever be. Thread creation is ~20us here, inside the measured span
      // where it belongs.
      double spin_elapsed = 0.0;
      if (pre_spin_us > 0) {
        const double t_spin_begin = NowMs();
        const double want_ms = (double)pre_spin_us / 1000.0;
        std::vector<std::thread> helpers;
        for (int t = 1; t < pre_spin_threads; ++t) {
          helpers.emplace_back([t_spin_begin, want_ms]() {
            while (NowMs() - t_spin_begin < want_ms) {
              spin_sink = spin_sink + 1;
            }
          });
        }
        while (NowMs() - t_spin_begin < want_ms) {
          spin_sink = spin_sink + 1;
        }
        for (auto& h : helpers) {
          h.join();
        }
        spin_elapsed = NowMs() - t_spin_begin;
      }
      const double t_score_begin = NowMs();

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
            fprintf(stderr, "candidate batch overflow at %d (kNBatch %d)\n", batch.n_tokens,
                    kNBatch);
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
      const int n_decoded_this_iter = batch.n_tokens;
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

      score_ms.push_back(t_end - t_score_begin);
      if (sp == 0) {
        score_first_ms.push_back(t_end - t_score_begin);
      } else {
        score_next_ms.push_back(t_end - t_score_begin);
      }
      whole.push_back((t_prefilled - t_begin) + (t_end - t_score_begin));
      spin_ms.push_back(spin_elapsed);
      keystroke_ms.push_back(spin_elapsed + (t_end - t_score_begin));
      decoded_tokens += n_decoded_this_iter;
      scorings += 1;
    }
    prefill_ms.push_back(t_prefilled - t_begin);
  }
  // Busy wall time: what the cpu/wall ratio has always been about. Without the
  // subtraction --idle-ms would drive it toward zero and read as "entirely on
  // the GPU" for any configuration at all.
  const double wall_total = (NowMs() - wall_start) - slept_ms;
  const double cpu_total = CpuMs() - cpu_start;

  std::sort(whole.begin(), whole.end());
  std::sort(prefill_ms.begin(), prefill_ms.end());
  std::sort(score_ms.begin(), score_ms.end());
  std::sort(score_first_ms.begin(), score_first_ms.end());
  std::sort(score_next_ms.begin(), score_next_ms.end());
  std::sort(spin_ms.begin(), spin_ms.end());
  std::sort(keystroke_ms.begin(), keystroke_ms.end());

  const double decoded_per_iter = (double)decoded_tokens / (double)scorings;

  if (json) {
    // Hand-rolled rather than pulling nlohmann in: this target links llama.cpp
    // and nothing else on purpose (CLAUDE.md, "score_candidates links llama.cpp
    // and nlohmann_json only" -- the same linkage discipline, one dependency
    // tighter). The object is flat and every value is a number or a plain
    // string with no escaping to do.
    printf("{");
    printf("\"model\": \"%s\", ", model_path.c_str());
    printf("\"iters\": %d, ", iters);
    printf("\"threads\": %d, ", n_threads);
    printf("\"gpu_layers\": %d, ", n_gpu_layers);
    printf("\"context_chars\": %d, ", context_chars);
    printf("\"candidates\": %d, ", n_candidates);
    printf("\"candidate_chars\": %d, ", candidate_chars);
    printf("\"idle_ms\": %d, ", idle_ms);
    printf("\"qos\": %d, ", qos ? 1 : 0);
    printf("\"idle_spin\": %d, ", idle_spin ? 1 : 0);
    printf("\"pre_spin_us\": %d, ", pre_spin_us);
    printf("\"pre_spin_threads\": %d, ", pre_spin_threads);
    printf("\"spin_p50_ms\": %.4f, ", Percentile(spin_ms, 0.50));
    printf("\"keystroke_p50_ms\": %.4f, ", Percentile(keystroke_ms, 0.50));
    printf("\"keystroke_p99_ms\": %.4f, ", Percentile(keystroke_ms, 0.99));
    printf("\"decoded_per_iter\": %.3f, ", decoded_per_iter);
    printf("\"scores_per_prefill\": %d, ", scores_per_prefill);
    printf("\"score_p50_ms\": %.4f, ", Percentile(score_ms, 0.50));
    printf("\"score_first_p50_ms\": %.4f, ", Percentile(score_first_ms, 0.50));
    printf("\"score_next_p50_ms\": %.4f, ", Percentile(score_next_ms, 0.50));
    printf("\"score_p99_ms\": %.4f, ", Percentile(score_ms, 0.99));
    printf("\"prefill_p50_ms\": %.4f, ", Percentile(prefill_ms, 0.50));
    printf("\"prefill_p99_ms\": %.4f, ", Percentile(prefill_ms, 0.99));
    printf("\"both_p50_ms\": %.4f, ", Percentile(whole, 0.50));
    printf("\"both_p99_ms\": %.4f, ", Percentile(whole, 0.99));
    printf("\"cpu_ms_per_iter\": %.4f, ", cpu_total / scorings);
    printf("\"cpu_per_wall\": %.4f, ", cpu_total / wall_total);
    printf("\"logprob_checksum\": %.4f",
           score_checksum / ((double)scorings * (double)n_candidates));
    printf("}\n");
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
  }

  printf(
      "model %s  iters %d  threads %d  gpu_layers %d  context %d chars  candidates %d x %d "
      "chars  idle %d ms\n",
      model_path.c_str(), iters, n_threads, n_gpu_layers, context_chars, n_candidates,
      candidate_chars, idle_ms);
  // The line that says which of the two cost modes was measured. 0.00 means no
  // llama_decode ran at all -- every candidate was a single token, scored off
  // the prefill's own last logits -- which is the cheap mode and 44% of live
  // scorings. Any latency figure here is about the mode this number names.
  printf("tokens decoded/iter: %.2f  (%s)\n", decoded_per_iter,
         decoded_per_iter > 0.0 ? "the decode-bearing mode" : "the decode-free mode");
  printf("score   (the p99<10ms budget): p50 %.2f ms  p99 %.2f ms\n", Percentile(score_ms, 0.50),
         Percentile(score_ms, 0.99));
  printf("prefill (background warm-up):  p50 %.2f ms  p99 %.2f ms\n", Percentile(prefill_ms, 0.50),
         Percentile(prefill_ms, 0.99));
  if (pre_spin_us > 0) {
    printf("pre-spin %.2f ms + score  =    p50 %.2f ms  p99 %.2f ms   <- what a keystroke pays\n",
           Percentile(spin_ms, 0.50), Percentile(keystroke_ms, 0.50),
           Percentile(keystroke_ms, 0.99));
  }
  if (scores_per_prefill > 1) {
    // The graph-reuse question, in two numbers. A scoring that follows another
    // scoring of the same shape is the only one that can hit gf_res_prev; one
    // that follows a prefill never can.
    printf("  first after a prefill:       p50 %.2f ms   (graph reuse cannot fire)\n",
           Percentile(score_first_ms, 0.50));
    printf("  after another scoring:       p50 %.2f ms   (it can)\n",
           Percentile(score_next_ms, 0.50));
  }
  printf("both together:                 p50 %.2f ms  p99 %.2f ms\n", Percentile(whole, 0.50),
         Percentile(whole, 0.99));
  // cpu/wall well below 1 means the work is on the GPU and this line is NOT the
  // energy story -- pair it with `sudo powermetrics --samplers cpu_power,gpu_power`
  // while this runs. Above 1 it is the whole energy story: that many core-
  // milliseconds per scoring, on the CPU, and no GPU involved at all.
  printf("cpu time: %.2f ms per scoring  (cpu/busy-wall %.2f)\n", cpu_total / scorings,
         cpu_total / wall_total);
  // Not a result -- the mean log-probability the scoring produced, printed so
  // the arithmetic above has a reader and cannot be optimized away. Constant
  // across runs of the same model and context; a change means the score phase
  // stopped mirroring ScoreGroup.
  printf("mean candidate logprob: %.4f  (checksum, not a measurement)\n",
         score_checksum / ((double)scorings * (double)n_candidates));

  llama_batch_free(batch);
  llama_free(ctx);
  llama_model_free(model);
  llama_backend_free();
  return 0;
}
