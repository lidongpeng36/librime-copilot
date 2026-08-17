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
