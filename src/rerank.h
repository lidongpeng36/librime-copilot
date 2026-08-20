#pragma once

// Pure decisions behind contextual candidate re-ranking (see rerank_filter.h).
//
// Three steps, all free of any engine/session state so they can be
// unit-tested directly (test/rerank_test.cc) without standing up Rime:
//   1. ConfirmedPrefix — composition's earlier segments -> text confirmed so far
//   2. TrailingCjkRun  — raw text before the caret      -> usable n-gram context
//   3. PickPromotion   — context's continuations        -> which candidate to lift
//
// ConfirmedPrefix does need real Rime types (Composition/Segment), which is
// heavier than the other two need — that's intentional, it's what lets a test
// drive it without an engine.

#include <rime/candidate.h>
#include <rime/composition.h>

#include <string>
#include <vector>

#include "auto_spacer_util.h"  // Utf8ToCodepoint
#include "history.h"           // copilot::UTF8
#include "provider.h"          // copilot::Entry

namespace rime {

namespace rerank_detail {

// Han ideographs only. Everything else — CJK punctuation (U+3000-303F),
// fullwidth forms (U+FF00-FFEF), latin, digits, spaces, emoji — is a context
// boundary, so one predicate covers every separator case.
inline bool IsHanIdeograph(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) ||  // CJK Unified Ideographs
         (cp >= 0x3400 && cp <= 0x4DBF) ||  // Extension A
         (cp >= 0xF900 && cp <= 0xFAFF) ||  // Compatibility Ideographs
         (cp >= 0x20000 && cp <= 0x2FA1F);  // Extensions B+ and Compat Supplement
}

}  // namespace rerank_detail

// The text the LLM scorer conditions on: the last `max_chars` characters
// before the caret, whatever they are.
//
// Deliberately NOT TrailingCjkRun. That trims to a Han-only tail because the
// db is keyed by Han sequences and can look up nothing else -- a constraint of
// the n-gram, not of the caret. Applying it to the model gated scoring on
// "does the text before the caret end in Han", and measured on the replay
// corpus that is false for 68.2% of segments (`llm_skip=noctx`), so the model
// was consulted on 8.8% of them.
//
// The model has no such constraint: its training corpus keeps punctuation,
// Latin and digits precisely because 0% of real scoring contexts end in a Han
// character (2026-08-19-corpus-pipeline-design.md). Feeding it "好的, " is
// exactly what it was built for.
//
// Lives here, and is called by every site that needs it, because THREE
// places compute this string -- the filter, Copilot's warm trigger
// (copilot.cc) and the replay harness -- and the warm cache is keyed by it.
// Two of them disagreeing does not fail loudly; it makes every warm land on a
// context nobody asks about, so every Apply() finds the cache cold and the
// feature silently never runs.
inline std::string ScoringContext(const std::string& text, int max_chars) {
  if (text.empty() || max_chars <= 0) {
    return {};
  }
  ::copilot::UTF8 utf8(text);
  const int n = static_cast<int>(utf8.size());
  const int take = std::min(n, max_chars);
  std::string out;
  for (int i = n - take; i < n; ++i) {
    out += std::string(utf8[i]);
  }
  return out;
}

// Which positions in a candidate window a promotion may consider, by input
// span. Extracted from the two places in rerank_filter.cc that used to
// duplicate it -- the LLM branch and the db branch -- because a filter the
// two branches could disagree about is a filter with two behaviours, and
// because the switch it now carries needs coverage that standing up a Rime
// engine cannot give.
//
// `same_span_only` true keeps only candidates covering exactly as much input
// as the window head, which is what this filter has always done: promoting
// across spans changes how much input Space commits. False keeps everything,
// which is worth +13.9% net offline and is a question about how a promotion
// FEELS that no corpus can settle. See RerankOptions::same_span_only.
inline std::vector<size_t> EligibleBySpan(const std::vector<size_t>& ends, bool same_span_only) {
  std::vector<size_t> eligible;
  eligible.reserve(ends.size());
  if (ends.empty()) {
    return eligible;
  }
  const size_t head_end = ends.front();
  for (size_t i = 0; i < ends.size(); ++i) {
    if (!same_span_only || ends[i] == head_end) {
      eligible.push_back(i);
    }
  }
  return eligible;
}

// The candidates `EligibleBySpan` left out, capped at `cap` entries.
//
// `eligible` holds positions into `texts`, ascending, as EligibleBySpan
// returns them. Recorded for telemetry only: `sel in dropped` is what says how
// often `same_span_only` had any chance to change the outcome. They are NOT
// scored -- doing so would double the per-segment model cost, which is the
// resource the switch exists to protect.
inline std::vector<std::string> DroppedBySpan(const std::vector<std::string>& texts,
                                              const std::vector<size_t>& eligible, int cap) {
  std::vector<std::string> dropped;
  if (cap <= 0) {
    return dropped;
  }
  std::vector<bool> keep(texts.size(), false);
  for (size_t i : eligible) {
    if (i < keep.size()) {
      keep[i] = true;
    }
  }
  for (size_t i = 0; i < texts.size() && dropped.size() < static_cast<size_t>(cap); ++i) {
    if (!keep[i]) {
      dropped.push_back(texts[i]);
    }
  }
  return dropped;
}

// The run of Han characters immediately before the caret, at most `max_chars`
// long. Empty when the caret does not sit right after a Han character.
//
// The db is keyed by Han sequences, so anything else ends the context: after
// "高屋建。" or "高屋建 " there is nothing to continue, and "see 高屋建" still
// yields 高屋建.
inline std::string TrailingCjkRun(const std::string& text, int max_chars) {
  if (text.empty() || max_chars <= 0) {
    return {};
  }
  ::copilot::UTF8 utf8(text);
  const int n = static_cast<int>(utf8.size());
  int taken = 0;
  for (int i = n - 1; i >= 0 && taken < max_chars; --i) {
    std::string ch(utf8[i]);
    if (!rerank_detail::IsHanIdeograph(auto_spacer_detail::Utf8ToCodepoint(ch))) {
      break;
    }
    ++taken;
  }
  if (taken == 0) {
    return {};
  }
  return std::string(utf8(n - taken, -1));
}

// The text of the composition's segments already confirmed before `current` —
// i.e. what the user selected earlier in the SAME still-uncommitted
// composition (this 顺序 in "这个|顺序" once 这个 has been picked).
// `surrounding->before` cannot see this: it is real application text, and
// nothing has been committed to the application yet. Left out, the re-ranker
// keys on stale context for every segment but the first — measured on a real
// corpus, roughly 1500 of 3819 segments (39%) are the second or later segment
// of their composition.
//
// `current == nullptr` means no segment could safely be identified as
// "current" (see AppliesToSegment/Apply in rerank_filter.cc for when that
// happens). With nothing to stop the walk at, there is no way to tell how
// much of the composition counts as "before" it, so this returns no prefix
// rather than guess — the same "drop rather than misattribute" call already
// made for pending_trace_span_ there.
inline std::string ConfirmedPrefix(const Composition& composition, const Segment* current) {
  if (!current) {
    return {};
  }
  std::string prefix;
  for (const Segment& seg : composition) {
    if (&seg == current) {
      break;
    }
    if (seg.status < Segment::kSelected) {
      // An unselected segment before `current` means the user has not
      // actually confirmed anything past it — Rime lets input segments be
      // revisited while still composing, so treating a later segment's
      // selection as settled would fabricate context that isn't there yet.
      break;
    }
    if (auto candidate = seg.GetSelectedCandidate()) {
      prefix += candidate->text();
    }
  }
  return prefix;
}

// Which candidate to promote, and how well the db ranks it.
struct Promotion {
  int index = -1;  // index into the candidate texts; -1 = leave the order alone
  int rank = 0;    // 1-based position among the key's continuations, by weight
  int level = 0;   // match quality: 3 exact, 2 candidate-prefixes-continuation,
                   // 1 continuation-prefixes-candidate, 0 no match. Reported so
                   // the telemetry can test whether level 3 dominating is
                   // harmful; PickPromotion's own logic is unchanged.
};

// Pick the candidate best supported by `continuations` (the db's successors for
// one context key).
//
// A candidate matches when it equals a continuation, starts one (高屋 -> 建瓴
// while only 建 has been typed), or is started by one (the db knows 建, the
// list offers 建瓴之势). Match quality outranks likelihood, so an exact match
// beats a likelier partial one.
//
// The quality floor is the winner's RANK among the key's continuations, not its
// share of their total weight: dictionaries are merged on different scales (a
// personal dictionary is deliberately lifted above the frequency-bearing one),
// which makes weight ratios meaningless while leaving the order intact. A key
// like 建 has thousands of continuations, so a share-of-total threshold could
// never be satisfied either.
inline Promotion PickPromotion(const std::vector<std::string>& candidate_texts,
                               const std::vector<::copilot::Entry>& continuations, int max_rank) {
  if (candidate_texts.empty() || continuations.empty() || max_rank <= 0) {
    return {};
  }
  int best_level = 0;
  int best_index = -1;
  double best_weight = 0.0;
  for (size_t i = 0; i < candidate_texts.size(); ++i) {
    const std::string& text = candidate_texts[i];
    if (text.empty()) {
      continue;
    }
    for (const auto& entry : continuations) {
      if (entry.weight <= 0.0 || entry.text.empty()) {
        continue;
      }
      int level = 0;
      if (entry.text == text) {
        level = 3;
      } else if (entry.text.size() > text.size() && entry.text.compare(0, text.size(), text) == 0) {
        level = 2;  // candidate starts the continuation
      } else if (text.size() > entry.text.size() &&
                 text.compare(0, entry.text.size(), entry.text) == 0) {
        level = 1;  // continuation starts the candidate
      }
      if (level == 0) {
        continue;
      }
      if (level > best_level || (level == best_level && entry.weight > best_weight)) {
        best_level = level;
        best_index = static_cast<int>(i);
        best_weight = entry.weight;
      }
    }
  }
  if (best_level == 0) {
    return {};
  }
  int rank = 1;
  for (const auto& entry : continuations) {
    if (entry.weight > best_weight) {
      ++rank;
    }
  }
  if (rank > max_rank) {
    return {};
  }
  return Promotion{best_index, rank, best_level};
}

}  // namespace rime
