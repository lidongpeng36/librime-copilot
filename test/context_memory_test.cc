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

#include "context_memory_step.h"

using rime::context_memory::Options;
using rime::context_memory::Step;

namespace {
struct FakeMode {
  bool ascii = false;
  bool Get() const { return ascii; }
  void Set(bool v) { ascii = v; }
};
}  // namespace

// The single most important case: the first time a context is seen the mode
// must not be touched. This is the entire basis for Squirrel's app_options
// degrading into a per-context default rather than being fought.
TEST(ContextMemoryStep, FirstSightingDoesNotTouchTheMode) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;
  mode.ascii = true;
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  EXPECT_TRUE(mode.ascii);
}

TEST(ContextMemoryStep, RestoresOnSwitchBack) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;

  mode.ascii = false;  // pane 1: Chinese
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%1", mode.Get(), false);

  mode.ascii = true;  // pane 2: the user switches to English
  step.OnHead("tmux:default:%2", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%2", mode.Get(), false);

  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  EXPECT_FALSE(mode.ascii);  // pane 1 is Chinese again
}

// The tail runs at the END of the event, so it captures the keystroke that
// changed the mode -- a Shift toggle. Recording at the head would store the
// value the user just left behind.
TEST(ContextMemoryStep, RecordsTheTailValueNotTheHeadValue) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;
  mode.ascii = false;
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  mode.ascii = true;  // the keystroke toggled it
  step.OnTail("tmux:default:%1", mode.Get(), false);
  ASSERT_TRUE(table.Get("tmux:default:%1").has_value());
  EXPECT_TRUE(*table.Get("tmux:default:%1"));
}

// Without this, an event whose identity is momentarily unavailable (tmux timed
// out, the server went away) leaves last_key_ pointing at the pane the user
// was in BEFORE, and the tail writes the current mode into that pane's slot.
// The value written is not that pane's, and the corrupted entry is
// indistinguishable from a legitimate one.
TEST(ContextMemoryStep, UnresolvedEventWritesNothing) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;
  mode.ascii = false;
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%1", mode.Get(), false);

  mode.ascii = true;
  step.OnUnresolved();  // identity unavailable this event
  // OnTail, deliberately, and NOT OnTailUnresolved: the latter's entire body is
  // `resolved_ = false`, so a test that calls it still passes with OnTail's
  // `!resolved_` early-return deleted -- it exercises nothing. The invariant
  // being defended is that a tail REACHING OnTail after an unresolved head
  // writes nothing, and only this sequence reaches it. Verified by deleting the
  // `!resolved_` term and watching this line fail.
  step.OnTail("tmux:default:%1", mode.Get(), false);
  ASSERT_TRUE(table.Get("tmux:default:%1").has_value());
  EXPECT_FALSE(*table.Get("tmux:default:%1"));  // still pane 1's own value
}

// The call sequence the RAII guard in Copilot::ProcessKeyEvent actually takes
// when the head resolved nothing. It cannot fail while OnTail's guard exists --
// it pins the guard's wiring, not the invariant above, which is why the two are
// separate tests rather than one that looks like it covers both.
TEST(ContextMemoryStep, GuardsUnresolvedTailPathWritesNothing) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;
  mode.ascii = false;
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%1", mode.Get(), false);

  mode.ascii = true;
  step.OnUnresolved();
  step.OnTailUnresolved();
  EXPECT_EQ(table.size(), 1u);
  ASSERT_TRUE(table.Get("tmux:default:%1").has_value());
  EXPECT_FALSE(*table.Get("tmux:default:%1"));
}

// ImeBridge's pending-action queue carries no context. Pane A's nvim queues
// set(ascii=true) on InsertLeave; the user switches to pane B (remembered
// Chinese) and types one key. The head restores false, RunProcessors then
// applies A's queued action -> true, and without this guard the tail records
// true as PANE B's mode. Before per-context memory that misapplication lasted
// one keystroke; recorded, it becomes durable and self-reinforcing.
//
// The bridge still WINS the mode -- that is the design's precedence rule and
// this does not touch it. It just does not get to say which pane the value
// belongs to.
TEST(ContextMemoryStep, ABridgeWriteDuringTheEventIsNotAttributedToThePane) {
  Table table;
  Options opt;
  opt.enable = true;  // the struct default is false: the feature ships off
  Step step(&table, opt);
  FakeMode mode;

  mode.ascii = false;  // pane B is remembered as Chinese
  step.OnHead("tmux:default:%2", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%2", mode.Get(), false);
  ASSERT_TRUE(table.Get("tmux:default:%2").has_value());
  EXPECT_FALSE(*table.Get("tmux:default:%2"));

  // Next keystroke in pane B: ImeBridge applies pane A's queued set(true).
  step.OnHead("tmux:default:%2", mode.Get(), [&](bool v) { mode.Set(v); });
  mode.ascii = true;  // the bridge wrote it, mid-event
  step.OnTail("tmux:default:%2", mode.Get(), true);

  ASSERT_TRUE(table.Get("tmux:default:%2").has_value());
  EXPECT_FALSE(*table.Get("tmux:default:%2"));  // still B's own value
}

TEST(ContextMemoryStep, DisabledDoesNothing) {
  Table table;
  Options opt;
  opt.enable = false;
  Step step(&table, opt);
  FakeMode mode;
  mode.ascii = true;
  step.OnHead("tmux:default:%1", mode.Get(), [&](bool v) { mode.Set(v); });
  step.OnTail("tmux:default:%1", mode.Get(), false);
  EXPECT_EQ(table.size(), 0u);
}
