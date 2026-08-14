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
#include <deque>
#include <string>

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
  // `record.text` is empty when re-ranking ran but promoted nothing. The
  // context above is still meaningful in that case.
  telemetry::RerankRecord record;

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

  void Clear() { entries_.clear(); }
  size_t size() const { return entries_.size(); }

 private:
  std::deque<RerankTrace> entries_;  // oldest first
};

inline void RerankTraceStore::Record(const RerankTrace& trace) {
  if (!trace.valid) {
    return;
  }
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
