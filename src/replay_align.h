#pragma once

// Pure alignment helpers for the replay harness (tools/replay_copilot.cc).
//
// Free of Rime types so they can be unit-tested directly
// (test/replay_align_test.cc) -- the same split rerank.h and
// auto_spacer_util.h use. Nothing in the plugin's module registration
// references this header; it lives under src/ only because copilot_test's
// include path does not reach tools/.

#include <string>
#include <vector>

#include "history.h"  // copilot::UTF8

namespace rime {
namespace replay {

// The characters of `text` in [begin, end) -- HALF-OPEN, by codepoint.
//
// Two properties of copilot::UTF8 make the conversion below non-obvious, and
// both were checked against history.cc:138 rather than assumed:
//
//   1. UTF8::operator()(start, end) is a CLOSED interval [start, end], not a
//      half-open one. Hence `end - 1`. (rerank.h:56's `utf8(n - taken, -1)`
//      reads naturally once you know this: -1 is the last character, not a
//      sentinel.)
//   2. It clamps with std::clamp(start, 0, n - 1). For an empty string n is 0,
//      so that is clamp(x, 0, -1) -- lo > hi, which is undefined behaviour.
//      The empty guard below must therefore come BEFORE any call into it.
//
// Out-of-range indices clamp rather than throw: a malformed segment must
// degrade to an empty ground truth, which the caller records as a miss, rather
// than crash a run of thousands of samples.
inline std::string SliceChars(const std::string& text, int begin, int end) {
  if (text.empty()) {
    return {};
  }
  ::copilot::UTF8 utf8(text);
  const int n = static_cast<int>(utf8.size());
  if (n == 0) {
    return {};
  }
  begin = begin < 0 ? 0 : (begin > n ? n : begin);
  end = end < 0 ? 0 : (end > n ? n : end);
  if (begin >= end) {
    return {};
  }
  // Half-open [begin, end) -> closed [begin, end - 1].
  return std::string(utf8(begin, end - 1));
}

// The result of matching a candidate list against the still-unconsumed
// remainder of a request's ground-truth text. hit == -1 (with want empty and
// chars == 0) is the miss sentinel.
struct PrefixMatch {
  int hit = -1;
  std::string want;
  // Codepoint count of `want` -- NOT its byte length. The replayer's `span`
  // is in KEYS, at two keys per Han character (双拼), so a caller that used
  // want.size() (bytes) here would desynchronize every span after the first
  // multi-byte match. Left at 0 on a miss, matching `want`'s emptiness.
  int chars = 0;
};

// The candidate in `cands` that is the LONGEST byte-prefix of `remaining`; on
// a tie (two candidates of equal length both matching -- only possible if the
// candidate list itself contains a duplicate string), the lowest index, since
// the loop only replaces the current best on a STRICT length increase.
//
// This is what a real user does -- they take the longest candidate that
// already says what they meant -- and it is also what makes `want`/`hit`
// meaningful: matching one fixed-length character at a time against a
// candidate list that can offer a multi-character phrase manufactures
// "opportunities" out of segments where the correct, longer answer was
// already first (see task-5-report.md's "Critical 1" writeup for a worked
// example: guyide -> 故意的 came back as three fabricated one-character
// segments instead of one correct three-character one).
//
// Byte-prefix comparison is exact here, not an approximation: every
// candidate string is a whole number of complete UTF-8 characters (it came
// out of Rime's own candidate list), so a byte-for-byte prefix match can only
// ever land on a character boundary of `remaining` too -- no UTF8 decoding
// is needed for the matching step itself, only for the `chars` count
// afterward.
inline PrefixMatch FindLongestPrefixMatch(const std::vector<std::string>& cands,
                                          const std::string& remaining) {
  PrefixMatch best;
  for (size_t i = 0; i < cands.size(); ++i) {
    const std::string& c = cands[i];
    if (c.empty() || c.size() > remaining.size()) {
      continue;
    }
    if (remaining.compare(0, c.size(), c) == 0 && c.size() > best.want.size()) {
      best.hit = static_cast<int>(i);
      best.want = c;
    }
  }
  if (best.hit >= 0) {
    // best.want is non-empty whenever hit >= 0 (empty candidates are skipped
    // above), so this never hits copilot::UTF8's empty-string UB (see
    // SliceChars's comment above for that trap).
    best.chars = static_cast<int>(::copilot::UTF8(best.want).size());
  }
  return best;
}

}  // namespace replay
}  // namespace rime
