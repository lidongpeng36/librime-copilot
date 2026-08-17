#include <gtest/gtest.h>

#include "replay_align.h"

using namespace rime::replay;

TEST(SliceChars, SlicesByCharacterNotByte) {
  EXPECT_EQ(SliceChars("故意的", 0, 2), "故意");
  EXPECT_EQ(SliceChars("故意的", 2, 3), "的");
  EXPECT_EQ(SliceChars("故意的", 0, 3), "故意的");
}

TEST(SliceChars, ClampsOutOfRange) {
  EXPECT_EQ(SliceChars("故意的", 2, 99), "的");
  EXPECT_EQ(SliceChars("故意的", 5, 9), "");
  EXPECT_EQ(SliceChars("故意的", 2, 1), "");
}

// copilot::UTF8 clamps with std::clamp(start, 0, n - 1); for an empty string
// that is clamp(x, 0, -1), which is undefined behaviour. SliceChars must not
// reach it.
TEST(SliceChars, HandlesEmptyText) {
  EXPECT_EQ(SliceChars("", 0, 0), "");
  EXPECT_EQ(SliceChars("", 0, 3), "");
}

// The interval is half-open, so the last character is reachable only as
// [n-1, n) -- the boundary the closed-interval conversion is most likely to
// get wrong.
TEST(SliceChars, IncludesTheLastCharacter) {
  EXPECT_EQ(SliceChars("故意的", 2, 3), "的");
  EXPECT_EQ(SliceChars("故意的", 1, 3), "意的");
}

// This is the function that produced the 67%-where-truth-is-0% error: a
// fixed one-syllable stride matched a one-character `want` against a
// candidate list that could offer the whole remaining phrase, manufacturing
// "opportunities" out of segments that were already correct. See
// task-5-report.md's "Critical 1" writeup.
TEST(FindLongestPrefixMatch, PicksTheLongestPrefixOverAnyShorterOne) {
  std::vector<std::string> cands{"故", "故意", "故意的"};
  const PrefixMatch match = FindLongestPrefixMatch(cands, "故意的");
  EXPECT_EQ(match.hit, 2);
  EXPECT_EQ(match.want, "故意的");
}

// Only reachable if the candidate list itself contains a duplicate string
// (the same text at two indices) -- the loop only replaces the current best
// on a STRICT length increase, so the first occurrence wins.
TEST(FindLongestPrefixMatch, TiesBreakToTheLowestIndex) {
  std::vector<std::string> cands{"故意的", "故意的"};
  const PrefixMatch match = FindLongestPrefixMatch(cands, "故意的");
  EXPECT_EQ(match.hit, 0);
}

TEST(FindLongestPrefixMatch, RejectsACandidateLongerThanWhatsLeft) {
  std::vector<std::string> cands{"故意的的"};
  const PrefixMatch match = FindLongestPrefixMatch(cands, "故意的");
  EXPECT_EQ(match.hit, -1);
}

// The replayer's `span` is in KEYS, at two keys per Han character, so `chars`
// must be a codepoint count -- a byte count here (9, for this 3-character,
// 3-bytes-per-character string) would desynchronize every span after the
// first multi-byte match.
TEST(FindLongestPrefixMatch, ReturnsCodepointCountNotByteLength) {
  std::vector<std::string> cands{"故意的"};
  const PrefixMatch match = FindLongestPrefixMatch(cands, "故意的");
  EXPECT_EQ(match.chars, 3);
}

TEST(FindLongestPrefixMatch, ReturnsTheMissSentinelWhenNothingMatches) {
  std::vector<std::string> cands{"顾忌", "估计"};
  const PrefixMatch miss = FindLongestPrefixMatch(cands, "故意");
  EXPECT_EQ(miss.hit, -1);
  EXPECT_EQ(miss.want, "");
  EXPECT_EQ(miss.chars, 0);

  const PrefixMatch empty_list = FindLongestPrefixMatch({}, "故意");
  EXPECT_EQ(empty_list.hit, -1);
}
