#pragma once

// The decision: given scored candidates, which one (if any) to lift to first.
//
// Free of Rime and of llama.cpp, so test/rerank_llm_test.cc drives it directly.
// Every constant here was measured over 8324 segments of real typing; see
// docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md. Do not tune them
// by intuition -- the offline harness re-measures any change in minutes.

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

#include "auto_spacer_util.h"  // Utf8ToCodepoint
#include "history.h"           // copilot::UTF8
#include "rerank.h"            // rerank_detail::IsHanIdeograph

namespace rime {

struct LlmRerankOptions {
  bool enable = false;  // off by default: ~1 GB resident and continuous CPU
  std::string model;
  bool battery_active = false;
  int top_n = 4;        // 4 beats 32 on accuracy AND speed; see the spec
  float margin = 2.0f;  // cuts harmful false promotion to ~a quarter
  float length_exponent = 0.7f;
  // Characters of context handed to the model, independent of the db's
  // max_context_chars (8, sized for n-gram keys). Larger because an LM uses
  // what an n-gram cannot, and bounded because prefill cost scales with it:
  // measured ~0.03 ms per token on this model, so 32 costs about 1 ms more
  // than 8.
  int context_chars = 32;
  // Whether a segment needs a trailing HAN context before the model may be
  // consulted. True is today's behaviour and is a db constraint the model does
  // not share -- see BailOnEmptyDbContext below for what it costs and why
  // lifting it is a measurement rather than an obvious win.
  bool require_han_context = true;
  // v10: the word-head prior (wordhead_table.h). 0 is inert, and is the code
  // default so nothing changes for a schema that does not opt in; the shipped
  // schema patch sets 1.0 -- the same split `margin` has. Measured with the
  // table built from the user's own corpus: 1.0 picked on the earlier half of
  // 23 days, +145 against +120 net keystrokes on the held-out later half.
  float word_head_weight = 0.0f;
  // Relative to the Rime user directory, resolved like the model path.
  std::string word_head_table = "private/wordhead.txt";
};

namespace llm_rerank {

// Why re-ranking did not promote anything. "Declined" and "never ran" are
// different outcomes and telemetry has to tell them apart, or "the model judged
// this should not move" reads identically to "the model was never consulted".
enum class SkipReason {
  kNone = 0,   // something was promoted
  kDisabled,   // rerank/llm/enable is false
  kBattery,    // on battery and battery_active is false
  kNoModel,    // model missing or failed to load
  kNoContext,  // nothing at all before the caret to score against
  kNoSource,   // no surrounding-text source answered
  kCold,       // warm cache miss -- the one reason only live use can measure
  kNoHan,      // no all-Han candidate in the window
  kMargin,     // the best challenger did not beat the incumbent by `margin`
};

// The exact strings telemetry writes. Defined beside the enum so the two
// cannot drift; test/rerank_llm_test.cc pins every enumerator.
inline const char* SkipReasonName(SkipReason r) {
  switch (r) {
    case SkipReason::kNone:
      return "none";
    case SkipReason::kDisabled:
      return "disabled";
    case SkipReason::kBattery:
      return "battery";
    case SkipReason::kNoModel:
      return "nomodel";
    case SkipReason::kNoContext:
      return "noctx";
    case SkipReason::kNoSource:
      return "nosource";
    case SkipReason::kCold:
      return "cold";
    case SkipReason::kNoHan:
      return "nohan";
    case SkipReason::kMargin:
      return "margin";
  }
  return "none";
}

// Orthogonal to the ordered fallback chain: a cold model can ALSO have a
// non-Han context gate. Record both without changing any ranking decision.
inline const char* ContextGate(bool source_available, bool llm_empty, bool han_empty,
                               bool require_han) {
  if (!source_available) return "unavailable";
  if (llm_empty) return "empty";
  if (han_empty && require_han) return "non_han";
  return "clear";
}

// The reason to record when the db-context gate (rerank_filter.cc's
// `context.empty()` early return) ends a segment before anything is scored.
//
// The Han-only db context can be empty while the LLM's own context is not --
// the user typing after Latin, punctuation or a line start -- so the fallback
// chain may well have left kNone behind, and kNone means "the model ran".
// kNoContext is the honest name: this IS the gate CLAUDE.md's "What is NOT
// built" describes, and naming it here is what finally makes `noctx`
// countable (rerank_filter.cc:290 notes the design doc named a bucket the
// code could never produce).
inline SkipReason SkipForEmptyDbContext(SkipReason chain_reason) {
  return chain_reason == SkipReason::kNone ? SkipReason::kNoContext : chain_reason;
}

// The db-context gate: should CopilotRerankFilter::Apply end this segment?
//
// `db_context_empty` means the trailing Han run is empty, which is the only
// thing the db can key on. The MODEL has no such need -- it reads punctuation
// and Latin -- so this gate costs it real segments.
//
// HOW MANY was re-measured on 2026-08-21 over 16322 requests / 27245 segments
// (docs/superpowers/specs/2026-08-21-rerank-cost-and-gate-results.md): the gate
// blocks 42.9% of all segments, and `noctx` is 16322 of 27245 (59.9%) --
// exactly the request count, because the gate ends every request's segment 0
// and nothing else. Any figure from before that date is unusable rather than
// approximate: the replay harness's trace detector saturated at 16 entries, so
// its surviving sample was biased toward short requests.
//
// Lifting it was tried once and reverted: those newly-eligible segments were
// exactly the ones where RawInputFilter had put the raw keystrokes first, and
// promoting into them displaced that head -- bucket B+D fell from 43.8% to
// 13.4%. `Decide` enforces "never touch bucket B" when choosing WHICH candidate
// wins, but nothing enforces it on where the winner is INSERTED.
//
// That danger is gone and the question is now settled -- on cost, not fear.
// RawInputFilter no longer puts the raw keystrokes first in any segment
// (b64ef1c -- they go to the last slot of the page, or nowhere), and bucket B
// measured 4 segments of 27245 in BOTH arms. Lifting the gate is safe, and it
// changes what the user sees on 75 of 27245 segments (GAIN 44 / LOSS 31,
// McNemar p=0.17) for 2.07x the scorings. So it stays a switch, defaulting to
// true because 2.07x the model runs buys nothing measurable -- not because the
// answer is unknown.
//
// The `!llm_eligible` clause is not redundant with the caller's later check:
// with the gate lifted and no eligible model there is nothing either path can
// act on, and falling through would reach LookupContinuations("") -- a db query
// on an empty key whose answer is already known.
inline bool BailOnEmptyDbContext(bool db_context_empty, bool llm_eligible,
                                 bool require_han_context) {
  if (!db_context_empty) {
    return false;
  }
  return require_han_context || !llm_eligible;
}

struct Decision {
  int promote_index = -1;  // index into `candidates`; -1 means leave the order alone
  // The best-scoring all-Han candidate, set whether or not it was promoted.
  // `best_index == incumbent_index` is the model AGREEING with the head, not a
  // degenerate value: it is the only signal that separates a model-quality
  // problem (lowering `margin` cannot help) from a threshold problem (lowering
  // it recovers the segment). -1 only when there is no all-Han candidate.
  int best_index = -1;
  SkipReason skip = SkipReason::kNoHan;
  int incumbent_index = -1;  // the first all-Han candidate
  float margin = 0.0f;       // challenger score minus incumbent score
  int n_scored = 0;
  // v10. How much the word-head prior moved `margin`: this decision's margin
  // minus the margin the same call reports with no priors. So
  // `margin - prior_delta` is the pre-v10 quantity even when the prior changed
  // which candidate is `best`. 0 when no prior was applied.
  float prior_delta = 0.0f;
  // Whether the prior changed which candidate (if any) is promoted, in either
  // direction. telemetry_commit.cc records these segments in full: a
  // prior-blocked promotion otherwise looks like a plain success and is
  // sampled 1 in sample_ok -- the one-sided count CLAUDE.md records under
  // `margin`.
  bool prior_changed_verdict = false;
  // What the same call promotes with no priors: `promote_index` of the
  // no-prior DecideImpl. Set only when priors were given, -1 otherwise (and
  // -1 when the no-prior call would have promoted nothing). `best_index` is
  // the post-prior pick, so when the prior BLOCKS a promotion this is the
  // only record of the challenger it blocked.
  int promote_index_noprior = -1;
};

// Every codepoint is a Han ideograph. Deliberately strict: the raw-input
// candidate ("guyide"), mixed forms and CJK punctuation are all excluded, so
// none of them can be promoted or displaced.
inline bool IsAllHan(const std::string& text) {
  if (text.empty()) {
    return false;
  }
  ::copilot::UTF8 utf8(text);
  const int n = static_cast<int>(utf8.size());
  for (int i = 0; i < n; ++i) {
    if (!rerank_detail::IsHanIdeograph(auto_spacer_detail::Utf8ToCodepoint(std::string(utf8[i])))) {
      return false;
    }
  }
  return true;
}

inline float NormalizedScore(float logprob, int n_tokens, float exponent) {
  const int n = n_tokens > 0 ? n_tokens : 1;
  return logprob / std::pow(static_cast<float>(n), exponent);
}

inline float PriorAt(const std::vector<float>& priors, int i) {
  return i >= 0 && i < static_cast<int>(priors.size()) ? priors[i] : 0.0f;
}

namespace detail {

inline Decision DecideImpl(const std::vector<std::string>& candidates,
                           const std::vector<float>& raw_logprobs, const std::vector<int>& n_tokens,
                           const std::vector<float>& priors, const LlmRerankOptions& options) {
  Decision d;
  const int limit =
      std::min<int>(static_cast<int>(candidates.size()), options.top_n > 0 ? options.top_n : 0);
  d.n_scored = std::min<int>(
      {limit, static_cast<int>(raw_logprobs.size()), static_cast<int>(n_tokens.size())});

  int best = -1;
  float best_score = 0.0f;
  for (int i = 0; i < d.n_scored; ++i) {
    if (!IsAllHan(candidates[i])) {
      continue;  // never promote the raw input or a mixed candidate
    }
    if (d.incumbent_index < 0) {
      d.incumbent_index = i;  // first all-Han candidate wins the incumbency
    }
    const float s =
        NormalizedScore(raw_logprobs[i], n_tokens[i], options.length_exponent) + PriorAt(priors, i);
    if (best < 0 || s > best_score) {
      best = i;
      best_score = s;
    }
  }

  d.best_index = best;
  if (d.incumbent_index < 0) {
    d.skip = SkipReason::kNoHan;
    return d;
  }
  const float incumbent_score =
      NormalizedScore(raw_logprobs[d.incumbent_index], n_tokens[d.incumbent_index],
                      options.length_exponent) +
      PriorAt(priors, d.incumbent_index);
  d.margin = best_score - incumbent_score;

  if (best == d.incumbent_index || d.margin < options.margin) {
    d.skip = SkipReason::kMargin;
    return d;
  }
  d.promote_index = best;
  d.skip = SkipReason::kNone;
  return d;
}

}  // namespace detail

// `raw_logprobs`, `n_tokens` and `priors` are parallel to `candidates`;
// `priors` may be shorter (missing entries are 0) or empty (no prior at all --
// exactly the pre-v10 decision). The prior is added AFTER length
// normalisation; see wordhead_table.h for where the values come from.
inline Decision Decide(const std::vector<std::string>& candidates,
                       const std::vector<float>& raw_logprobs, const std::vector<int>& n_tokens,
                       const std::vector<float>& priors, const LlmRerankOptions& options) {
  Decision d = detail::DecideImpl(candidates, raw_logprobs, n_tokens, priors, options);
  if (priors.empty()) {
    return d;
  }
  const Decision base = detail::DecideImpl(candidates, raw_logprobs, n_tokens, {}, options);
  d.prior_delta = d.margin - base.margin;
  d.prior_changed_verdict = base.promote_index != d.promote_index;
  d.promote_index_noprior = base.promote_index;
  return d;
}

inline Decision Decide(const std::vector<std::string>& candidates,
                       const std::vector<float>& raw_logprobs, const std::vector<int>& n_tokens,
                       const LlmRerankOptions& options) {
  return Decide(candidates, raw_logprobs, n_tokens, std::vector<float>{}, options);
}

}  // namespace llm_rerank
}  // namespace rime
