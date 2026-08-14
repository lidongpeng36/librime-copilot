#include "telemetry_commit.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/segmentation.h>

namespace rime {
namespace telemetry {

std::vector<Event> BuildCommitEvents(Context* ctx, const RerankTraceStore* traces,
                                     const Options& options, const std::string& machine,
                                     const std::string& schema, const std::string& ts) {
  std::vector<Event> events;
  if (!ctx) {
    return events;
  }
  const std::string& input = ctx->input();

  for (const Segment& seg : ctx->composition()) {
    // Prediction placeholders are already db-ordered, and re-ranking skips them
    // too (CopilotRerankFilter::AppliesToSegment, rerank_filter.cc:149).
    if (seg.HasTag("copilot")) {
      continue;
    }
    // TraceSpanOf, not `seg.end` and not a second copy of its arithmetic: the
    // filter keys its write on the very same function (rerank_filter.cc,
    // AppliesToSegment), which is what stops the two ends of this path drifting
    // apart. See rerank_trace.h for why the span is the segment's original
    // extent rather than its possibly narrowed `end`.
    const TraceSpan span = TraceSpanOf(seg);
    const RerankTrace* trace = traces ? traces->Find(input, span.start, span.end) : nullptr;
    const bool traced = trace != nullptr;
    const bool promoted = traced && !trace->record.text.empty();
    const int sel_idx = static_cast<int>(seg.selected_index);
    if (!ShouldRecord(sel_idx, promoted)) {
      continue;
    }
    auto selected = seg.GetSelectedCandidate();
    if (!selected) {
      continue;
    }

    Event e;
    e.ts = ts;
    e.machine = machine;
    e.schema = schema;
    if (seg.end > seg.start && seg.end <= input.size()) {
      e.input = input.substr(seg.start, seg.end - seg.start);
    }
    e.sel_idx = sel_idx;
    e.sel = selected->text();
    for (int i = 0; i < options.top_n; ++i) {
      auto cand = seg.GetCandidateAt(static_cast<size_t>(i));
      if (!cand) {
        break;
      }
      e.top.push_back(cand->text());
    }
    if (traced) {
      // Deliberately from the trace, not a fresh GetSurroundingContext(): on a
      // terminal that call spawns tmux, and a subprocess per commit is not
      // worth context we already captured.
      e.ctx = trace->ctx;
      e.src = trace->src;
    }
    if (promoted) {
      e.rr = trace->record;
    }
    events.push_back(std::move(e));
  }
  return events;
}

}  // namespace telemetry
}  // namespace rime
