// The commit-time decision, driven with hand-built compositions so no Rime
// engine is required — the same technique as commit_text_test.cc.

#include <gtest/gtest.h>

#include <rime/candidate.h>
#include <rime/common.h>
#include <rime/composition.h>
#include <rime/context.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include "telemetry_commit.h"

using namespace rime;
using namespace rime::telemetry;

namespace {

// A segment spanning [start,end) offering `texts`, with `selected` highlighted.
Segment MakeSegment(size_t start, size_t end, const std::vector<std::string>& texts,
                    size_t selected) {
  Segment seg(static_cast<int>(start), static_cast<int>(end));
  seg.status = Segment::kSelected;
  auto menu = New<Menu>();
  auto translation = New<FifoTranslation>();
  for (const auto& t : texts) {
    translation->Append(New<SimpleCandidate>("test", start, end, t));
  }
  menu->AddTranslation(translation);
  seg.menu = menu;
  seg.selected_index = selected;
  return seg;
}

// A segment whose `end` has been narrowed below `start + length`, which is what
// Segment::Close() does when the user selects a partially matching candidate
// (librime src/rime/segmentation.cc:17-24). `length` keeps the original extent.
Segment MakeNarrowedSegment(size_t start, size_t original_end, size_t narrowed_end,
                            const std::vector<std::string>& texts, size_t selected) {
  Segment seg = MakeSegment(start, original_end, texts, selected);
  seg.end = narrowed_end;
  seg.tags.insert("partial");
  return seg;
}

RerankTrace TraceFor(const std::string& input, size_t start, size_t end,
                     const std::string& promoted, int from) {
  RerankTrace t;
  t.valid = true;
  t.input = input;
  t.start = start;
  t.end = end;
  t.ctx = "我们";
  t.src = "tmux";
  t.record.key = "们";
  t.record.key_len = 1;
  t.record.n = 500;
  t.record.text = promoted;
  t.record.from = from;
  t.record.rank = 7;
  t.record.level = 3;
  return t;
}

// The store the filter writes into, holding just these traces.
RerankTraceStore StoreOf(const std::vector<RerankTrace>& traces) {
  RerankTraceStore store;
  for (const auto& t : traces) {
    store.Record(t);
  }
  return store;
}

std::vector<Event> Build(Context* ctx, const RerankTraceStore* traces) {
  Options o;
  return BuildCommitEvents(ctx, traces, o, "M1", "flypy", "2026-08-14T10:00:00+0800");
}

}  // namespace

TEST(BuildCommitEvents, IgnoresOrdinaryTyping) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 0));
  EXPECT_TRUE(Build(&ctx, nullptr).empty());
}

TEST(BuildCommitEvents, RecordsANonFirstSelectionWithNoRerank) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  auto events = Build(&ctx, nullptr);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].sel, "尼");
  EXPECT_EQ(events[0].sel_idx, 1);
  EXPECT_EQ(events[0].input, "ni");
  EXPECT_EQ(events[0].machine, "M1");
  EXPECT_EQ(events[0].schema, "flypy");
  EXPECT_FALSE(events[0].rr.has_value());
}

TEST(BuildCommitEvents, RecordsAnAcceptedPromotion) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 0));
  auto store = StoreOf({TraceFor("ni", 0, 2, "你", 3)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(events[0].rr.has_value());
  EXPECT_EQ(events[0].rr->text, "你");
  EXPECT_EQ(events[0].sel, "你");
  EXPECT_EQ(events[0].ctx, "我们");
  EXPECT_EQ(events[0].src, "tmux");
}

TEST(BuildCommitEvents, RecordsARejectedPromotion) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  auto store = StoreOf({TraceFor("ni", 0, 2, "你", 3)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(events[0].rr.has_value());
  EXPECT_EQ(events[0].rr->text, "你");
  EXPECT_EQ(events[0].sel, "尼");  // promoted != selected: a rejection
}

// from == 0 means re-ranking agreed with the translator and moved nothing. It
// is still recorded — the spec's control group — and must not be mistaken for
// ordinary typing.
TEST(BuildCommitEvents, RecordsAPromotionThatMovedNothing) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 0));
  auto store = StoreOf({TraceFor("ni", 0, 2, "你", 0)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(events[0].rr.has_value());
  EXPECT_EQ(events[0].rr->from, 0);
}

// A trace from an abandoned composition must never be credited to this one.
TEST(BuildCommitEvents, RefusesAStaleTrace) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  ctx.composition().push_back(MakeSegment(0, 5, {"你好", "拟好"}, 1));
  auto stale = StoreOf({TraceFor("ni", 0, 2, "你", 3)});
  auto events = Build(&ctx, &stale);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_FALSE(events[0].rr.has_value());
  EXPECT_TRUE(events[0].ctx.empty());  // context came from the stale trace too
}

// Re-ranking ran but promoted nothing: the context is still worth attaching to
// a non-first selection, while `rr` must stay absent.
TEST(BuildCommitEvents, KeepsContextWhenNothingWasPromoted) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  RerankTrace t = TraceFor("ni", 0, 2, "", 0);
  t.record.text.clear();
  auto store = StoreOf({t});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_FALSE(events[0].rr.has_value());
  EXPECT_EQ(events[0].ctx, "我们");
}

TEST(BuildCommitEvents, SkipsPredictionSegments) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  Segment seg = MakeSegment(0, 2, {"你", "尼"}, 1);
  seg.tags.insert("copilot");
  ctx.composition().push_back(std::move(seg));
  EXPECT_TRUE(Build(&ctx, nullptr).empty());
}

TEST(BuildCommitEvents, CapsTopAtTopN) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"a", "b", "c", "d", "e", "f", "g"}, 6));
  Options o;
  o.top_n = 3;
  auto events = BuildCommitEvents(&ctx, nullptr, o, "M1", "flypy", "t");
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].top.size(), 3u);
  EXPECT_EQ(events[0].top[0], "a");
}

TEST(BuildCommitEvents, TopStopsAtTheEndOfAShortList) {
  Context ctx;
  ctx.set_input("ni");
  ctx.composition().Reset("ni");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  auto events = Build(&ctx, nullptr);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events[0].top.size(), 2u);
}

// Only the segment the trace was recorded for gets an `rr` block. The other
// segment is still recorded on its own merits.
TEST(BuildCommitEvents, AttributesTheTraceToOneSegmentOnly) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  ctx.composition().push_back(MakeSegment(2, 5, {"好", "号"}, 1));
  auto store = StoreOf({TraceFor("nihao", 0, 2, "你", 3)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 2u);
  EXPECT_TRUE(events[0].rr.has_value());
  EXPECT_FALSE(events[1].rr.has_value());
  EXPECT_EQ(events[1].input, "hao");
}

// The regression this store exists for. Re-ranking fires on both segments of
// one composition; with a single shared slot the second decision destroyed the
// first, and the first segment was then reported with `rr` absent — which
// analyze_telemetry.py files as "misrank", the bucket that flatters the filter.
TEST(BuildCommitEvents, AttributesEverySegmentOfAMultiSegmentComposition) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  ctx.composition().push_back(MakeSegment(0, 2, {"你", "尼"}, 1));
  ctx.composition().push_back(MakeSegment(2, 5, {"好", "号"}, 1));
  auto store = StoreOf({TraceFor("nihao", 0, 2, "你", 3), TraceFor("nihao", 2, 5, "好", 2)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 2u);
  ASSERT_TRUE(events[0].rr.has_value());
  ASSERT_TRUE(events[1].rr.has_value());
  EXPECT_EQ(events[0].rr->text, "你");
  EXPECT_EQ(events[1].rr->text, "好");
  EXPECT_EQ(events[0].sel, "尼");
  EXPECT_EQ(events[1].sel, "号");
}

// Segment::Close() narrows `end` to the selected candidate's end on a partial
// selection but leaves `length` at the original extent, and the filter recorded
// the span it saw while the menu was built — the original extent. Matching on
// `seg.end` would drop the event; matching on `seg.start + seg.length` finds
// it. Every other test here builds segments where the two are equal, so this is
// the only one that can tell the conventions apart.
TEST(BuildCommitEvents, MatchesTheSegmentsOriginalExtentNotItsNarrowedEnd) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  Segment seg = MakeNarrowedSegment(0, 5, 2, {"你好", "拟好"}, 1);
  ASSERT_EQ(seg.end, 2u);
  ASSERT_EQ(seg.start + seg.length, 5u);  // the two conventions disagree here
  ctx.composition().push_back(std::move(seg));

  auto store = StoreOf({TraceFor("nihao", 0, 5, "你好", 3)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  ASSERT_TRUE(events[0].rr.has_value());
  EXPECT_EQ(events[0].rr->text, "你好");
  EXPECT_EQ(events[0].ctx, "我们");
  EXPECT_EQ(events[0].input, "ni");  // the input actually committed, seg.end
}

// The converse, so the match cannot be loosened into accepting either
// convention: a trace recorded for the narrowed span is a different segment and
// must be refused.
TEST(BuildCommitEvents, RefusesATraceRecordedForTheNarrowedSpan) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  ctx.composition().push_back(MakeNarrowedSegment(0, 5, 2, {"你好", "拟好"}, 1));
  auto store = StoreOf({TraceFor("nihao", 0, 2, "你", 3)});
  auto events = Build(&ctx, &store);
  ASSERT_EQ(events.size(), 1u);
  EXPECT_FALSE(events[0].rr.has_value());
  EXPECT_TRUE(events[0].ctx.empty());
}

TEST(BuildCommitEvents, ToleratesANullContext) { EXPECT_TRUE(Build(nullptr, nullptr).empty()); }

// The premise Copilot::OnContextUpdate's trace clearing rests on: an abandoned
// composition reaches the update notifier, with the composition already empty.
// Copilot itself needs an Engine and cannot be built in this suite, but the
// librime behaviour it depends on can be — and it is the part that a librime
// upgrade could silently take away.
TEST(ContextClear, FiresTheUpdateNotifierWithTheCompositionEmptied) {
  Context ctx;
  ctx.set_input("nihao");
  ctx.composition().Reset("nihao");
  ctx.composition().push_back(MakeSegment(0, 5, {"你好"}, 0));
  ASSERT_TRUE(ctx.IsComposing());

  int updates = 0;
  bool composing_when_notified = true;
  auto connection = ctx.update_notifier().connect([&](Context* c) {
    ++updates;
    composing_when_notified = c->IsComposing();
  });

  ctx.Clear();  // what Esc and Context::AbortComposition() reach

  connection.disconnect();
  EXPECT_EQ(updates, 1);
  EXPECT_FALSE(composing_when_notified);
}
