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

// One event per segment worth recording. `traces` may be null; a segment with
// no matching entry in it is recorded with `rr` absent.
std::vector<Event> BuildCommitEvents(Context* ctx, const RerankTraceStore* traces,
                                     const Options& options, const std::string& machine,
                                     const std::string& schema, const std::string& ts);

}  // namespace telemetry
}  // namespace rime
