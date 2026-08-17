#pragma once

// The per-session counters behind the stats line (telemetry_event.h's
// StatsLine / SerializeStatsJsonl) -- the "everything" denominator warm-hit
// rate needs, as opposed to ShouldRecord's "hard cases only" scope.
//
// Pure logic, no Rime types beyond RerankTrace (itself a POD struct) and no
// I/O, driven directly by test/telemetry_stats_test.cc the same way
// ShouldRecord and BuildCommitEvents are (CLAUDE.md's testing discipline).
// Observe() is called once per non-placeholder segment BuildCommitEvents
// already iterates -- see its `stats` parameter -- rather than a second walk
// over the composition, so there is exactly one place that decides which
// segments count and how their trace is found.

#include <algorithm>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "rerank_llm.h"  // llm_rerank::SkipReason, SkipReasonName
#include "rerank_trace.h"
#include "telemetry_event.h"

namespace rime {
namespace telemetry {

// Nearest-rank percentile, sorted[min(n-1, floor(n*p))] -- the exact
// definition tools/rime_corpus/compare_rerank.py's `_percentile` already
// uses, so a live p50/p95 and the offline harness's are the same statistic
// rather than two numbers that merely look alike.
inline double Percentile(std::vector<int64_t> values, double p) {
  if (values.empty()) {
    return 0.0;
  }
  std::sort(values.begin(), values.end());
  const size_t idx = std::min(values.size() - 1, static_cast<size_t>(values.size() * p));
  return static_cast<double>(values[idx]);
}

class StatsAccumulator {
 public:
  // One segment was looked at. `trace` is whatever BuildCommitEvents found
  // for it (possibly null -- no trace was ever recorded, e.g. the re-ranking
  // filter itself is off). Always counts `segments`. When the LLM path
  // engaged (llm_skip == kNone), also counts `llm_acted` and records
  // `trace->llm.us` for the latency percentiles; otherwise, when a trace
  // exists but did not engage, counts the reason in `skip_counts`. A null
  // trace increments no skip bucket: which of the eight reasons applies is
  // not recoverable from "no trace at all", and guessing would misattribute
  // exactly the way rerank_trace.h's header comment warns against.
  void Observe(const RerankTrace* trace) {
    ++segments_;
    if (!trace) {
      return;
    }
    if (trace->llm_skip == llm_rerank::SkipReason::kNone) {
      ++llm_acted_;
      if (trace->llm.us >= 0) {
        us_samples_.push_back(trace->llm.us);
      }
      return;
    }
    ++skip_counts_[llm_rerank::SkipReasonName(trace->llm_skip)];
  }

  // A snapshot as of now. Does not reset -- the caller only commits to a
  // window closing (and calls Reset()) once the line this produced has
  // actually reached the Writer, so a write the Writer silently drops
  // (telemetry.h: "must never break input") does not also lose the counts
  // that would have described it.
  StatsLine Snapshot(const std::string& ts) const {
    StatsLine s;
    s.ts = ts;
    s.segments = segments_;
    s.llm_acted = llm_acted_;
    s.skip_counts = skip_counts_;
    s.us_p50 = Percentile(us_samples_, 0.5);
    s.us_p95 = Percentile(us_samples_, 0.95);
    return s;
  }

  void Reset() {
    segments_ = 0;
    llm_acted_ = 0;
    skip_counts_.clear();
    us_samples_.clear();
  }

  int64_t segments() const { return segments_; }

 private:
  int64_t segments_ = 0;
  int64_t llm_acted_ = 0;
  std::map<std::string, int64_t> skip_counts_;
  std::vector<int64_t> us_samples_;
};

}  // namespace telemetry
}  // namespace rime
