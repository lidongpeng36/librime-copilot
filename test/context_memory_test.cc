#include "context_memory.h"

#include <gtest/gtest.h>

using rime::context_memory::Identity;
using rime::context_memory::MakeKey;
using rime::context_memory::Table;

TEST(ContextMemoryKey, IncludesSocketAndPane) {
  Identity id{"default", "%7", "claude"};
  EXPECT_EQ(MakeKey(id, false), "tmux:default:%7");
}

TEST(ContextMemoryKey, AppendsCommandWhenAsked) {
  Identity id{"default", "%7", "claude"};
  EXPECT_EQ(MakeKey(id, true), "tmux:default:%7|claude");
}

TEST(ContextMemoryKey, EmptySocketBecomesDefault) {
  Identity id{"", "%3", ""};
  EXPECT_EQ(MakeKey(id, true), "tmux:default:%3");
}

TEST(ContextMemoryKey, EmptyCommandAddsNoSeparator) {
  Identity id{"default", "%3", ""};
  EXPECT_EQ(MakeKey(id, true), "tmux:default:%3");
}

TEST(ContextMemoryTable, MissOnFirstSighting) {
  Table t;
  EXPECT_FALSE(t.Get("tmux:default:%1").has_value());
}

TEST(ContextMemoryTable, RoundTrips) {
  Table t;
  t.Put("a", true);
  t.Put("b", false);
  ASSERT_TRUE(t.Get("a").has_value());
  EXPECT_TRUE(*t.Get("a"));
  ASSERT_TRUE(t.Get("b").has_value());
  EXPECT_FALSE(*t.Get("b"));
}

TEST(ContextMemoryTable, PutOverwrites) {
  Table t;
  t.Put("a", true);
  t.Put("a", false);
  EXPECT_EQ(t.size(), 1u);
  EXPECT_FALSE(*t.Get("a"));
}

TEST(ContextMemoryTable, EvictsLeastRecentlyUsed) {
  Table t(2);
  t.Put("a", true);
  t.Put("b", true);
  t.Get("a");        // "a" is now the most recently used
  t.Put("c", true);  // evicts "b", not "a"
  EXPECT_TRUE(t.Get("a").has_value());
  EXPECT_FALSE(t.Get("b").has_value());
  EXPECT_TRUE(t.Get("c").has_value());
  EXPECT_EQ(t.size(), 2u);
}

TEST(ContextMemoryTable, ShrinkingMaxEntriesEvictsImmediately) {
  Table t(4);
  t.Put("a", true);
  t.Put("b", true);
  t.Put("c", true);
  t.set_max_entries(1);
  EXPECT_EQ(t.size(), 1u);
  EXPECT_TRUE(t.Get("c").has_value());
}

// Contains is the probe for callers that must not become a reason a key stays
// remembered -- Get would splice "a" to the front and save it from eviction.
TEST(ContextMemoryTable, ContainsDoesNotChangeRecency) {
  Table t(2);
  t.Put("a", true);
  t.Put("b", true);
  EXPECT_TRUE(t.Contains("a"));
  EXPECT_FALSE(t.Contains("nobody"));
  t.Put("c", true);  // evicts "a", which Contains must not have promoted
  EXPECT_FALSE(t.Contains("a"));
  EXPECT_TRUE(t.Contains("b"));
  EXPECT_TRUE(t.Contains("c"));
}

