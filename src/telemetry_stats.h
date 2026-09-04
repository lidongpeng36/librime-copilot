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
#include "utf8_index.h"

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
  //
  // Every non-null trace ALSO feeds the surrounding-fetch counters, whatever
  // the LLM did with it: how the fetch was truncated is a property of the
  // frontend, not of the scorer, and a segment the model skipped had its
  // context fetched all the same. See ObserveFetch.
  // One warm-up, classified against the one before it. Records the CLASS and
  // a character count, never the context itself: a scoring context is the
  // user's own text, which is why the evaluation corpus is kept off this
  // remote at all (CLAUDE.md, "Where the design records live"), and a
  // telemetry line that carried 64 characters of it per warm would put it
  // somewhere no such decision was made about.
  //
  // The three classes are exactly the applicability of an incremental
  // prefill, which is the question this exists to answer:
  //
  //   dedup   the same context as last time -- the scorer's WarmUp returns
  //           early and no prefill happens at all
  //   extend  the previous context is a PREFIX of this one, so its KV entries
  //           are still valid at the positions they already hold and only the
  //           new characters would need decoding
  //   rebuild anything else: the window slid, the caret moved, the app
  //           changed. Positions shift, so the cache cannot be extended
  void ObserveWarm(const std::string& context, const std::string& previous) {
    if (context == previous) {
      ++warm_counts_["dedup"];
      return;
    }
    if (!previous.empty() && context.rfind(previous, 0) == 0) {
      ++warm_counts_["extend"];
      warm_extend_chars_.push_back(
          static_cast<int64_t>(::copilot::CharCount(context) - ::copilot::CharCount(previous)));
      return;
    }
    ++warm_counts_["rebuild"];
  }

  void Observe(const RerankTrace* trace) {
    ++segments_;
    if (!trace) {
      return;
    }
    ObserveFetch(*trace);
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
  // `fetch_chars` is config-fixed and lives on the caller (Copilot reads it in
  // its constructor), so it is passed in rather than accumulated -- passing it
  // at Snapshot time also keeps Reset() from having to preserve it, which is
  // the shape of bug a stored copy invites.
  StatsLine Snapshot(const std::string& ts, int fetch_chars = -1) const {
    StatsLine s;
    s.ts = ts;
    s.fetch_chars = fetch_chars;
    s.segments = segments_;
    s.llm_acted = llm_acted_;
    s.skip_counts = skip_counts_;
    s.warm_counts = warm_counts_;
    s.warm_extend_chars_p50 = Percentile(warm_extend_chars_, 0.5);
    s.us_p50 = Percentile(us_samples_, 0.5);
    s.us_p95 = Percentile(us_samples_, 0.95);
    s.trunc_counts = trunc_counts_;
    // Left at StatsLine's negative default when nothing was cut short by the
    // environment: Percentile() answers 0.0 for an empty vector, and a 0 here
    // would read as "the source reached no characters at all", which is a
    // real and very different finding. See StatsLine's field comment.
    if (!depth_samples_.empty()) {
      s.depth_p50 = Percentile(depth_samples_, 0.5);
      s.depth_p95 = Percentile(depth_samples_, 0.95);
    }
    return s;
  }

  void Reset() {
    segments_ = 0;
    llm_acted_ = 0;
    skip_counts_.clear();
    us_samples_.clear();
    warm_counts_.clear();
    warm_extend_chars_.clear();
    trunc_counts_.clear();
    depth_samples_.clear();
  }

  int64_t segments() const { return segments_; }

 private:
  // Why this segment's surrounding fetch stopped, and -- only when it stopped
  // because the environment ran out -- how deep it had got.
  //
  // kUnknown counts as "unknown" rather than being dropped: it is the honest
  // answer for the IME Bridge (imk_client.h), and a window that is mostly
  // unknown means the question cannot be answered from that data at all,
  // which has to be visible rather than quietly missing from the denominator.
  //
  // The depth sample is conditional on kByScreen/kByApp for the reason
  // StatsLine's `depth_p50` comment gives: the other three truncations carry
  // a depth that is not a limit. A trace that reported a truncation but no
  // depth (-1) still counts its bucket; only its depth is missing, and
  // pushing the -1 would report a fetch that reached nothing.
  void ObserveFetch(const RerankTrace& trace) {
    ++trunc_counts_[TruncationName(trace.truncation)];
    const bool cut_short =
        trace.truncation == Truncation::kByScreen || trace.truncation == Truncation::kByApp;
    if (cut_short && trace.before_depth >= 0) {
      depth_samples_.push_back(trace.before_depth);
    }
  }

  int64_t segments_ = 0;
  int64_t llm_acted_ = 0;
  std::map<std::string, int64_t> skip_counts_;
  std::vector<int64_t> us_samples_;
  std::map<std::string, int64_t> warm_counts_;
  std::vector<int64_t> warm_extend_chars_;
  std::map<std::string, int64_t> trunc_counts_;
  std::vector<int64_t> depth_samples_;
};

}  // namespace telemetry
}  // namespace rime
