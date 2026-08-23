#include "telemetry_commit.h"

#include <rime/candidate.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/segmentation.h>

#include "telemetry_stats.h"

namespace rime {
namespace telemetry {

std::vector<Event> BuildCommitEvents(Context* ctx, const RerankTraceStore* traces,
                                     const Options& options, const std::string& machine,
                                     const std::string& schema, const std::string& ts,
                                     StatsAccumulator* stats, bool selection_commit,
                                     int64_t* ok_seen) {
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
    // Every segment reaching this point observes stats, regardless of
    // ShouldRecord below -- that gate is what makes the per-event stream
    // unfit to measure warm-hit rate in the first place (telemetry_event.h).
    // This happens even on a bail-out (selection_commit == false): whether
    // the scorer engaged for this segment is a fact about the scorer, not
    // about what the user ultimately committed.
    if (stats) {
      stats->Observe(trace);
    }
    // A bail-out never reaches Event construction below: `seg.GetSelectedCandidate()`
    // still names whatever was highlighted when the user pressed Enter/a
    // stale number key instead, and Event::sel would report it as accepted
    // when it was in fact discarded. See this function's header comment.
    if (!selection_commit) {
      continue;
    }
    const bool traced = trace != nullptr;
    const bool db_promoted = traced && !trace->record.text.empty();
    // Engaged, not merely traced: llm_skip == kNone is the only state where
    // trace->llm was filled in at all (rerank_trace.h) -- everywhere else
    // (disabled/battery/nomodel/noctx/cold) llm.text is empty for a reason
    // that has nothing to do with promotion, and must not be read as one.
    const bool llm_engaged = traced && trace->llm_skip == llm_rerank::SkipReason::kNone;
    const bool llm_promoted = llm_engaged && !trace->llm.text.empty();
    // Both re-rankers count: `promoted` decides ShouldRecord's scope
    // (telemetry_event.h), and a db-shaped check here silently drops every
    // LLM-only promotion, leaving Event::llm populated in principle but never
    // in practice.
    const bool promoted = db_promoted || llm_promoted;
    const int sel_idx = static_cast<int>(seg.selected_index);
    // Without a counter there is no way to make 1-in-N deterministic, so a
    // caller that passes no `ok_seen` gets today's behaviour (no plain
    // successes) rather than an ok_index stuck at 0, which would otherwise
    // read as "always the first of every N" and record every plain success.
    int64_t ok_index = 0;
    const int sample_ok = ok_seen ? options.sample_ok : 0;
    if (!promoted && sel_idx == 0 && ok_seen) {
      ok_index = (*ok_seen)++;
    }
    if (!ShouldRecord(sel_idx, promoted, ok_index, sample_ok)) {
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
      e.before_depth = trace->before_depth;
      e.trunc = trace->truncation == Truncation::kUnknown
                    ? std::string()
                    : std::string(TruncationName(trace->truncation));
      e.llm_skip = llm_rerank::SkipReasonName(trace->llm_skip);
    }
    if (db_promoted) {
      e.rr = trace->record;
    }
    // Whenever the model was actually consulted for this segment -- promoted
    // or declined -- not only on llm_promoted: a decline that still left the
    // user picking a non-first candidate (ShouldRecord's other trigger) is
    // exactly the case that tells "the LLM ran and disagreed with the user"
    // apart from "the LLM never got a chance here".
    if (llm_engaged) {
      e.llm = trace->llm;
    }
    events.push_back(std::move(e));
  }
  return events;
}

}  // namespace telemetry
}  // namespace rime
