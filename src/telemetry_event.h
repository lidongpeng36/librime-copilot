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

#include <optional>
#include <string>
#include <vector>

namespace rime {
namespace telemetry {

// Bumped whenever a field changes meaning, so a reader can tell old lines from
// new ones. Files outlive the code that wrote them.
inline constexpr int kSchemaVersion = 1;

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
};

// Whether this segment is worth a line at all.
//
// `promoted` is "re-ranking moved a candidate", not "re-ranking ran": a run
// that found no promotion and left the user taking the first candidate is
// ordinary typing.
inline bool ShouldRecord(int sel_idx, bool promoted) { return promoted || sel_idx != 0; }

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
  // dump() defaults to ensure_ascii=false, which keeps Chinese literal rather
  // than escaping it to \uXXXX. Keep that default. It also escapes newlines
  // inside strings, which is what keeps one event on one line.
  return j.dump();
}

}  // namespace telemetry
}  // namespace rime
