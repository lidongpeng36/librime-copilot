#include <gtest/gtest.h>

#include <rime/candidate.h>
#include <rime/menu.h>
#include <rime/segmentation.h>
#include <rime/translation.h>

#include <string>
#include <utility>
#include <vector>

#include "rerank_filter.h"
#include "rerank_trace.h"

using namespace rime;

namespace {

RerankTrace Recorded() {
  RerankTrace t;
  t.valid = true;
  t.input = "nihao";
  t.start = 0;
  t.end = 2;
  t.ctx = "我们";
  t.src = "tmux";
  t.record.text = "你";
  return t;
}

RerankTrace At(const std::string& input, size_t start, size_t end, const std::string& promoted) {
  RerankTrace t = Recorded();
  t.input = input;
  t.start = start;
  t.end = end;
  t.record.text = promoted;
  return t;
}

// A segment spanning [start,end) offering (text, candidate end) pairs. The
// candidate end matters here: a candidate ending short of the segment is a
// partial match, and that is the only case in which the segment's extent and
// the head candidate's end differ.
Segment MakeSegment(size_t start, size_t end,
                    const std::vector<std::pair<std::string, size_t>>& cands, size_t selected) {
  Segment seg(static_cast<int>(start), static_cast<int>(end));
  seg.status = Segment::kSelected;
  auto menu = New<Menu>();
  auto translation = New<FifoTranslation>();
  for (const auto& c : cands) {
    translation->Append(New<SimpleCandidate>("test", start, c.second, c.first));
  }
  menu->AddTranslation(translation);
  seg.menu = menu;
  seg.selected_index = selected;
  return seg;
}

// Needs no engine: Filter's constructor only stores ticket.engine and
// ticket.name_space, and a null db makes Apply() a no-op.
CopilotRerankFilter MakeFilter() {
  return CopilotRerankFilter(Ticket{}, an<CopilotDb>(), RerankOptions{}, an<RerankTraceStore>(),
                             an<CopilotEngine>());
}

}  // namespace

TEST(RerankTrace, MatchesTheSegmentItWasRecordedFor) {
  EXPECT_TRUE(TraceMatches(Recorded(), "nihao", 0, 2));
}

TEST(RerankTrace, RejectsADifferentInput) {
  EXPECT_FALSE(TraceMatches(Recorded(), "nihaoma", 0, 2));
}

TEST(RerankTrace, RejectsADifferentSpan) {
  EXPECT_FALSE(TraceMatches(Recorded(), "nihao", 0, 5));
  EXPECT_FALSE(TraceMatches(Recorded(), "nihao", 2, 4));
}

TEST(RerankTrace, RejectsAnUnrecordedTrace) {
  RerankTrace t;
  EXPECT_FALSE(TraceMatches(t, "", 0, 0));
}

TEST(RerankTrace, ClearInvalidates) {
  RerankTrace t = Recorded();
  t.Clear();
  EXPECT_FALSE(TraceMatches(t, "nihao", 0, 2));
}

// Re-ranking ran and recorded its context but promoted nothing. The context is
// still worth keeping for a non-first selection; the absent promotion is what
// must not be reported as one.
TEST(RerankTrace, RecordsAContextWithoutAPromotion) {
  RerankTrace t = Recorded();
  t.record.text.clear();
  EXPECT_TRUE(TraceMatches(t, "nihao", 0, 2));
  EXPECT_TRUE(t.record.text.empty());
}

// The reason the store exists: with one slot, the second segment's decision
// destroyed the first's, and the first segment was then reported as "no
// re-ranking" — the bucket that flatters the filter.
TEST(RerankTraceStore, KeepsEverySegmentOfOneComposition) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  store.Record(At("nihao", 2, 5, "好"));

  const RerankTrace* first = store.Find("nihao", 0, 2);
  const RerankTrace* second = store.Find("nihao", 2, 5);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);
  EXPECT_EQ(first->record.text, "你");
  EXPECT_EQ(second->record.text, "好");
}

TEST(RerankTraceStore, FindRejectsAnUnrecordedSegment) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  EXPECT_EQ(store.Find("nihao", 2, 5), nullptr);    // right input, wrong span
  EXPECT_EQ(store.Find("nihaoma", 0, 2), nullptr);  // right span, wrong input
  EXPECT_EQ(store.Find("", 0, 0), nullptr);
}

// Apply() builds a fresh translation on every keystroke, so the same segment is
// re-recorded constantly. That must replace, not accumulate.
TEST(RerankTraceStore, ReRecordingASegmentReplacesItInPlace) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  store.Record(At("nihao", 0, 2, "尼"));
  EXPECT_EQ(store.size(), 1u);
  ASSERT_NE(store.Find("nihao", 0, 2), nullptr);
  EXPECT_EQ(store.Find("nihao", 0, 2)->record.text, "尼");
}

TEST(RerankTraceStore, IgnoresAnUnrecordedTrace) {
  RerankTraceStore store;
  RerankTrace blank;  // valid == false
  store.Record(blank);
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.Find("", 0, 0), nullptr);
}

// Record() is on a keystroke path, so the store must not grow without bound.
// Past the cap the oldest entry is dropped: that costs an event and never
// misattributes one.
TEST(RerankTraceStore, IsBoundedAndDropsTheOldest) {
  RerankTraceStore store;
  const size_t n = RerankTraceStore::kMaxEntries + 4;
  for (size_t i = 0; i < n; ++i) {
    store.Record(At("input", i, i + 1, "x"));
  }
  EXPECT_EQ(store.size(), RerankTraceStore::kMaxEntries);
  EXPECT_EQ(store.Find("input", 0, 1), nullptr);      // dropped
  EXPECT_EQ(store.Find("input", 3, 4), nullptr);      // dropped
  EXPECT_NE(store.Find("input", n - 1, n), nullptr);  // newest kept
  EXPECT_NE(
      store.Find("input", n - RerankTraceStore::kMaxEntries, n - RerankTraceStore::kMaxEntries + 1),
      nullptr);  // oldest survivor
}

// The store is cleared at commit and whenever the composition empties. Nothing
// may survive that, or the next composition inherits a decision never made for
// it.
TEST(RerankTraceStore, ClearDropsEverything) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  store.Record(At("nihao", 2, 5, "好"));
  store.Clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.Find("nihao", 0, 2), nullptr);
  EXPECT_EQ(store.Find("nihao", 2, 5), nullptr);
}

TEST(TraceSpanOf, IsTheSegmentsOwnExtent) {
  Segment seg(2, 5);
  EXPECT_EQ(TraceSpanOf(seg).start, 2u);
  EXPECT_EQ(TraceSpanOf(seg).end, 5u);
}

// Segment::Close() narrows `end` to the selected candidate's end but leaves
// `length` alone, so the span survives a partial selection unchanged. This is
// the property that lets the writer (before selection) and the reader (after
// it) agree.
TEST(TraceSpanOf, SurvivesTheNarrowingSegmentCloseDoes) {
  Segment seg = MakeSegment(0, 5, {{"你", 2}, {"你好", 5}}, 0);
  const TraceSpan before = TraceSpanOf(seg);
  seg.Close();
  ASSERT_EQ(seg.end, 2u);  // librime narrowed it
  ASSERT_TRUE(seg.HasTag("partial"));
  EXPECT_EQ(TraceSpanOf(seg).start, before.start);
  EXPECT_EQ(TraceSpanOf(seg).end, before.end);
  EXPECT_NE(TraceSpanOf(seg).end, seg.end);  // the two conventions disagree
}

// The writer-side regression. The filter used to key its trace on the head
// CANDIDATE's end (window.front()->end()). Whenever the head candidate is a
// partial match that is a different number from the segment's extent, the
// commit lookup missed, and the event was written with `rr` absent — which
// analyze_telemetry.py files as "misrank", the bucket that blames the
// translator rather than this filter.
TEST(RerankFilterTraceSpan, KeysOnTheSegmentExtentNotTheHeadCandidatesEnd) {
  CopilotRerankFilter filter = MakeFilter();

  // Head candidate covers "ni" of "nihao"; the segment spans all five bytes.
  Segment seg = MakeSegment(0, 5, {{"你", 2}, {"你好", 5}}, 0);
  ASSERT_EQ(seg.GetCandidateAt(0)->end(), 2u);
  ASSERT_EQ(seg.start + seg.length, 5u);

  ASSERT_TRUE(filter.AppliesToSegment(&seg));
  ASSERT_TRUE(filter.pending_trace_span().has_value());
  EXPECT_EQ(filter.pending_trace_span()->start, 0u);
  EXPECT_EQ(filter.pending_trace_span()->end, 5u);  // not 2, the head's end
}

// End to end over the two conventions: what the filter records at translate
// time is what the commit walk looks up after librime has narrowed the segment.
TEST(RerankFilterTraceSpan, WhatTheWriterRecordsIsWhatTheReaderLooksUp) {
  CopilotRerankFilter filter = MakeFilter();
  Segment seg = MakeSegment(0, 5, {{"你", 2}, {"你好", 5}}, 0);
  const size_t head_candidate_end = seg.GetCandidateAt(0)->end();

  // Translate time: the filter captures the span it will key its trace on.
  ASSERT_TRUE(filter.AppliesToSegment(&seg));
  const TraceSpan written = *filter.pending_trace_span();
  RerankTraceStore store;
  store.Record(At("nihao", written.start, written.end, "你好"));

  // Selection time: the user takes the partial candidate.
  seg.Close();
  ASSERT_EQ(seg.end, 2u);

  // Commit time: telemetry_commit.cc looks up TraceSpanOf(seg) and finds it.
  const TraceSpan looked_up = TraceSpanOf(seg);
  ASSERT_NE(store.Find("nihao", looked_up.start, looked_up.end), nullptr);
  EXPECT_EQ(store.Find("nihao", looked_up.start, looked_up.end)->record.text, "你好");

  // The old writer convention would have missed this lookup entirely.
  ASSERT_NE(head_candidate_end, written.end);
  EXPECT_EQ(store.Find("nihao", written.start, head_candidate_end), nullptr);
}

// The pre-existing behaviour of AppliesToSegment is unchanged: prediction
// placeholders are still declined.
TEST(RerankFilterTraceSpan, StillDeclinesThePredictionPlaceholder) {
  CopilotRerankFilter filter = MakeFilter();
  Segment seg = MakeSegment(0, 5, {{"你好", 5}}, 0);
  seg.tags.insert("copilot");
  EXPECT_FALSE(filter.AppliesToSegment(&seg));
}

// Declining a "copilot"-tagged segment must not leave a previous segment's
// span behind either: that segment's Apply() never runs, so nothing should
// remain for the *next* segment's Apply() to misattribute it to.
TEST(RerankFilterTraceSpan, DecliningTheSegmentClearsAnyPendingSpan) {
  CopilotRerankFilter filter = MakeFilter();
  Segment normal = MakeSegment(0, 5, {{"你好", 5}}, 0);
  ASSERT_TRUE(filter.AppliesToSegment(&normal));
  ASSERT_TRUE(filter.pending_trace_span().has_value());

  Segment placeholder = MakeSegment(5, 8, {{"预测", 3}}, 0);
  placeholder.tags.insert("copilot");
  EXPECT_FALSE(filter.AppliesToSegment(&placeholder));
  EXPECT_FALSE(filter.pending_trace_span().has_value());
}

// A null segment leaves no span behind, so the following Apply() records no
// trace rather than keying one on whatever came before.
TEST(RerankFilterTraceSpan, ANullSegmentClearsThePendingSpan) {
  CopilotRerankFilter filter = MakeFilter();
  Segment seg = MakeSegment(0, 5, {{"你好", 5}}, 0);
  ASSERT_TRUE(filter.AppliesToSegment(&seg));
  ASSERT_TRUE(filter.pending_trace_span().has_value());
  EXPECT_TRUE(filter.AppliesToSegment(nullptr));
  EXPECT_FALSE(filter.pending_trace_span().has_value());
}

// `size()` saturates: the store is a bounded deque, so past kMaxEntries a
// Record() drops the oldest and the count stops moving. replay_copilot.cc used
// a size() DELTA to decide "a fresh trace landed during that call", which is
// therefore blind from the 16th entry on -- measured on the real corpus, every
// segment past 16 fed keys came back "notrace", 100% of 148. A monotonic count
// of Record() calls is what that detector actually needs.
TEST(RerankTraceStore, CountsEveryRecordEvenOnceSizeSaturates) {
  RerankTraceStore store;
  const size_t n = RerankTraceStore::kMaxEntries + 8;
  for (size_t i = 0; i < n; ++i) {
    store.Record(At("input", i, i + 1, "x"));
  }
  EXPECT_EQ(store.size(), RerankTraceStore::kMaxEntries);  // saturated, as before
  EXPECT_EQ(store.records(), n);                           // and still counting
}

// The other half of the same blindness: Apply() rebuilds a segment's
// translation on every keystroke, and re-recording the SAME (input, span)
// replaces in place without growing the store at all -- so even below
// kMaxEntries a size() delta misses it.
TEST(RerankTraceStore, CountsAReRecordThatReplacesInPlace) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  EXPECT_EQ(store.records(), 1u);
  store.Record(At("nihao", 0, 2, "妮"));
  EXPECT_EQ(store.size(), 1u);  // replaced, not appended
  EXPECT_EQ(store.records(), 2u);
}

// An invalid trace is not recorded, so it must not be counted either -- a
// caller reading the delta would otherwise be told a decision it cannot find.
TEST(RerankTraceStore, DoesNotCountAnInvalidTrace) {
  RerankTraceStore store;
  RerankTrace invalid;
  invalid.valid = false;
  store.Record(invalid);
  EXPECT_EQ(store.records(), 0u);
  EXPECT_EQ(store.Last(), nullptr);
}

// Last() claims to be "the most recently recorded entry". With an in-place
// replacement it was returning entries_.back() instead, which is a DIFFERENT
// segment -- so replay_copilot attributed one segment's decision to another
// whenever the newest write was a replacement.
TEST(RerankTraceStore, LastIsTheMostRecentlyRecordedNotTheBackOfTheDeque) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  store.Record(At("nihao", 2, 5, "好"));
  store.Record(At("nihao", 0, 2, "妮"));  // replaces the FIRST entry in place
  ASSERT_NE(store.Last(), nullptr);
  EXPECT_EQ(store.Last()->start, 0u);
  EXPECT_EQ(store.Last()->end, 2u);
}

// records() must not reset: the delta is read across calls that may contain a
// commit, and a commit clears the store. Resetting would make the delta
// negative and read as "nothing recorded" -- the exact failure being fixed.
TEST(RerankTraceStore, ClearDropsEntriesButNotTheRecordCount) {
  RerankTraceStore store;
  store.Record(At("nihao", 0, 2, "你"));
  store.Clear();
  EXPECT_EQ(store.size(), 0u);
  EXPECT_EQ(store.records(), 1u);
}

// Segment 0's blind spot (replay_copilot.cc): ProcessRequest feeds every key of
// a request BEFORE the loop that captures the records() count, so segment 0's
// trace is already written by the time that window opens and the delta is zero.
// Moving the capture earlier would make Last() name whichever segment Apply()
// ran for last on the final keystroke -- a later one, in any multi-segment
// composition. Looking the segment up by its span start instead needs no
// window at all, and start is 0 for segment 0 under every segmentation, so it
// needs no unit conversion between the tool's key positions and librime's
// input offsets either.
TEST(RerankTraceStore, FindsTheLatestTraceForASpanStart) {
  RerankTraceStore store;
  store.Record(At("ni", 0, 2, "你"));
  store.Record(At("niha", 0, 2, "妮"));  // same segment, later keystroke
  store.Record(At("niha", 2, 4, "哈"));  // a LATER segment, recorded last
  const RerankTrace* found = store.FindLatestByStart(0);
  ASSERT_NE(found, nullptr);
  EXPECT_EQ(found->start, 0u);
  EXPECT_EQ(found->input, "niha");  // the newest of the two, not the oldest
}

TEST(RerankTraceStore, FindLatestByStartIgnoresOtherSegments) {
  RerankTraceStore store;
  store.Record(At("niha", 2, 4, "哈"));
  EXPECT_EQ(store.FindLatestByStart(0), nullptr);
}

// Every keystroke records a fresh (input, span) triple, so a composition longer
// than kMaxEntries evicts segment 0's EARLY entries. Its latest one must
// survive that -- which it does, because the newest keystroke re-records every
// live segment. This is the case Last()-plus-delta could never reach.
TEST(RerankTraceStore, FindLatestByStartSurvivesEviction) {
  RerankTraceStore store;
  for (size_t i = 0; i < RerankTraceStore::kMaxEntries + 8; ++i) {
    store.Record(At("input" + std::to_string(i), 4, 6, "x"));
  }
  store.Record(At("final", 0, 2, "你"));  // segment 0, newest keystroke
  ASSERT_NE(store.FindLatestByStart(0), nullptr);
  EXPECT_EQ(store.FindLatestByStart(0)->input, "final");
}

TEST(RerankTraceStore, FindLatestByStartFindsNothingInAnEmptyStore) {
  RerankTraceStore store;
  EXPECT_EQ(store.FindLatestByStart(0), nullptr);
}
