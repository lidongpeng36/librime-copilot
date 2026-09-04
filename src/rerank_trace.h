#pragma once

// The re-ranking decisions for the composition being typed, carried from the
// moment each is made to the moment the user's choice is known.
//
// RerankTranslation::Replenish() decides the promotion lazily, while the menu
// is built; the selection only arrives at commit. These objects bridge that gap.
// They are shared per schema through CopilotEngineComponent — not a file-level
// global — so two schemas can never read each other's decisions.
//
// One slot is not enough. A composition can hold several segments, each with
// its own menu and its own re-ranking decision, and every segment is recorded
// separately at commit. With a single slot the last segment to build its menu
// destroyed every earlier segment's decision, and the analysis tool files an
// event whose `rr` is missing as "no re-ranking, the translator mis-ranked" —
// the bucket that flatters this filter. Under-reporting would have been
// harmless; silently moving events into the wrong bucket is not.
//
// The matching rule is deliberately strict. A trace left over from an abandoned
// composition and credited to the next one would corrupt exactly the data this
// telemetry exists to gather. Dropping a real event is acceptable;
// misattributing one is not.

#include <rime/segmentation.h>

#include <cstddef>
#include <cstdint>
#include <deque>
#include <string>

#include "imk_client.h"  // Truncation
#include "rerank_llm.h"  // llm_rerank::SkipReason
#include "telemetry_event.h"

namespace rime {

// The span a trace is keyed on. Both ends of the telemetry path must derive it
// from the same expression, so it lives here rather than being spelled out
// twice: rerank_filter.cc writes it, telemetry_commit.cc looks it up.
//
// `start + length`, not `end`. Segment::Close() narrows `end` to the selected
// candidate's end on a partial selection (librime src/rime/segmentation.cc:17-24)
// while `length` is fixed at construction and never touched again — librime's
// own name for the same sum is `original_end_pos` (Segment::Reopen, :30).
//
// The filter used to key its trace on the HEAD CANDIDATE's end instead. That is
// a different number whenever the head candidate is a partial match, and then
// the commit-side lookup missed, the event was written with `rr` absent, and
// analyze_telemetry.py filed it as "misrank" — the bucket that blames the
// translator rather than this filter. Deriving both ends from one function is
// what makes that class of disagreement unrepresentable.
struct TraceSpan {
  size_t start = 0;
  size_t end = 0;
};

inline TraceSpan TraceSpanOf(const Segment& segment) {
  return TraceSpan{segment.start, segment.start + segment.length};
}

struct RerankTrace {
  bool valid = false;
  std::string input;  // the FULL context input at decision time
  size_t start = 0;   // the segment's span within that input
  size_t end = 0;
  std::string ctx;  // TrailingCjkRun the decision was made against
  std::string src;  // which surrounding-text source supplied it
  // What the surrounding fetch behind `ctx` actually managed, recorded at
  // decision time. Carried on the trace rather than re-read at commit because
  // the fetch that produced this decision is gone by then -- a second
  // GetSurroundingContext() at commit describes a different caret.
  int before_depth = -1;
  Truncation truncation = Truncation::kUnknown;
  // `record.text` is empty when re-ranking ran but promoted nothing. The
  // context above is still meaningful in that case.
  telemetry::RerankRecord record;
  // What Apply()'s fallback chain (rerank_filter.cc) decided for the LLM path
  // on this segment -- kNone means eligible (the scorer was consulted; a
  // subsequent kNoHan/kMargin from Decide() does not retroactively change
  // this, "engaged" and "engaged but declined to promote" are both kNone
  // here). Measurement consumer: replay_copilot.cc's --wait-for-warm reads
  // this rather than re-deriving it, because every OTHER guard in the chain
  // (llm.enable, battery, warm/loaded) is constant for the run and a
  // re-derivation collapses to "is there a trailing Han context at all" --
  // see task-6-report.md's review notes for how that was caught.
  llm_rerank::SkipReason llm_skip = llm_rerank::SkipReason::kDisabled;
  // Wall-clock microseconds of the Scorer::Score() call this trace's decision
  // came from, or -1 when no Score() call was made (llm_skip != kNone, i.e.
  // the db path decided instead). NOT the same thing as replay_copilot.cc's
  // pre-existing `us.menu` timer: that wraps WalkCandidates(), which for a
  // segment materialized inside a PRECEDING select_candidate() call (measured
  // directly -- see task-6-report.md) reads an already-cached menu and times
  // a cache hit, not the scoring. This field is the only place that measures
  // the real thing.
  int64_t score_us = -1;
  // The two halves of `score_us`, and whether the batch decoded -- straight
  // off ScoreTiming (scorer.h), -1 when Score() was never called or returned
  // before it could measure. `score_us` is deliberately NOT redefined as their
  // sum: it is the outer wall time including the little either side of the
  // lock, it is what the p99 budget is written against, and every figure
  // recorded before schema v7 is that quantity. Two of these three are a
  // latency attribution (which half moved) and the third is the cost class the
  // latency turned out to be bimodal on.
  int64_t score_lock_us = -1;
  int64_t score_work_us = -1;
  int score_n_decoded = -1;
  // The LLM path's decision, in the shape telemetry needs. Populated only
  // inside the `llm_scorer_` branch of RerankTranslation::Replenish(), i.e.
  // exactly when llm_skip above is kNone -- `text` empty then still
  // distinguishes "engaged but declined" (kNoHan/kMargin, telemetry_commit.cc
  // reads `llm.skip` for which) from a real promotion, the same way
  // `record.text` does for the db path. Left default (an absent-looking
  // record) whenever llm_skip != kNone, so a commit-side reader must check
  // llm_skip before trusting this rather than guessing from an empty `text`
  // alone -- kNoModel and a genuine decline both leave `text` empty.
  telemetry::LlmRecord llm;

  void Clear() { *this = RerankTrace{}; }
};

// Believe a trace only for the exact segment it was recorded for.
inline bool TraceMatches(const RerankTrace& trace, const std::string& input, size_t start,
                         size_t end) {
  return trace.valid && trace.input == input && trace.start == start && trace.end == end;
}

// Every segment's decision for the composition currently being typed, keyed by
// the (context input, span) triple TraceMatches compares.
//
// Bounded on purpose: Record() runs on a keystroke path, so an unbounded map
// would be a slow leak for any composition that keeps re-segmenting. A real
// composition never carries anything close to kMaxEntries segments; beyond it
// the oldest entry is dropped, which costs an event and never misattributes
// one.
//
// The whole store is cleared at commit *and* whenever the composition empties
// (Copilot::OnContextUpdate), so entries never outlive the composition they
// describe.
class RerankTraceStore {
 public:
  static constexpr size_t kMaxEntries = 16;

  // Records `trace` under its own (input, start, end). Re-recording the same
  // segment — Apply() builds a fresh translation on every keystroke — replaces
  // the previous decision in place rather than growing the store.
  void Record(const RerankTrace& trace);

  // The decision recorded for exactly this segment, or null.
  const RerankTrace* Find(const std::string& input, size_t start, size_t end) const;

  // The most recently recorded trace whose span STARTS at `start`, whatever
  // input it was recorded under, or null.
  //
  // Measurement-only consumer, and a different question from Find(): the caller
  // (replay_copilot.cc, segment 0) knows which segment it wants but not the
  // `input` that was live when the decision was made, nor `end` -- Apply()
  // re-records a segment on every keystroke, so both move while `start` does
  // not. It is also the only query that works without an observation window at
  // all, which is what segment 0 needs: ProcessRequest feeds every key of a
  // request before the loop that could open one.
  //
  // Newest-first so a segment re-recorded across several keystrokes answers
  // with its latest decision, and so a composition longer than kMaxEntries --
  // where the segment's early entries have been evicted -- still answers from
  // the entries the newest keystroke wrote.
  const RerankTrace* FindLatestByStart(size_t start) const;

  // The most recently recorded trace, or null. Measurement-only consumer:
  // replay_copilot.cc reads this immediately after the one call (the first
  // WalkCandidates() of a request, or a select_candidate()) it knows can
  // record at most one new trace on this single-threaded input path --
  // paired with a records() check before/after to tell "a fresh trace landed"
  // from "nothing changed", this sidesteps reconstructing the exact
  // (input, start, end) a segment was recorded under, which depends on
  // segmentation internals the tool has no other reason to model (see
  // TraceSpanOf's own comment on why `end` is not what a caller would guess).
  //
  // Held separately rather than read off entries_.back(): a re-record replaces
  // in place, so the newest WRITE is frequently not the newest ENTRY, and
  // back() then names a different segment entirely.
  const RerankTrace* Last() const { return records_ ? &last_recorded_ : nullptr; }

  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

  // Every Record() that actually recorded, counted for the lifetime of the
  // store. NOT size(): that saturates at kMaxEntries and does not move for an
  // in-place replacement either, so a size() delta cannot answer "did this call
  // record anything". replay_copilot.cc asked it that way and went blind past
  // the 16th entry -- measured on the real corpus, 100% of the 148 segments
  // sitting past 16 fed keys came back "notrace", against 57% below it.
  //
  // Deliberately NOT reset by Clear(): the delta is read across a call that may
  // contain a commit, and a commit clears the store. Resetting would make the
  // delta negative and read as "nothing recorded" -- the very failure this
  // exists to fix.
  size_t records() const { return records_; }

 private:
  std::deque<RerankTrace> entries_;  // oldest first
  RerankTrace last_recorded_;
  size_t records_ = 0;
};

inline void RerankTraceStore::Record(const RerankTrace& trace) {
  if (!trace.valid) {
    return;
  }
  last_recorded_ = trace;
  ++records_;
  for (auto& entry : entries_) {
    if (TraceMatches(entry, trace.input, trace.start, trace.end)) {
      entry = trace;
      return;
    }
  }
  entries_.push_back(trace);
  while (entries_.size() > kMaxEntries) {
    entries_.pop_front();
  }
}

inline const RerankTrace* RerankTraceStore::FindLatestByStart(size_t start) const {
  for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
    if (it->valid && it->start == start) {
      return &*it;
    }
  }
  return nullptr;
}

inline const RerankTrace* RerankTraceStore::Find(const std::string& input, size_t start,
                                                 size_t end) const {
  for (const auto& entry : entries_) {
    if (TraceMatches(entry, input, start, end)) {
      return &entry;
    }
  }
  return nullptr;
}

}  // namespace rime
