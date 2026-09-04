// StatsAccumulator driven directly, the same way rerank_llm_test.cc drives
// llm_rerank::Decide -- no Context, no Rime engine, since Observe() only
// needs a RerankTrace* (see telemetry_stats.h).

#include <gtest/gtest.h>

#include "telemetry_stats.h"

using namespace rime;
using namespace rime::telemetry;

namespace {

RerankTrace EngagedTrace(const std::string& promoted, int64_t us) {
  RerankTrace t;
  t.valid = true;
  t.llm_skip = llm_rerank::SkipReason::kNone;
  t.llm.text = promoted;
  t.llm.us = us;
  return t;
}

RerankTrace SkippedTrace(llm_rerank::SkipReason reason) {
  RerankTrace t;
  t.valid = true;
  t.llm_skip = reason;
  return t;
}

// A trace whose surrounding fetch reached `before_depth` characters and
// stopped for `truncation`. Every real source sets the two together
// (tmux_source_util.h:606-611, ime_bridge.cc:593-598).
RerankTrace FetchTrace(int before_depth, Truncation truncation) {
  RerankTrace t;
  t.valid = true;
  t.llm_skip = llm_rerank::SkipReason::kNone;
  t.llm.us = 1000;
  t.before_depth = before_depth;
  t.truncation = truncation;
  return t;
}

// v8. Whether an incremental prefill would apply is not a property anything in
// this tree could see: both backends wipe the KV cache and re-decode the whole
// context on every warm, so "how often does the new context merely extend the
// old one" had no answer at all.
TEST(StatsWarm, AnExtensionIsDistinguishedFromARebuild) {
  StatsAccumulator a;
  a.ObserveWarm("我们今天", "");            // first ever -> nothing to extend
  a.ObserveWarm("我们今天要", "我们今天");  // one character appended
  a.ObserveWarm("完全不同的文字", "我们今天要");
  const auto s = a.Snapshot("t");
  EXPECT_EQ(s.warm_counts.at("extend"), 1);
  EXPECT_EQ(s.warm_counts.at("rebuild"), 2);  // the first one has no predecessor
  EXPECT_EQ(s.warm_extend_chars_p50, 1.0);
}

// The class that costs nothing and must not be counted as work: WarmUp returns
// early for a context already hot, so a repeat is not a prefill an incremental
// path would make cheaper. Counting it as `extend` would overstate the win.
TEST(StatsWarm, TheSameContextTwiceIsDedupNotExtend) {
  StatsAccumulator a;
  a.ObserveWarm("我们今天", "我们今天");
  const auto s = a.Snapshot("t");
  EXPECT_EQ(s.warm_counts.at("dedup"), 1);
  EXPECT_EQ(s.warm_counts.count("extend"), 0u);
}

// Characters, not bytes. Every context here is Han, where one character is
// three bytes -- a byte count would report 3 and make the appended-length
// distribution useless for sizing the incremental decode.
TEST(StatsWarm, TheAppendedLengthIsCountedInCharacters) {
  StatsAccumulator a;
  a.ObserveWarm("我们今天要讨论", "我们今天");
  const auto s = a.Snapshot("t");
  EXPECT_EQ(s.warm_extend_chars_p50, 3.0);
}

// A window with no warm at all must leave the map empty rather than writing
// zeroes: SerializeStatsJsonl omits the field entirely in that case, and "no
// warms" is a different fact from "warms that classified as nothing".
TEST(StatsWarm, AWindowWithNoWarmReportsNothing) {
  StatsAccumulator a;
  const auto s = a.Snapshot("t");
  EXPECT_TRUE(s.warm_counts.empty());
}

// Flush clears them, like every other counter here: a window is a window.
TEST(StatsWarm, TheCountersResetWithTheWindow) {
  StatsAccumulator a;
  a.ObserveWarm("abc", "ab");
  a.Snapshot("t");
  a.Reset();
  const auto s = a.Snapshot("t");
  EXPECT_TRUE(s.warm_counts.empty());
}

}  // namespace

TEST(Percentile, MatchesCompareRerankPysDefinition) {
  // sorted[min(n-1, floor(n*p))] -- tools/rime_corpus/compare_rerank.py's
  // `_percentile`, reproduced here so a live and an offline p50/p95 are the
  // same statistic.
  std::vector<int64_t> v = {10, 20, 30, 40, 50};
  EXPECT_EQ(Percentile(v, 0.5), 30);   // idx = min(4, floor(2.5)) = 2 -> 30
  EXPECT_EQ(Percentile(v, 0.95), 50);  // idx = min(4, floor(4.75)) = 4 -> 50
}

TEST(Percentile, EmptyIsZero) { EXPECT_EQ(Percentile({}, 0.5), 0.0); }

TEST(StatsAccumulator, CountsEverySegmentRegardlessOfOutcome) {
  StatsAccumulator acc;
  acc.Observe(nullptr);
  acc.Observe(nullptr);
  EXPECT_EQ(acc.segments(), 2);
}

TEST(StatsAccumulator, NullTraceCountsOnlyTheDenominator) {
  StatsAccumulator acc;
  acc.Observe(nullptr);
  auto s = acc.Snapshot("t");
  EXPECT_EQ(s.segments, 1);
  EXPECT_EQ(s.llm_acted, 0);
  EXPECT_TRUE(s.skip_counts.empty());
}

TEST(StatsAccumulator, EngagedTraceCountsLlmActedAndItsLatency) {
  StatsAccumulator acc;
  RerankTrace t = EngagedTrace("故意", 4100);
  acc.Observe(&t);
  auto s = acc.Snapshot("t");
  EXPECT_EQ(s.segments, 1);
  EXPECT_EQ(s.llm_acted, 1);
  EXPECT_EQ(s.us_p50, 4100);
  EXPECT_EQ(s.us_p95, 4100);
}

TEST(StatsAccumulator, DeclinedIsStillEngaged) {
  // An empty `text` (declined -- kNoHan/kMargin) is still llm_skip == kNone:
  // the scorer ran. Promotion vs decline is Event::llm's business
  // (telemetry_commit.cc), not the aggregate counters'.
  StatsAccumulator acc;
  RerankTrace t = EngagedTrace(/*promoted=*/"", 900);
  acc.Observe(&t);
  EXPECT_EQ(acc.Snapshot("t").llm_acted, 1);
}

TEST(StatsAccumulator, NonEngagedTraceCountsItsSkipReason) {
  StatsAccumulator acc;
  RerankTrace cold = SkippedTrace(llm_rerank::SkipReason::kCold);
  RerankTrace battery = SkippedTrace(llm_rerank::SkipReason::kBattery);
  RerankTrace cold2 = SkippedTrace(llm_rerank::SkipReason::kCold);
  acc.Observe(&cold);
  acc.Observe(&battery);
  acc.Observe(&cold2);
  auto s = acc.Snapshot("t");
  EXPECT_EQ(s.segments, 3);
  EXPECT_EQ(s.llm_acted, 0);
  EXPECT_EQ(s.skip_counts.at("cold"), 2);
  EXPECT_EQ(s.skip_counts.at("battery"), 1);
}

TEST(StatsAccumulator, SnapshotCarriesTheGivenTimestamp) {
  StatsAccumulator acc;
  EXPECT_EQ(acc.Snapshot("2026-08-17T10:00:00+0800").ts, "2026-08-17T10:00:00+0800");
}

TEST(StatsAccumulator, ResetClearsEverything) {
  StatsAccumulator acc;
  RerankTrace t = EngagedTrace("故意", 100);
  RerankTrace c = SkippedTrace(llm_rerank::SkipReason::kCold);
  acc.Observe(&t);
  acc.Observe(&c);
  acc.Reset();
  EXPECT_EQ(acc.segments(), 0);
  auto s = acc.Snapshot("t");
  EXPECT_EQ(s.llm_acted, 0);
  EXPECT_TRUE(s.skip_counts.empty());
  EXPECT_EQ(s.us_p50, 0.0);
}

// warm_hit has no counterpart here on purpose: under the fallback chain
// (rerank_filter.cc) kCold and kNone are mutually exclusive, so this
// accumulator has no path that would populate a "scored while cold" bucket
// even if one existed. See telemetry_event.h's StatsLine comment.
TEST(StatsAccumulator, HasNoWarmHitConcept) {
  StatsAccumulator acc;
  RerankTrace t = EngagedTrace("故意", 100);
  acc.Observe(&t);
  EXPECT_EQ(acc.Snapshot("t").skip_counts.count("cold"), 0u);
}

// The other half of the kNone-without-scoring defect: Observe() reads
// llm_skip as "the model ran", so a mislabelled trace was counted in
// llm_acted AND pushed its us=0 into the latency samples, inflating the
// engagement rate and dragging p50/p95 down at the same time.
TEST(StatsAccumulator, ASegmentThatWasNeverScoredIsNeitherActedNorTimed) {
  StatsAccumulator acc;
  acc.Observe(nullptr);  // no trace at all
  auto skipped = SkippedTrace(llm_rerank::SkipReason::kNoContext);
  acc.Observe(&skipped);
  auto engaged = EngagedTrace("故意", 7000);
  acc.Observe(&engaged);

  const auto s = acc.Snapshot("2026-08-20T23:00:00+0800");
  EXPECT_EQ(s.segments, 3);
  EXPECT_EQ(s.llm_acted, 1);
  EXPECT_EQ(s.skip_counts.at("noctx"), 1);
  EXPECT_DOUBLE_EQ(s.us_p50, 7000.0);  // the un-scored segment contributed no 0
}

// --- how deep the surrounding fetch reached, and why it stopped -------------
//
// These exist because `before_depth`/`trunc` on Event are SAMPLED: ShouldRecord
// keeps every miss and promotion but only 1 in `sample_ok` plain successes, so
// a truncation distribution computed from the event stream is biased toward
// hard cases. The stats line observes every segment, which is the only
// unbiased place this can live.

TEST(StatsAccumulator, CountsWhyEachFetchStopped) {
  StatsAccumulator acc;
  RerankTrace screen1 = FetchTrace(3, Truncation::kByScreen);
  RerankTrace screen2 = FetchTrace(2, Truncation::kByScreen);
  RerankTrace full = FetchTrace(5, Truncation::kFull);
  acc.Observe(&screen1);
  acc.Observe(&screen2);
  acc.Observe(&full);
  const auto s = acc.Snapshot("t");
  EXPECT_EQ(s.trunc_counts.at("screen"), 2);
  EXPECT_EQ(s.trunc_counts.at("full"), 1);
}

// kUnknown gets a bucket rather than being dropped. It is the honest answer
// for the IME Bridge (imk_client.h), and a window that is mostly "unknown"
// means the truncation question cannot be answered from that data at all --
// which has to be visible, not silently missing from the denominator.
TEST(StatsAccumulator, AnUnknownTruncationIsItsOwnBucketNotASilentDrop) {
  StatsAccumulator acc;
  RerankTrace t = FetchTrace(8, Truncation::kUnknown);
  acc.Observe(&t);
  EXPECT_EQ(acc.Snapshot("t").trunc_counts.at("unknown"), 1);
}

// The same rule skip_counts follows: which of the five answers applies is not
// recoverable from "no trace at all", and guessing would misattribute.
TEST(StatsAccumulator, NullTraceCountsNoTruncationBucket) {
  StatsAccumulator acc;
  acc.Observe(nullptr);
  EXPECT_TRUE(acc.Snapshot("t").trunc_counts.empty());
}

// The depth summary answers exactly one question -- "when the source ran out,
// how deep did it get" -- so only the two truncations that MEAN "ran out"
// feed it. kByConfig's depth is the configured cap (a constant, no
// information); kFull's is the length of an input region that ended on its
// own, which is not a limit at all. Mixing either in would report a ceiling
// that is not one.
TEST(StatsAccumulator, OnlyAnEnvironmentTruncatedFetchFeedsTheDepthSummary) {
  StatsAccumulator acc;
  RerankTrace screen = FetchTrace(3, Truncation::kByScreen);
  RerankTrace app = FetchTrace(3, Truncation::kByApp);
  RerankTrace config = FetchTrace(8, Truncation::kByConfig);
  RerankTrace full = FetchTrace(7, Truncation::kFull);
  RerankTrace unknown = FetchTrace(8, Truncation::kUnknown);
  acc.Observe(&screen);
  acc.Observe(&app);
  acc.Observe(&config);
  acc.Observe(&full);
  acc.Observe(&unknown);
  const auto s = acc.Snapshot("t");
  EXPECT_DOUBLE_EQ(s.depth_p50, 3.0);  // only the two 3s
  EXPECT_DOUBLE_EQ(s.depth_p95, 3.0);
}

TEST(StatsAccumulator, DepthPercentilesUseTheSameDefinitionAsTheLatencyOnes) {
  StatsAccumulator acc;
  std::vector<RerankTrace> traces;
  for (int d : {1, 2, 3, 4, 5}) {
    traces.push_back(FetchTrace(d, Truncation::kByScreen));
  }
  for (auto& t : traces) {
    acc.Observe(&t);
  }
  const auto s = acc.Snapshot("t");
  EXPECT_DOUBLE_EQ(s.depth_p50, 3.0);  // idx = min(4, floor(5*0.5)) = 2
  EXPECT_DOUBLE_EQ(s.depth_p95, 5.0);  // idx = min(4, floor(5*0.95)) = 4
}

// Negative is "no environment-truncated fetch was measured in this window",
// which is a different fact from a window where the source reached zero
// characters every time -- the same distinction Event::before_depth's -1
// draws, and the reason the serializer omits the field rather than writing 0.
TEST(StatsAccumulator, NoTruncatedFetchLeavesTheDepthUnreported) {
  StatsAccumulator acc;
  RerankTrace full = FetchTrace(7, Truncation::kFull);
  acc.Observe(&full);
  const auto s = acc.Snapshot("t");
  EXPECT_LT(s.depth_p50, 0.0);
  EXPECT_LT(s.depth_p95, 0.0);
}

TEST(StatsAccumulator, AMeasuredZeroIsASampleNotAnAbsence) {
  StatsAccumulator acc;
  RerankTrace screen = FetchTrace(0, Truncation::kByScreen);
  acc.Observe(&screen);
  EXPECT_DOUBLE_EQ(acc.Snapshot("t").depth_p50, 0.0);
}

// A source that reported a truncation but no depth still counts as that
// truncation. Only the depth is missing, and a -1 must not be averaged in as
// if the fetch had reached nothing.
TEST(StatsAccumulator, AnUnmeasuredDepthIsNotCountedAsZero) {
  StatsAccumulator acc;
  RerankTrace unmeasured = FetchTrace(-1, Truncation::kByScreen);
  RerankTrace measured = FetchTrace(4, Truncation::kByScreen);
  acc.Observe(&unmeasured);
  acc.Observe(&measured);
  const auto s = acc.Snapshot("t");
  EXPECT_EQ(s.trunc_counts.at("screen"), 2);
  EXPECT_DOUBLE_EQ(s.depth_p50, 4.0);
}

TEST(StatsAccumulator, ResetClearsTheTruncationCountsAndTheDepth) {
  StatsAccumulator acc;
  RerankTrace screen = FetchTrace(3, Truncation::kByScreen);
  acc.Observe(&screen);
  acc.Reset();
  const auto s = acc.Snapshot("t");
  EXPECT_TRUE(s.trunc_counts.empty());
  EXPECT_LT(s.depth_p50, 0.0);
}

// Passed at Snapshot time rather than accumulated, so Reset() has nothing to
// preserve -- a stored copy is the shape of bug that invites.
TEST(StatsAccumulator, SnapshotStampsTheFetchDepthItIsGiven) {
  telemetry::StatsAccumulator acc;
  acc.Observe(nullptr);
  EXPECT_EQ(acc.Snapshot("t", 32).fetch_chars, 32);
  acc.Reset();
  acc.Observe(nullptr);
  EXPECT_EQ(acc.Snapshot("t", 32).fetch_chars, 32);
}

TEST(StatsAccumulator, AnUngivenFetchDepthStaysUnrecorded) {
  telemetry::StatsAccumulator acc;
  acc.Observe(nullptr);
  EXPECT_LT(acc.Snapshot("t").fetch_chars, 0);
}
