// Where RawInputFilter puts the raw-keystrokes candidate.
//
// The feature is a memory aid for double pinyin: on a long input, show the
// letters actually typed so the user can check what they have entered without
// abandoning the composition. It is supposed to sit at the LAST slot of the
// page, never displace the real first candidate -- and on a short input it is
// supposed to be absent entirely.
//
// Neither half was in the code. `filters.cc` short-circuited a `sentence`
// first candidate straight to slot 0, did the same whenever the first
// candidate did not span the whole input, and computed the syllable count
// `(input.size() + 1) / 2` without ever using it.

// gtest first: the plugin headers pull in Rime's X11-style key table, whose
// `Bool` macro would otherwise clobber gtest's Bool() generator.
#include <gtest/gtest.h>

#include "raw_input_util.h"

#include <string>
#include <vector>

using rime::raw_input_detail::kNoSlot;
using rime::raw_input_detail::Slot;
using rime::raw_input_detail::SyllableCount;

namespace {

// Four syllables of flypy; the shortest input the raw candidate is wanted on.
const std::string kLong = "ujwsdehk";
const std::vector<std::string> kFive = {"建议", "简易", "件", "简", "见"};

}  // namespace

TEST(RawInputOrder, SyllableCountRoundsAPartialSyllableUp) {
  EXPECT_EQ(0u, SyllableCount(""));
  EXPECT_EQ(1u, SyllableCount("u"));
  EXPECT_EQ(1u, SyllableCount("uj"));
  EXPECT_EQ(2u, SyllableCount("ujw"));
  EXPECT_EQ(4u, SyllableCount("ujwsdehk"));
}

TEST(RawInputOrder, LandsOnTheLastSlotOfThePage) {
  // page_size 5 -> four real candidates, then the letters. This is the whole
  // point of the feature: visible without costing a candidate you would pick.
  EXPECT_EQ(4u, Slot(kFive, kLong, 5));
}

TEST(RawInputOrder, ShortInputGetsNoRawCandidate) {
  // Three syllables is still readable off the preedit.
  EXPECT_EQ(kNoSlot, Slot(kFive, "ujwsde", 5));
  EXPECT_EQ(kNoSlot, Slot(kFive, "uj", 5));
  EXPECT_EQ(kNoSlot, Slot(kFive, "", 5));
}

TEST(RawInputOrder, ASentenceFirstCandidateDoesNotPullItToTheFront) {
  // The regression this file exists for. A `sentence` candidate used to
  // short-circuit the placement and put the letters at slot 0, which is
  // exactly the long input the user needs a real first candidate on.
  EXPECT_EQ(4u, Slot({"建议方案", "简易", "件", "简", "见"}, kLong, 5));
}

TEST(RawInputOrder, APartialFirstCandidateDoesNotPullItToTheFront) {
  // Nor does a first candidate covering less than the whole input -- on a
  // long input that is the common case, not the exception.
  EXPECT_EQ(4u, Slot({"建", "简", "见", "件", "剑"}, kLong, 5));
}

TEST(RawInputOrder, GoesAtTheEndWhenThereAreFewerCandidatesThanAPage) {
  EXPECT_EQ(2u, Slot({"建议", "简易"}, kLong, 5));
  EXPECT_EQ(1u, Slot({"建议"}, kLong, 5));
  EXPECT_EQ(0u, Slot({}, kLong, 5));
}

TEST(RawInputOrder, NotDuplicatedWhenACandidateAlreadyIsTheLetters) {
  // melt_eng and friends can offer the typed letters as a real candidate;
  // showing them twice is noise.
  EXPECT_EQ(kNoSlot, Slot({"建议", kLong, "件", "简", "见"}, kLong, 5));
}

TEST(RawInputOrder, ACandidateBeyondThePageDoesNotCountAsADuplicate) {
  // Only the candidates the user can actually see alongside the raw one.
  // Position 5 is on the next page; the letters still belong on this one.
  EXPECT_EQ(4u, Slot({"建议", "简易", "件", "简", kLong}, kLong, 5));
}

TEST(RawInputOrder, APageWithNoRoomGetsNoRawCandidate) {
  // page_size 1 has no slot that is not the first candidate, and page_size 0
  // is what an unset schema reports. Neither may displace the head -- the
  // old `page_size_ - 1` loop bound went negative and did exactly that.
  EXPECT_EQ(kNoSlot, Slot(kFive, kLong, 1));
  EXPECT_EQ(kNoSlot, Slot(kFive, kLong, 0));
}

TEST(RawInputOrder, TheThresholdIsTheCallersToChoose) {
  EXPECT_EQ(4u, Slot(kFive, "ujwsde", 5, /*min_syllables=*/3));
  EXPECT_EQ(kNoSlot, Slot(kFive, kLong, 5, /*min_syllables=*/5));
}
