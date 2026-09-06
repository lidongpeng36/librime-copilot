#pragma once

// The schema keys that MORE THAN ONE component reads.
//
// Nothing else belongs here. A key with a single reader is read where it is
// used -- `copilot/tmux_source/*` and `copilot/context_memory/*` in the
// processor, `copilot/max_candidates` and `copilot/llm/*` in the engine
// component, `copilot/rerank/window` in the filter -- because a key with one
// reader cannot disagree with itself, and moving it here would only put more
// distance between the read and the use.
//
// What IS here is every key that had two or three independent readers, each
// with its own spelling of the path, its own default and (for the two lengths)
// its own clamp, kept in step by hand:
//
//   copilot/db                            filter + engine component
//   copilot/rerank/enable                 processor + filter + engine component
//   copilot/rerank/max_context_chars      processor + filter   (both clamped)
//   copilot/rerank/llm/enable             processor + filter + engine component
//   copilot/rerank/llm/model              filter + engine component
//   copilot/rerank/llm/battery_active     processor + filter
//   copilot/rerank/llm/context_chars      processor + filter   (both clamped)
//   copilot/telemetry/*                   processor ctor + CopilotComponent::Create
//
// That last one was a verbatim eight-line duplicate inside a single file.
//
// DELIBERATELY NOT CACHED, and this is the load-bearing decision. Caching by
// schema_id is the obvious shape -- CopilotEngineComponent already keys two
// maps that way -- and it is wrong here: a redeploy changes the config while
// the schema_id stays put, so the cache would serve the pre-deploy value
// forever. `copilot_engine_by_schema_id` escapes that only because it holds a
// weak<> that dies with the instance, which a plain value cache has no
// analogue for. So each component calls these at its own construction time and
// the keys are read two or three times, which is a YAML map lookup off the
// critical path. The goal was never to read once; it was to make the readers
// unable to DISAGREE, and one function is what does that.
//
// Testable without an engine: rime::Config has a public default constructor
// and an exported LoadFromStream, so test/copilot_config_test.cc drives real
// YAML. Both clamps and every default are pinned there.

#include <string>

#include "telemetry.h"  // telemetry::Options

namespace rime {

class Config;

// Defaults below are the ones the individual call sites carried before this
// existed; they already agreed, and the test pins them so they go on agreeing.
struct CopilotSharedConfig {
  std::string db = "copilot.db";

  bool rerank_enable = true;
  // Clamped to [1, kMaxSurroundingPrefixChars] by the reader. Both this and
  // `llm_context_chars` are terms in SurroundingPrefixChars (surrounding_source.h),
  // so since 2026-08-28 they size a per-keystroke query rather than merely
  // truncating a string already fetched -- an unclamped value would make every
  // fetch report itself truncated by config forever.
  int rerank_max_context_chars = 8;

  bool llm_enable = false;
  std::string llm_model;
  bool llm_battery_active = false;
  // Clamped, same reason as above. The SCORER's context: a different and
  // longer string than the db's Han-only tail, because a language model reads
  // punctuation and Latin and an n-gram key cannot.
  int llm_context_chars = 32;
};

// `config` may be null (a ticket with no schema): every field then keeps the
// default above, which is what each call site did for itself.
CopilotSharedConfig ReadCopilotSharedConfig(Config* config);

// Reads copilot/telemetry/* and runs telemetry::ClampOptions, which both old
// sites did. Null `config` yields clamped defaults.
telemetry::Options ReadTelemetryOptions(Config* config);

}  // namespace rime
