// Unit tests for the UTF8 view and History ring buffer (history.h).
//
// UTF8 stores a string_view over the source string, so every test keeps the
// backing std::string alive for the lifetime of the UTF8 object.

#include "history.h"

#include <gtest/gtest.h>
#include <string>

using copilot::History;
using copilot::UTF8;

TEST(Utf8View, IndexingMixedWidth) {
  std::string s = "中a文";  // 中 a 文
  UTF8 u(s);
  ASSERT_EQ(3u, u.size());
  EXPECT_EQ("中", u[0]);
  EXPECT_EQ("a", u[1]);
  EXPECT_EQ("文", u[2]);
  EXPECT_EQ("文", u[-1]);  // negative index from the end
  EXPECT_EQ("", u[3]);     // out of range
}

TEST(Utf8View, RangeSlicingInclusive) {
  std::string s = "abcde";
  UTF8 u(s);
  EXPECT_EQ("bcd", u(1, 3));     // inclusive [1,3]
  EXPECT_EQ("abcde", u(0, -1));  // whole string
  EXPECT_EQ("de", u(-2, -1));    // last two
  EXPECT_EQ("", u(3, 1));        // start > end -> empty
}

TEST(Utf8View, LeftRightAroundPunctuation) {
  std::string s = "ni,hao";  // punctuation splits the string
  UTF8 u(s);
  EXPECT_EQ("ni", u.left());
  EXPECT_EQ("hao", u.right());
}

TEST(Utf8View, LeftRightWithoutPunctuation) {
  std::string s = "abc";
  UTF8 u(s);
  EXPECT_EQ("ab", u.left());   // no punct: all but the last char
  EXPECT_EQ("bc", u.right());  // no punct: from the second char
}

TEST(Utf8View, LeftIsEmptyWhenPunctuationLeads) {
  // Nothing precedes a leading punctuation mark. Returning (0, -1) here made
  // left() hand back the WHOLE string, punctuation included.
  std::string s = ",hao";
  UTF8 u(s);
  EXPECT_EQ("", u.left());
  EXPECT_EQ("hao", u.right());
}

TEST(Utf8View, RightIsEmptyWhenPunctuationTrails) {
  // Mirror case: nothing follows a trailing punctuation mark; the clamped
  // (i+1, -1) slice used to return the punctuation itself.
  std::string s = "ni,";
  UTF8 u(s);
  EXPECT_EQ("ni", u.left());
  EXPECT_EQ("", u.right());
}

TEST(Utf8View, LeftRightAroundChinesePunctuation) {
  std::string s = "你，好";
  UTF8 u(s);
  EXPECT_EQ("你", u.left());
  EXPECT_EQ("好", u.right());
}

TEST(HistoryBuffer, BackReturnsLastCharacter) {
  History h(10);
  h.add("中文");
  EXPECT_EQ("文", h.back());    // back() = last character of last chunk
  EXPECT_EQ("中文", h.last());  // last() = whole last chunk
  EXPECT_EQ(1u, h.size());
  EXPECT_FALSE(h.empty());
}

TEST(HistoryBuffer, GetsReturnsLastNChunks) {
  History h(10);
  h.add("ab");
  h.add("中文");
  EXPECT_EQ("中文", h.gets(1));
  EXPECT_EQ("ab中文", h.gets(2));
  EXPECT_EQ("ab中文", h.gets(5));  // asking for more than available
}

TEST(HistoryBuffer, GetCharsCrossesChunkBoundary) {
  History h(10);
  h.add("ab");
  h.add("中文");
  EXPECT_EQ("中文", h.get_chars(2));
  EXPECT_EQ("b中文", h.get_chars(3));
}

TEST(HistoryBuffer, PopRemovesTrailingCharacter) {
  History h(10);
  h.add("中文");
  h.pop();
  EXPECT_EQ("中", h.back());
  EXPECT_EQ("中", h.last());
  h.pop();
  EXPECT_TRUE(h.empty());
}

TEST(HistoryBuffer, ClearEmptiesEverything) {
  History h(10);
  h.add("abc");
  h.add("中");
  h.clear();
  EXPECT_TRUE(h.empty());
  EXPECT_EQ(0u, h.size());
  EXPECT_EQ("", h.back());
}

TEST(HistoryBuffer, CleanupHonorsCapacity) {
  // capacity_ == size_ * 2; adding the 2nd entry triggers cleanup down to size_.
  History h(1);
  h.add("a");
  h.add("b");
  EXPECT_EQ(1u, h.size());
  EXPECT_EQ("b", h.last());
}

TEST(HistoryBuffer, CleanupDropsExactlyTheEvictedBytes) {
  // Regression: cleanup() summed the totals of the WRONG entries (it indexed a
  // deque it was popping from, so it read every other entry). With chunks of
  // mixed byte width it erased too many bytes from input_, leaving the text
  // shorter than what pos_ still accounted for — every later read then computed
  // a negative (wrapped) offset and threw std::out_of_range.
  History h(2);  // capacity_ == 4: the 4th add evicts the first two chunks
  h.add("a");    // 1 byte  ─┬ evicted
  h.add("b");    // 1 byte  ─┘
  h.add("好");   // 3 bytes ─┬ must survive intact
  h.add("世");   // 3 bytes ─┘

  EXPECT_EQ(2u, h.size());
  EXPECT_EQ("好世", h.gets(10));
  EXPECT_EQ("好世", h.get_chars(2));
  EXPECT_EQ("世", h.back());
  EXPECT_EQ("世", h.last());
}

TEST(HistoryBuffer, CleanupKeepsSurvivorsWhenEvictedChunksAreLonger) {
  // The mirror case: evicting long chunks must not leave stale bytes behind.
  History h(2);
  h.add("好");  // 3 bytes ─┬ evicted
  h.add("世");  // 3 bytes ─┘
  h.add("a");   // 1 byte
  h.add("b");   // 1 byte

  EXPECT_EQ(2u, h.size());
  EXPECT_EQ("ab", h.gets(10));
  EXPECT_EQ("ab", h.get_chars(2));
}
