#pragma once

// "What text is around the caret" -- one question, one answer.
//
// Shaped like GetSurroundingContext(), which it extends: a sequence of ifs in
// priority order, first to answer wins, tagged with its source. The fourth
// rung reconstructs from what this input method has committed, which is a
// strictly weaker answer -- hence AllowReconstruction, which is how a consumer
// says whether that weaker answer is acceptable to it.
//
// Before this existed each consumer improvised its own fallback: AutoSpacer
// with a second complete implementation, the others by declining. Those
// differences were structure; now they are one declaration each.

#include <optional>
#include <string>

#include "imk_client.h"  // SurroundingText, SurroundingSource

namespace rime {

class Context;

// No new enum: SurroundingSource (imk_client.h:18) already names where caret
// text came from, and "reconstructed" is a legitimate answer to that question.
// kReconstructed lives there, with the matching case in SurroundingSourceName
// -- which also means telemetry can record it without a second vocabulary.
struct CaretContext {
  std::string before;
  std::string after;
  SurroundingSource source = SurroundingSource::kNone;
  // Identifies the source connection (ImeBridge client, tmux pane, ...).
  // Populated from SurroundingText on rungs 1-3; left empty on rung 4 --
  // reconstruction has no connection to identify. AutoSpacer uses this to
  // look up per-client state without a second GetSurroundingContext() call,
  // which could otherwise race a disconnecting client between two reads.
  std::string client_key;
  // Provenance of `before`, carried for the same reason `client_key` is: so a
  // consumer that needs it does not have to call GetSurroundingContext() a
  // second time. That second call is NOT free and NOT guaranteed to agree --
  // GetSurroundingContext() is three independent lookups of which only tmux
  // memoises, and ImeBridgeServer::GetActiveContext() re-evaluates its TTL
  // under its mutex on every call, so the socket thread can invalidate the
  // bridge's answer between two statements and the second call is then
  // answered by tmux. A trace built from two such calls says source="bridge"
  // beside a depth describing the tmux snapshot. See rerank_filter.cc, which
  // made exactly that pair of calls until this field existed.
  //
  // Rung 4 leaves both at their "cannot say" defaults: a reconstruction knows
  // only the last commit record, so it has no honest depth to report and no
  // way to know what lies further back. -1 is that "cannot say" value, the
  // same convention RerankTrace::before_depth (rerank_trace.h) and
  // Event::before_depth (telemetry_event.h) use, and the one
  // SerializeJsonl reads to omit the key entirely -- 0 would instead claim
  // the source reached zero characters, which contradicts a non-empty
  // `before`. `after_depth` is deliberately not carried -- no consumer of
  // this struct reads it (verified by grep across src/), and a field nothing
  // reads is a field nothing keeps correct.
  int before_depth = -1;
  Truncation truncation = Truncation::kUnknown;
};

enum class AllowReconstruction { kNo, kYes };

// Rungs 1-3 (IMK / ImeBridge / tmux) are process-global and answer regardless
// of `ctx`. Rung 4 (reconstruction from commit history) is per-session --
// the history lives on a Context -- so it answers only when `ctx` is non-null
// and only when the caller opts in via `allow`.
//
// nullopt means no answer this consumer is willing to accept. Note that an
// unusable reconstruction yields nullopt rather than an empty CaretContext:
// an empty `before` is a positive claim that the caret is at the start of the
// text, and NeedSpaceBefore acts on it.
std::optional<CaretContext> GetCaretContext(Context* ctx, AllowReconstruction allow);

using SurroundingFn = std::optional<SurroundingText> (*)();
using HistoryTextFn = std::string (*)();
// Two nullptrs restore the real sources. Plain function pointers, matching the
// precedent set by SetTmuxTestHooks in src/tmux_source.cc.
void SetCaretContextTestHooks(SurroundingFn surrounding, HistoryTextFn history);

}  // namespace rime
