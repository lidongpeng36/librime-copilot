//
// The two scoring backends, through the real Scorer seam, on one machine, in
// one process, interleaved.
//
// tools/bench_scorer.cc measures llama.cpp by REPLICATING its batch geometry;
// this one calls LlmScorer and MlxScorer themselves. That is the difference
// between "what the shape costs" and "what the shipped code costs", and it is
// the only vehicle that can answer whether a backend switch is worth taking:
// the replica cannot carry the warm cache, the worker thread, the tokenizer or
// the fallback chain, and every one of those is in the deployed path.
//
// TWO THINGS IT DOES THAT A SEPARATE-PROCESS COMPARISON CANNOT.
//
//   * It VERIFIES BEFORE IT TIMES. A latency comparison between two things
//     that compute different numbers is not a comparison. Both backends score
//     the same candidates against the same context and the tool refuses to
//     report timings if they disagree by more than --tolerance. MLX reads the
//     same gguf (its loader converts ggml Q8_0 to its own packed 8-bit affine
//     form) and agreement was 0.0023 on the probe set, so a real disagreement
//     means something is wrong, not that the backends are simply different.
//
//   * It INTERLEAVES. This machine drifts: the identical llama.cpp binary
//     measured 9.17 ms and then 10.30 ms half an hour apart, which is larger
//     than most effects worth shipping. Runs alternate A, B, A, B so drift
//     lands on both arms, and the per-arm spread is reported so a difference
//     smaller than it cannot be read as a result.
//
// --idle-ms goes between the warm and the scoring, for the reason
// tools/bench_scorer.cc's header gives at length: that is where the deployed
// gap is, and an idle over ~50 ms costs ~6x on both arms.
//
//   bench_backends --model <gguf> [--iters N] [--rounds N] [--idle-ms N]
//                  [--context-chars N] [--tolerance F] [--json]
//
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "llm_scorer.h"
#include "scorer.h"
#ifdef COPILOT_WITH_MLX
#include "mlx_scorer.h"
#endif

namespace {

using Clock = std::chrono::steady_clock;

double NowMs() {
  return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

// Same filler and the same reason as bench_scorer.cc: content is irrelevant to
// latency, real characters keep the tokenizer on the path it takes live.
const char* kFiller =
    "这个功能的能耗到底有多大我们先把它测出来然后再决定要不要在电池上继续开着"
    "输入法的候选窗口本来就在渲染所以显卡在这些时刻已经是醒着的并不需要额外唤醒";

std::string BuildContext(int chars) {
  std::string out;
  while ((int)out.size() / 3 < chars) {
    out += kFiller;
  }
  size_t end = 0;
  for (int i = 0; i < chars && end < out.size(); ++i) {
    end += 3;
  }
  return out.substr(0, end);
}

double Percentile(std::vector<double> v, double q) {
  if (v.empty()) return 0.0;
  std::sort(v.begin(), v.end());
  return v[std::min((size_t)(v.size() * q), v.size() - 1)];
}

double Median(std::vector<double> v) { return Percentile(std::move(v), 0.5); }

struct Arm {
  std::string name;
  std::vector<double> score_p50;  // one entry per round
  // WarmUp() to IsWarm(), i.e. the background prefill as deployed. It never
  // blocks a keystroke, so it is not the number the budget is about -- but it
  // is real work on a real machine and a comparison that omits it is not a
  // comparison of what the two backends cost, only of what they cost the user
  // at the moment of typing. Measured through the seam rather than inside it,
  // so it includes the queueing and the publish, which is what a caller
  // actually waits for.
  std::vector<double> warm_ms;
  std::vector<double> lock_us;
  std::vector<double> work_us;
  std::vector<double> n_decoded;
  std::vector<rime::CandidateScore> last_scores;
};

// One round: warm once, then `iters` scorings with `idle_ms` before each.
// Returns the round's score-phase p50 in milliseconds.
bool RunRound(rime::Scorer* scorer, const std::string& context,
              const std::vector<std::string>& candidates, int iters, int idle_ms, Arm* arm) {
  const double warm_t0 = NowMs();
  scorer->WarmUp(context);
  // The warm is asynchronous by contract. Waiting here rather than sleeping a
  // fixed amount: a fixed wait either wastes time or races, and a race would
  // show up as a cold Score() returning nothing, which reads as a backend
  // failure rather than as a harness bug.
  const double deadline = NowMs() + 30000.0;
  while (!scorer->IsWarm(context) && NowMs() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
  }
  if (!scorer->IsWarm(context)) {
    std::fprintf(stderr, "%s: never became warm\n", arm->name.c_str());
    return false;
  }
  // Only when the slot was actually cold. A repeat round finds it hot and
  // returns immediately, which would report a prefill of ~0 ms and drag the
  // median toward a number no prefill ever took.
  const double warm_ms = NowMs() - warm_t0;
  if (warm_ms > 1.0) {
    arm->warm_ms.push_back(warm_ms);
  }
  std::vector<double> ms;
  ms.reserve(iters);
  for (int i = 0; i < iters; ++i) {
    if (idle_ms > 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(idle_ms));
    }
    rime::ScoreTiming timing;
    const double t0 = NowMs();
    auto scores = scorer->Score(context, candidates, &timing);
    const double t1 = NowMs();
    if (scores.empty()) {
      std::fprintf(stderr, "%s: Score returned nothing on iteration %d\n", arm->name.c_str(), i);
      return false;
    }
    ms.push_back(t1 - t0);
    if (timing.lock_us >= 0) arm->lock_us.push_back((double)timing.lock_us);
    if (timing.work_us >= 0) arm->work_us.push_back((double)timing.work_us);
    if (timing.n_decoded >= 0) arm->n_decoded.push_back((double)timing.n_decoded);
    arm->last_scores = std::move(scores);
  }
  arm->score_p50.push_back(Percentile(ms, 0.5));
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  std::string model_path;
  int iters = 40, rounds = 3, idle_ms = 100, context_chars = 64;
  double tolerance = 0.01;
  bool json = false;
  // Energy is sampled by an external tool that cannot separate two arms in
  // one process, so --only runs a single backend under load. It is NOT for
  // latency: a one-arm run cannot be interleaved and this machine drifts
  // more than most effects worth shipping.
  std::string only;
  for (int i = 1; i < argc; ++i) {
    if (!std::strcmp(argv[i], "--model") && i + 1 < argc)
      model_path = argv[++i];
    else if (!std::strcmp(argv[i], "--iters") && i + 1 < argc)
      iters = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--rounds") && i + 1 < argc)
      rounds = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--idle-ms") && i + 1 < argc)
      idle_ms = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--context-chars") && i + 1 < argc)
      context_chars = std::atoi(argv[++i]);
    else if (!std::strcmp(argv[i], "--tolerance") && i + 1 < argc)
      tolerance = std::atof(argv[++i]);
    else if (!std::strcmp(argv[i], "--json"))
      json = true;
    else if (!std::strcmp(argv[i], "--only") && i + 1 < argc)
      only = argv[++i];
    else {
      std::fprintf(stderr,
                   "usage: %s --model <gguf> [--iters N] [--rounds N] [--idle-ms N]\n"
                   "       [--context-chars N] [--tolerance F] [--json] [--only llama|mlx]\n",
                   argv[0]);
      return 1;
    }
  }
  if (model_path.empty()) {
    std::fprintf(stderr, "--model is required\n");
    return 1;
  }
#ifndef COPILOT_WITH_MLX
  std::fprintf(stderr, "this build has no MLX backend; configure with -DCOPILOT_WITH_MLX=ON\n");
  return 1;
#else
  const std::string context = BuildContext(context_chars);
  const std::vector<std::string> candidates = {"务必", "无比", "五笔", "舞弊"};

  auto llama = std::make_unique<rime::LlmScorer>(model_path);
  auto mlx = std::make_unique<rime::MlxScorer>(model_path);
  Arm a_llama{"llama.cpp", {}, {}, {}, {}, {}};
  Arm a_mlx{"mlx", {}, {}, {}, {}, {}};

  // One untimed round each. The first call loads the model and, on MLX,
  // compiles Metal pipelines -- a per-process cost that belongs in neither
  // percentile.
  if (!RunRound(llama.get(), context, candidates, 3, 0, &a_llama)) return 1;
  if (!RunRound(mlx.get(), context, candidates, 3, 0, &a_mlx)) return 1;

  // Verify before timing. Both arms just scored the same candidates against
  // the same context; if they disagree there is nothing to compare.
  double worst = 0.0;
  for (size_t i = 0; i < candidates.size(); ++i) {
    if (i >= a_llama.last_scores.size() || i >= a_mlx.last_scores.size()) break;
    worst = std::max(
        worst, (double)std::fabs(a_llama.last_scores[i].logprob - a_mlx.last_scores[i].logprob));
  }
  if (!json) {
    std::printf("agreement check, %d candidates: worst |diff| = %.5f (tolerance %.5f)\n",
                (int)candidates.size(), worst, tolerance);
    for (size_t i = 0; i < candidates.size(); ++i) {
      std::printf("  %-8s llama %10.4f   mlx %10.4f   %+8.5f\n", candidates[i].c_str(),
                  a_llama.last_scores[i].logprob, a_mlx.last_scores[i].logprob,
                  a_mlx.last_scores[i].logprob - a_llama.last_scores[i].logprob);
    }
  }
  if (worst > tolerance) {
    std::fprintf(stderr,
                 "REFUSING to report timings: the backends disagree by %.5f, above the %.5f "
                 "tolerance. A latency comparison between two things computing different "
                 "numbers is not a comparison.\n",
                 worst, tolerance);
    return 2;
  }

  a_llama.score_p50.clear();
  a_mlx.score_p50.clear();
  if (!only.empty()) {
    // One arm, for an external power sampler. The agreement check above still
    // ran, so this is not a way to skip verification.
    rime::Scorer* s = (only == "mlx") ? (rime::Scorer*)mlx.get() : (rime::Scorer*)llama.get();
    Arm* arm = (only == "mlx") ? &a_mlx : &a_llama;
    std::printf("running %s only: %d scorings at %d ms idle\n", arm->name.c_str(), iters, idle_ms);
    if (!RunRound(s, context, candidates, iters, idle_ms, arm)) return 1;
    std::printf("%s: score p50 %.2f ms over %d scorings\n", arm->name.c_str(),
                Median(arm->score_p50), iters);
    return 0;
  }
  for (int r = 0; r < rounds; ++r) {
    // A different context per round, so every round's warm is genuinely cold
    // and the prefill is measured `rounds` times rather than once. Same
    // length, so the work is identical -- only the slot's identity changes.
    const std::string ctx = context + std::string(r, ' ');
    if (!RunRound(llama.get(), ctx, candidates, iters, idle_ms, &a_llama)) return 1;
    if (!RunRound(mlx.get(), ctx, candidates, iters, idle_ms, &a_mlx)) return 1;
  }

  auto spread = [](const std::vector<double>& v) {
    if (v.size() < 2) return 0.0;
    auto [lo, hi] = std::minmax_element(v.begin(), v.end());
    const double m = Median(v);
    return m ? (*hi - *lo) / m * 100.0 : 0.0;
  };
  const double la = Median(a_llama.score_p50), mx = Median(a_mlx.score_p50);
  if (json) {
    std::printf(
        "{\"agreement\": %.6f, \"iters\": %d, \"rounds\": %d, \"idle_ms\": %d, "
        "\"context_chars\": %d, \"llama_score_p50_ms\": %.4f, \"mlx_score_p50_ms\": %.4f, "
        "\"llama_spread_pct\": %.2f, \"mlx_spread_pct\": %.2f, "
        "\"llama_work_us_p50\": %.1f, \"mlx_work_us_p50\": %.1f, "
        "\"llama_lock_us_p50\": %.1f, \"mlx_lock_us_p50\": %.1f, "
        "\"llama_n_decoded\": %.1f, \"mlx_n_decoded\": %.1f, \"llama_warm_p50_ms\": %.3f, "
        "\"mlx_warm_p50_ms\": %.3f, \"llama_warm_n\": %d, \"mlx_warm_n\": %d}\n",
        worst, iters, rounds, idle_ms, context_chars, la, mx, spread(a_llama.score_p50),
        spread(a_mlx.score_p50), Median(a_llama.work_us), Median(a_mlx.work_us),
        Median(a_llama.lock_us), Median(a_mlx.lock_us), Median(a_llama.n_decoded),
        Median(a_mlx.n_decoded), Median(a_llama.warm_ms), Median(a_mlx.warm_ms),
        (int)a_llama.warm_ms.size(), (int)a_mlx.warm_ms.size());
    return 0;
  }
  std::printf("\ninterleaved, %d rounds x %d scorings, idle %d ms, context %d chars\n", rounds,
              iters, idle_ms, context_chars);
  std::printf("%12s %12s %9s %12s %12s %10s\n", "backend", "score p50", "spread", "warm p50",
              "work_us p50", "n_decoded");
  auto row = [&](const Arm& arm, double p50) {
    std::printf("%12s %9.2f ms %8.0f%% %9.2f ms %12.0f %10.0f\n", arm.name.c_str(), p50,
                spread(arm.score_p50), Median(arm.warm_ms), Median(arm.work_us),
                Median(arm.n_decoded));
  };
  row(a_llama, la);
  row(a_mlx, mx);
  std::printf("\n  mlx vs llama.cpp: %+.1f%%  (combined spread %.0f%%)\n", (mx - la) / la * 100.0,
              spread(a_llama.score_p50) + spread(a_mlx.score_p50));
  return 0;
#endif
}
