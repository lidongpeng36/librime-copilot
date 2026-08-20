#pragma once

// Turning a committed composition into telemetry events.
//
// A free function over a Context* rather than a method on the processor, for
// the reason CLAUDE.md gives: Engine::Create() pulls in Switcher/deployer init
// that a bare test main cannot provide, but a Context with a hand-built
// Composition can be driven directly (see test/commit_text_test.cc, and
// ComputeSpaceCommitText for the same shape). This is the integration point
// where the feature can silently produce nothing, so it is the one that most
// needs tests.
//
// `machine`, `schema` and `ts` are parameters rather than reads of
// Service::instance() so this stays free of global state.

#include <string>
#include <vector>

#include "rerank_trace.h"
#include "telemetry.h"
#include "telemetry_event.h"

namespace rime {

class Context;

namespace telemetry {

class StatsAccumulator;

// One event per segment worth recording. `traces` may be null; a segment with
// no matching entry in it is recorded with `rr` absent.
//
// `stats`, when non-null, observes EVERY non-placeholder segment this walks
// -- not just the ones ShouldRecord keeps -- so it is the one place that
// knows which segments count and how their trace is found; a second walk
// over the composition to gather the same counters would risk the two
// drifting apart the way rerank_trace.h's header warns a divergent span
// calculation once did.
//
// `selection_commit` is false for AutoSpacer's two bail-out paths -- Enter's
// raw commit and the number-key fallback's raw commit (auto_spacer.cc) --
// where the user discarded every candidate on offer and committed raw ASCII
// input instead. `seg.GetSelectedCandidate()` still reflects whatever was
// highlighted before the bail-out, but it was never committed, so `Event::sel`
// would misreport it as an accepted promotion (telemetry_commit.cc reads the
// composition, not the text actually handed to CommitText()). When false, no
// Event is produced for this call -- the accepted/rejected accounting
// (analyze_telemetry.py's `verdict`/`llm_verdict`, both keyed on `sel`) never
// sees a bail-out at all, which is the correct "invisible" side of that
// choice. `stats` still observes every segment either way: whether the LLM
// engaged for a segment is a fact about the scorer, independent of what the
// user ultimately did with the result, and StatsLine carries no `sel`-derived
// field for a bail-out to corrupt.
//
// `ok_seen`, when non-null, counts the plain successes this walk saw and is
// what makes Options::sample_ok's 1-in-N deterministic across calls. It is
// advanced for every plain success observed, not only the ones kept --
// advancing only on a keep would hold the counter at a multiple of N forever
// and record every success after the first.
std::vector<Event> BuildCommitEvents(Context* ctx, const RerankTraceStore* traces,
                                     const Options& options, const std::string& machine,
                                     const std::string& schema, const std::string& ts,
                                     StatsAccumulator* stats = nullptr,
                                     bool selection_commit = true, int64_t* ok_seen = nullptr);

}  // namespace telemetry
}  // namespace rime
