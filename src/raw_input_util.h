#pragma once

// Where the raw-keystrokes candidate goes, as a pure decision.
//
// Extracted from RawInputFilterTranslation::Replenish (filters.cc) so the
// ordering can be tested without an engine -- the same reason
// ComputeSpaceCommitText exists. Depends only on std::string / std::vector.

#include <algorithm>
#include <cstddef>
#include <string>
#include <vector>

namespace rime {
namespace raw_input_detail {

// The raw candidate is a memory aid for double pinyin: two keystrokes per
// syllable, so `n` here is how many syllables the user has typed. An odd
// length rounds up -- a trailing single letter is a syllable in progress.
inline size_t SyllableCount(const std::string& input) { return (input.size() + 1) / 2; }

// Below this many syllables the input is short enough to read off the
// preedit, and a raw candidate would only take a slot from a real one.
inline constexpr size_t kMinSyllables = 4;

// `Slot` returns this when the raw candidate does not belong in the list.
inline constexpr size_t kNoSlot = static_cast<size_t>(-1);

// The 0-based position the raw candidate takes among `texts` (the upstream
// candidates, in order), or kNoSlot.
inline size_t Slot(const std::vector<std::string>& texts, const std::string& input, int page_size,
                   size_t min_syllables = kMinSyllables) {
  // A page with one slot has no room that is not the first candidate, and an
  // unset schema reports 0. Neither may displace the head.
  if (page_size < 2) {
    return kNoSlot;
  }
  if (SyllableCount(input) < min_syllables) {
    return kNoSlot;
  }
  // The last slot of the page, or the end of a list too short to fill one.
  const size_t slot = std::min(static_cast<size_t>(page_size) - 1, texts.size());
  // Only what the user sees beside it counts as a duplicate: a candidate on
  // the next page does not make this page's reminder redundant.
  for (size_t i = 0; i < slot; ++i) {
    if (texts[i] == input) {
      return kNoSlot;
    }
  }
  return slot;
}

}  // namespace raw_input_detail
}  // namespace rime
