#pragma once

// Pure decisions behind contextual candidate re-ranking (see rerank_filter.h).
//
// Two steps, both free of Rime types so they can be unit-tested directly
// (test/rerank_test.cc):
//   1. TrailingCjkRun  — raw text before the caret  -> usable n-gram context
//   2. PickPromotion   — context's continuations    -> which candidate to lift

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
