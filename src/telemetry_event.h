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
// v3 adds `best`/`best_from`/`dropped` to LlmRecord and `llm_skip` to Event,
// and stops emitting an `llm` object for a segment that was never scored (the
// kNone-without-scoring path fixed in rerank_filter.cc). A v2 line carries
// none of those and must keep loading unchanged under a v3 reader.
//
// v4 adds `before_depth` and `trunc` to Event: how many characters the
// surrounding source actually returned, and why it stopped. Both are omitted on
// a line whose trace carried no measurement, so a v3 reader loses nothing and a
// v4 reader can tell "not measured" from "measured zero". A v3 line carries
// neither and must keep loading unchanged under a v4 reader.
//
// v5 adds `trunc_counts` and `depth_p50`/`depth_p95` to StatsLine: the same
// two facts v4 put on Event, but over EVERY segment rather than the sampled
// subset ShouldRecord keeps. v4's Event fields are unchanged and still the
// only place either fact can be joined back to the segment it came from. A
// v4 line carries neither and must keep loading unchanged under a v5 reader.
//
// v6 adds `fetch_chars` to StatsLine, and unlike every bump above it exists
// because an EXISTING field changed meaning. `copilot/rerank/llm/context_chars`
// became a term in the fetch depth on 2026-08-28 (SurroundingPrefixChars,
// surrounding_source.h); before that the sources stopped at 8 whatever the
// schema said. So `trunc_counts["config"]` counted fetches cut at 8, and now
// counts fetches cut at whatever the max resolves to -- two different
// measurements under one key, and nothing in the line said which. Pooling them
// would answer "is the cap binding?" over a cap that changed mid-file, which is
// the class of merge this file's own comments keep warning about. A v5 line
// carries no `fetch_chars` and must keep loading unchanged. Absent does NOT
// mean 8: it means the line predates the wiring, when the depth was
// max(surrounding_context_chars, max_context_chars) -- 8 under the shipped
// schema, but a reader cannot know that machine's schema and must not assume
// it. Report absent as unrecorded, and never pool it with a recorded depth.
//
// v7 adds `lock_us`, `work_us` and `n_decoded` to Event's `llm` object,
// splitting the existing `us` into the two halves that have opposite fixes and
// recording whether the batch decoded at all. `us` is unchanged and still the
// whole Score() call, so a v6 reader loads a v7 line without noticing. A v6
// line carries none of the three, and absent must be read as "not measured" --
// in particular `n_decoded` absent is NOT 0, which is a real and common
// measurement (see LlmRecord).
inline constexpr int kSchemaVersion = 7;

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
  std::string text;                  // the promoted candidate, empty when nothing moved
  std::string incumbent;             // the first all-Han candidate it was compared against.
                                     // Without this the "never touch bucket B" invariant
                                     // cannot be verified in production.
  std::string best;                  // the model's top-scoring all-Han candidate, whether or
                                     // not it was promoted. Equal to `incumbent` when the
                                     // model agreed with the head -- the one signal that
                                     // separates a model-quality problem from a threshold
                                     // one, which `margin` alone cannot: margin 0.0 means
                                     // agreement, not a near miss.
  int best_from = 0;                 // its position in the displayed window
  std::vector<std::string> dropped;  // candidates the same-span gate removed
                                     // from the scoring window, capped at
                                     // llm.top_n. Empty when the gate removed
                                     // nothing OR when same_span_only is off;
                                     // those two are the same fact for the
                                     // question this answers.
  int from = 0;                      // the promoted candidate's position before promotion
  float margin = 0.0f;               // challenger minus incumbent, normalised. Stored so the
                                     // threshold can be re-swept against real selections
                                     // without a code change.
  int n_scored = 0;                  // how many candidates were actually scored
  int64_t us = 0;                    // scoring time on the input thread -- the same
                                     // quantity RerankTrace::score_us measures (the
                                     // Score() call alone). Measured p50/p90 came in ~2x
                                     // the design's estimate and over its 16ms budget;
                                     // this is what settles that in live use rather than
                                     // on 20 offline samples.
  // v7. `us` above is the sum of these two plus the little either side of them,
  // and the sum could not answer what it was collected for: live scoring runs
  // p50 11.1ms where bench_scorer reports 2.1ms for the same batch on the same
  // machine, and nothing distinguished "the model is slower in-process" from
  // "the call spent that time waiting for a background prefill to release the
  // model lock". See ScoreTiming (scorer.h) for both hypotheses and why
  // neither was idle. -1, never 0, when the field was not measured: an
  // uncontended lock and an unmeasured one are different findings.
  int64_t lock_us = -1;  // of `us`, the wait for LlmScorer's model mutex
  int64_t work_us = -1;  // of `us`, the part spent holding it
  int n_decoded = -1;    // candidate tokens submitted to llama_decode. 0 is the
                         // common and cheap case, not a missing measurement:
                         // every candidate's first token is scored off the
                         // prefill's own last logits, so a window of
                         // single-token candidates decodes nothing and costs
                         // ~0.18ms against ~11ms for one that decodes.
  std::string skip;      // none|disabled|battery|nomodel|noctx|cold|nohan|margin
};

struct Event {
  std::string ts;                // local time, ISO 8601 with offset
  std::string machine;           // Deployer::user_id
  std::string schema;            // Rime schema id
  std::string src;               // "imk" | "bridge" | "tmux" | "none"
  int before_depth = -1;         // characters the source returned before the caret; -1
                                 // means no trace carried a measurement, which is a
                                 // different fact from a measured 0
  std::string trunc;             // TruncationName(): full|config|app|screen. Empty when
                                 // the source could not say (the bridge, today) or when
                                 // no trace existed.
  std::string input;             // the segment's input code
  std::string ctx;               // TrailingCjkRun before the caret; Han only
  int sel_idx = 0;               // selected index in the list as displayed
  std::string sel;               // selected candidate text
  std::vector<std::string> top;  // first telemetry/top_n candidates, in order
  std::string llm_skip;          // why the model did not engage for this segment, one
                                 // of the eight SkipReasonName values. Empty only when
                                 // no trace existed at all. The stats line's aggregate
                                 // skip_counts cannot be joined back to the segment
                                 // that failed; this can.
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
  // TruncationName() -> count, over every segment whose trace carried one,
  // "unknown" included. Event::trunc says the same thing per segment but is
  // SAMPLED: ShouldRecord keeps every miss and promotion and only 1 in
  // `sample_ok` plain successes, so the share of kByScreen computed from the
  // event stream is biased toward hard cases -- the ones carrying least
  // context. This is the unbiased denominator, and which bucket dominates
  // decides which lever is the right one -- the two are not interchangeable.
  // kByConfig means the source HAD more and `prefix_chars` cut it, so raising
  // that cap recovers real text: 8 -> 64 characters is worth +2.47 points
  // (p = 4.6e-07, the context-length results record). kByScreen means the
  // terminal cannot see further, where raising the cap buys nothing and the
  // only levers -- chrome stripping, scrollback -- have NO measured gain; that
  // same record says in as many words that it "says nothing about chrome".
  // Reading one as the other points the next change at the wrong half.
  // Segments with no trace at all count here in nothing, exactly as they count
  // in no skip bucket, so the sum is <= `segments`.
  //
  // The unit is the segment, not the fetch: one fetch serves every segment of
  // a commit, and the +2.47 was measured per segment, so segment-weighting is
  // what says how much typing a deeper fetch would reach.
  std::map<std::string, int64_t> trunc_counts;
  // How deep the fetch got, over ONLY the fetches the environment cut short
  // -- kByScreen and kByApp. The other three answers carry a depth that is
  // not a limit: kByConfig's is the configured cap (a constant), kFull's is
  // the length of an input region that ended on its own, and kUnknown's is
  // whatever a source that cannot say happened to return. Averaging them
  // together would report a ceiling that is not one, which is the whole
  // reason this is a conditional summary rather than a marginal histogram.
  //
  // Negative means no such fetch was measured in this window -- distinct from
  // a window where the source reached zero characters every time, and the
  // reason SerializeStatsJsonl omits the field rather than writing 0.0. Same
  // convention as Event::before_depth's -1.
  double depth_p50 = -1.0;
  double depth_p95 = -1.0;
  // What `prefix_chars` the surrounding sources were asked for during this
  // window -- SurroundingPrefixChars' result, the max over the declarations of
  // the consumers that are on. It is the denominator `trunc_counts["config"]`
  // is a count against: "the cap bound 71% of the time" means nothing without
  // the cap. Config-fixed, so it does not vary within a window; it is written
  // per line anyway because a log spans redeploys and a reader has no other
  // way to date the change.
  //
  // Negative means not recorded -- a v5 or older line, whose depth was
  // max(surrounding_context_chars, max_context_chars): 8 under the shipped
  // schema, but not knowable from the line. Same omit-rather-than-zero
  // convention as the depth pair, and for the sharper reason: 0 is not a fetch
  // depth any build ever used, so writing it would invent a state.
  int fetch_chars = -1;
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
//
// `sample_ok` > 0 additionally keeps 1 in N of the plain successes -- ordinary
// typing that nothing moved. Those are what an eval set needs and what the
// hard-cases-only gate throws away; without them a set built from this log is
// all failures and every change scores absurdly. `ok_seen` is how many plain
// successes the caller has seen so far, so the sampling is deterministic and
// testable rather than a coin flip.
//
// This does NOT affect first-candidate accuracy: misses are kept
// unconditionally, so that number needs no sampling and no scaling.
inline bool ShouldRecord(int sel_idx, bool promoted, int64_t ok_seen, int sample_ok) {
  if (promoted || sel_idx != 0) {
    return true;
  }
  if (sample_ok <= 0) {
    return false;
  }
  return ok_seen % sample_ok == 0;
}

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
  // Omitted rather than written as -1/"": an absent key is how every other
  // optional field in this line says "no evidence", and a reader that has to
  // special-case a sentinel is a reader that will forget to.
  if (e.before_depth >= 0) {
    j["before_depth"] = e.before_depth;
  }
  if (!e.trunc.empty()) {
    j["trunc"] = e.trunc;
  }
  j["input"] = e.input;
  j["ctx"] = e.ctx;
  j["sel_idx"] = e.sel_idx;
  j["sel"] = e.sel;
  j["top"] = e.top;
  // Omitted rather than written empty: "no trace at all" and "a trace whose
  // reason was none" are different evidence, and "" is not a SkipReasonName.
  if (!e.llm_skip.empty()) {
    j["llm_skip"] = e.llm_skip;
  }
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
    l["best"] = e.llm->best;
    l["best_from"] = e.llm->best_from;
    l["dropped"] = e.llm->dropped;
    l["from"] = e.llm->from;
    l["margin"] = RoundFloat(e.llm->margin);
    l["n_scored"] = e.llm->n_scored;
    l["us"] = e.llm->us;
    // Omitted rather than written as -1 when unmeasured, so a reader sees the
    // same "absent" for a v7 line whose Score() never ran as for a v6 line
    // that could not have measured it -- one rule, not two.
    if (e.llm->lock_us >= 0) {
      l["lock_us"] = e.llm->lock_us;
    }
    if (e.llm->work_us >= 0) {
      l["work_us"] = e.llm->work_us;
    }
    if (e.llm->n_decoded >= 0) {
      l["n_decoded"] = e.llm->n_decoded;
    }
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
  nlohmann::ordered_json trunc;
  for (const auto& [reason, count] : s.trunc_counts) {
    trunc[reason] = count;
  }
  j["trunc_counts"] = std::move(trunc);
  // Omitted, not zeroed, when the window saw no environment-truncated fetch:
  // see StatsLine's field comment. Both move together -- they come from one
  // sample vector, so one present and the other absent is not a state.
  if (s.depth_p50 >= 0.0) {
    j["depth_p50"] = s.depth_p50;
    j["depth_p95"] = s.depth_p95;
  }
  // Omitted rather than written negative, so a reader distinguishes "this build
  // did not record it" from any value. See StatsLine's field comment.
  if (s.fetch_chars >= 0) {
    j["fetch_chars"] = s.fetch_chars;
  }
  return j.dump();
}

}  // namespace telemetry
}  // namespace rime
