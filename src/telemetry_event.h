#pragma once

// The telemetry record, and the pure decisions around it.
//
// Everything here is free of Rime types and of I/O, so it can be unit-tested
// directly (test/telemetry_event_test.cc) — the same split the rest of this
// plugin uses (see rerank.h, prediction_context.h).
//
// Why each field exists, and which claim about the ranking code it can refute,
// is documented in
// docs/superpowers/specs/2026-08-14-prediction-telemetry-design.md. Do not drop
// a field because it looks redundant: `n` in particular is what makes `rank`
// interpretable at all, since rank 12 of 20 and rank 12 of 5000 are different
// evidence by orders of magnitude.

#include <nlohmann/json.hpp>

#include <cmath>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace rime {
namespace telemetry {

// Bumped whenever a field changes meaning, so a reader can tell old lines from
// new ones. Files outlive the code that wrote them.
//
// v2 adds `llm` to Event and introduces the stats line (StatsLine /
// SerializeStatsJsonl); a v1 line has neither `llm` nor `type` and must keep
// loading unchanged under a v2 reader.
inline constexpr int kSchemaVersion = 2;

// What the re-ranking filter decided for one segment.
struct RerankRecord {
  std::string key;   // the lookup key that won the backoff
  int key_len = 0;   // its length in characters
  int n = 0;         // how many continuations that key has
  std::string text;  // the promoted candidate
  int from = 0;      // its position before promotion; 0 means nothing moved
  int rank = 0;      // its rank among the key's continuations
  int level = 0;     // its match level: 3 exact, 2/1 partial
};

// What the LLM scoring path decided for one segment.
struct LlmRecord {
  std::string text;       // the promoted candidate, empty when nothing moved
  std::string incumbent;  // the first all-Han candidate it was compared against.
                          // Without this the "never touch bucket B" invariant
                          // cannot be verified in production.
  int from = 0;           // the promoted candidate's position before promotion
  float margin = 0.0f;    // challenger minus incumbent, normalised. Stored so the
                          // threshold can be re-swept against real selections
                          // without a code change.
  int n_scored = 0;       // how many candidates were actually scored
  int64_t us = 0;         // scoring time on the input thread -- the same
                          // quantity RerankTrace::score_us measures (the
                          // Score() call alone). Measured p50/p90 came in ~2x
                          // the design's estimate and over its 16ms budget;
                          // this is what settles that in live use rather than
                          // on 20 offline samples.
  std::string skip;       // none|disabled|battery|nomodel|noctx|cold|nohan|margin
};

struct Event {
  std::string ts;                // local time, ISO 8601 with offset
  std::string machine;           // Deployer::user_id
  std::string schema;            // Rime schema id
  std::string src;               // "imk" | "bridge" | "tmux" | "none"
  std::string input;             // the segment's input code
  std::string ctx;               // TrailingCjkRun before the caret; Han only
  int sel_idx = 0;               // selected index in the list as displayed
  std::string sel;               // selected candidate text
  std::vector<std::string> top;  // first telemetry/top_n candidates, in order
  std::optional<RerankRecord> rr;
  std::optional<LlmRecord> llm;
};

// One line per flush interval, aggregating every segment the LLM path looked
// at -- not just the hard cases ShouldRecord keeps. The per-event stream's
// denominator is "cases worth debugging"; warm-hit rate needs "everything",
// since its denominator is ordinary typing that the per-event stream never
// records. That is the whole reason this is a second line type rather than a
// running total bolted onto Event.
struct StatsLine {
  std::string ts;         // local time the window closed
  int64_t segments = 0;   // segments looked at in the window -- the
                          // denominator ShouldRecord throws away, and the one
                          // that makes every rate below meaningful
  int64_t llm_acted = 0;  // segments where the scorer actually ran
                          // (llm_skip == kNone) -- "the engine sees traffic"
                          // as distinct from "the engine is enabled"
  // No separate `warm_hit` field: under the fallback chain as implemented
  // (rerank_filter.cc), kCold is assigned before Score() can ever run and is
  // mutually exclusive with kNone -- there is no path where a segment is
  // scored while not warm. A stored counter here would always equal
  // `llm_acted` and could not refute anything a duplicate of `llm_acted`
  // doesn't already refute. Warm-hit rate is therefore
  // `llm_acted / (llm_acted + skip_counts["cold"])`, computed by the
  // analyser from the two numbers that do carry independent information.
  std::map<std::string, int64_t> skip_counts;  // SkipReasonName() -> count,
                                               // over every segment, so a
                                               // spike in one reason (e.g.
                                               // "battery") is visible
                                               // without re-deriving it from
                                               // the per-event lines
  double us_p50 = 0.0;                         // Score() latency percentiles, the same quantity
  double us_p95 = 0.0;                         // LlmRecord::us measures -- p95 rather than the p90
                                               // the offline measurement used, since a live stats
                                               // line should watch the worse tail, not repeat it
};

// Whether this segment is worth a line at all.
//
// `promoted` is "a re-ranker moved a candidate" -- db or LLM, either counts --
// not "re-ranking ran": a run that found no promotion and left the user
// taking the first candidate is ordinary typing. The caller is responsible
// for OR-ing both re-rankers' outcomes into this one bool; an LLM-only
// promotion that the caller forgot to fold in here is dropped silently, which
// is exactly the gap that made every LLM-only event invisible before this was
// wired up.
inline bool ShouldRecord(int sel_idx, bool promoted) { return promoted || sel_idx != 0; }

// nlohmann's number_float_t is double, so assigning a `float` promotes it
// exactly -- including every bit of binary32 rounding noise (3.4f becomes
// 3.4000000953674316 once widened), which the shortest-round-trip dumper then
// spells out in full. Round to 4 decimal digits, more precision than a
// normalised logprob difference carries anyway, so the file states what the
// number actually is rather than what float's storage happens to be.
inline double RoundFloat(float v) { return std::round(static_cast<double>(v) * 10000.0) / 10000.0; }

// One JSONL line, WITHOUT the trailing newline — the writer appends that.
inline std::string SerializeJsonl(const Event& e) {
  // ordered_json, not json: the latter is backed by std::map and would sort the
  // keys alphabetically, making the file and these tests harder to read.
  nlohmann::ordered_json j;
  j["v"] = kSchemaVersion;
  j["ts"] = e.ts;
  j["machine"] = e.machine;
  j["schema"] = e.schema;
  j["src"] = e.src;
  j["input"] = e.input;
  j["ctx"] = e.ctx;
  j["sel_idx"] = e.sel_idx;
  j["sel"] = e.sel;
  j["top"] = e.top;
  if (e.rr) {
    nlohmann::ordered_json r;
    r["key"] = e.rr->key;
    r["key_len"] = e.rr->key_len;
    r["n"] = e.rr->n;
    r["text"] = e.rr->text;
    r["from"] = e.rr->from;
    r["rank"] = e.rr->rank;
    r["level"] = e.rr->level;
    j["rr"] = std::move(r);
  }
  if (e.llm) {
    nlohmann::ordered_json l;
    l["text"] = e.llm->text;
    l["incumbent"] = e.llm->incumbent;
    l["from"] = e.llm->from;
    l["margin"] = RoundFloat(e.llm->margin);
    l["n_scored"] = e.llm->n_scored;
    l["us"] = e.llm->us;
    l["skip"] = e.llm->skip;
    j["llm"] = std::move(l);
  }
  // dump() defaults to ensure_ascii=false, which keeps Chinese literal rather
  // than escaping it to \uXXXX. Keep that default. It also escapes newlines
  // inside strings, which is what keeps one event on one line.
  return j.dump();
}

// One JSONL line for a flush interval's aggregate counters. `type` is what a
// reader keys on: an Event line has no `type` field at all (v1 files predate
// the concept and must keep loading unchanged), so "type" == "stats" is what
// tells the two apart without touching Event's shape.
inline std::string SerializeStatsJsonl(const StatsLine& s) {
  nlohmann::ordered_json j;
  j["v"] = kSchemaVersion;
  j["type"] = "stats";
  j["ts"] = s.ts;
  j["segments"] = s.segments;
  j["llm_acted"] = s.llm_acted;
  nlohmann::ordered_json skip;
  for (const auto& [reason, count] : s.skip_counts) {
    skip[reason] = count;
  }
  j["skip_counts"] = std::move(skip);
  j["us_p50"] = s.us_p50;
  j["us_p95"] = s.us_p95;
  return j.dump();
}

}  // namespace telemetry
}  // namespace rime
