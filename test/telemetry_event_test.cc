#include <gtest/gtest.h>

#include <nlohmann/json.hpp>

#include "telemetry_event.h"

using namespace rime::telemetry;

namespace {

Event MinimalEvent() {
  Event e;
  e.ts = "2026-08-14T10:23:45+0800";
  e.machine = "MacBookPro-M4Pro";
  e.schema = "double_pinyin_flypy";
  e.src = "tmux";
  e.input = "ui";
  e.ctx = "我们下次";
  e.sel_idx = 2;
  e.sel = "会";
  e.top = {"回", "汇", "会", "惠", "毁"};
  return e;
}

}  // namespace

TEST(TelemetryEvent, SerializesWithoutRerankBlock) {
  auto j = nlohmann::json::parse(SerializeJsonl(MinimalEvent()));
  EXPECT_EQ(j["v"], kSchemaVersion);
  EXPECT_EQ(j["machine"], "MacBookPro-M4Pro");
  EXPECT_EQ(j["ctx"], "我们下次");
  EXPECT_EQ(j["sel_idx"], 2);
  EXPECT_EQ(j["top"].size(), 5u);
  EXPECT_FALSE(j.contains("rr"));
}

TEST(TelemetryEvent, SerializesRerankBlockWhenPresent) {
  Event e = MinimalEvent();
  RerankRecord rr;
  rr.key = "次";
  rr.key_len = 1;
  rr.n = 3182;
  rr.text = "回";
  rr.from = 3;
  rr.rank = 12;
  rr.level = 3;
  e.rr = rr;
  auto j = nlohmann::json::parse(SerializeJsonl(e));
  ASSERT_TRUE(j.contains("rr"));
  EXPECT_EQ(j["rr"]["key"], "次");
  EXPECT_EQ(j["rr"]["n"], 3182);
  EXPECT_EQ(j["rr"]["from"], 3);
  EXPECT_EQ(j["rr"]["level"], 3);
}

// The file is read line by line. A literal newline inside a field would split
// one event into two unparseable halves.
TEST(TelemetryEvent, EmitsExactlyOneLine) {
  Event e = MinimalEvent();
  e.sel = "a\nb";
  e.ctx = "quote\" and backslash\\";
  const std::string line = SerializeJsonl(e);
  EXPECT_EQ(line.find('\n'), std::string::npos);
  auto j = nlohmann::json::parse(line);
  EXPECT_EQ(j["sel"], "a\nb");
  EXPECT_EQ(j["ctx"], "quote\" and backslash\\");
}

// Chinese must stay readable in the file, not become \uXXXX escapes.
TEST(TelemetryEvent, KeepsChineseLiteral) {
  EXPECT_NE(SerializeJsonl(MinimalEvent()).find("我们下次"), std::string::npos);
}

// The recording scope from the spec: re-ranking promoted something, or the
// user did not take the first candidate. Ordinary typing is not recorded.
TEST(TelemetryEvent, ShouldRecordMatchesTheScope) {
  EXPECT_FALSE(ShouldRecord(0, false, 0, 0));  // ordinary typing
  EXPECT_TRUE(ShouldRecord(0, true, 0, 0));    // promotion accepted
  EXPECT_TRUE(ShouldRecord(3, true, 0, 0));    // promotion rejected
  EXPECT_TRUE(ShouldRecord(3, false, 0, 0));   // translator mis-ranking
}

TEST(SerializeJsonl, IncludesTheLlmRecordWhenPresent) {
  Event e;
  e.ts = "2026-08-17T10:00:00+08:00";
  e.sel = "故意";
  LlmRecord r;
  r.text = "故意";
  r.incumbent = "顾忌";
  r.from = 2;
  r.margin = 3.4f;
  r.n_scored = 4;
  r.us = 4100;
  r.skip = "none";
  e.llm = r;
  const std::string line = SerializeJsonl(e);
  EXPECT_NE(line.find("\"llm\""), std::string::npos);
  EXPECT_NE(line.find("\"incumbent\":\"顾忌\""), std::string::npos);
  EXPECT_NE(line.find("\"margin\":3.4"), std::string::npos);
}

TEST(SerializeJsonl, OmitsTheLlmRecordWhenAbsent) {
  Event e;
  e.ts = "t";
  EXPECT_EQ(SerializeJsonl(e).find("\"llm\""), std::string::npos);
}

// Named for the constant rather than a number: the literal spelling went stale
// twice (the name still said Four while the body asserted 5), and a test whose
// name contradicts its body is worse than none -- a reader trusts the name.
TEST(SerializeJsonl, EveryLineCarriesTheCurrentSchemaVersion) {
  Event e;
  e.ts = "t";
  auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["v"], kSchemaVersion);
}

// The per-event stream only records hard cases (ShouldRecord), which is right
// for judging quality and wrong for measuring warm-hit rate -- its denominator
// is ordinary typing, which is never recorded. Hence a second line type.
TEST(SerializeStatsJsonl, CarriesCountersAndItsOwnType) {
  StatsLine s;
  s.ts = "2026-08-17T10:00:00+08:00";
  s.segments = 8421;
  s.llm_acted = 5310;
  s.skip_counts["cold"] = 519;
  s.us_p50 = 4.1;
  s.us_p95 = 11.8;
  const std::string line = SerializeStatsJsonl(s);
  EXPECT_NE(line.find("\"type\":\"stats\""), std::string::npos);
  EXPECT_NE(line.find("\"llm_acted\":5310"), std::string::npos);
  EXPECT_NE(line.find("\"cold\":519"), std::string::npos);
  EXPECT_EQ(nlohmann::json::parse(line)["v"], kSchemaVersion);
}

// No stored `warm_hit`: under the fallback chain kCold and kNone are mutually
// exclusive (rerank_filter.cc), so a segment is never scored while cold, and
// a warm-hit counter would just be a second name for `llm_acted`. Warm-hit
// rate is `llm_acted / (llm_acted + skip_counts["cold"])`, left to the
// analyser rather than duplicated here.
TEST(SerializeStatsJsonl, HasNoWarmHitField) {
  StatsLine s;
  s.ts = "t";
  s.llm_acted = 1;
  s.skip_counts["cold"] = 1;
  EXPECT_EQ(SerializeStatsJsonl(s).find("warm_hit"), std::string::npos);
}

// v5. The unbiased half of the truncation question: `trunc` on an Event is
// sampled (ShouldRecord), these counters see every segment.
TEST(SerializeStatsJsonl, CarriesWhyEachFetchStoppedAndHowDeepTheTruncatedOnesGot) {
  StatsLine s;
  s.ts = "t";
  s.trunc_counts["screen"] = 812;
  s.trunc_counts["full"] = 240;
  s.depth_p50 = 3.0;
  s.depth_p95 = 6.0;
  const std::string line = SerializeStatsJsonl(s);
  EXPECT_NE(line.find("\"screen\":812"), std::string::npos);
  EXPECT_NE(line.find("\"depth_p50\":3.0"), std::string::npos);
  EXPECT_NE(line.find("\"depth_p95\":6.0"), std::string::npos);
}

// A window with no environment-truncated fetch OMITS the depth rather than
// writing 0.0, so "nothing ran out" and "everything ran out at zero
// characters" stay distinguishable -- the same choice Event makes for
// `before_depth`/`trunc`, and what lets a reader trust a depth of 0.
TEST(SerializeStatsJsonl, OmitsTheDepthWhenNothingWasTruncatedByTheEnvironment) {
  StatsLine s;
  s.ts = "t";
  s.trunc_counts["full"] = 12;
  const std::string line = SerializeStatsJsonl(s);
  EXPECT_EQ(line.find("depth_p50"), std::string::npos);
  EXPECT_EQ(line.find("depth_p95"), std::string::npos);
}

// A decline must carry what the model wanted. This is the `nali` case from
// the first hour of live data: incumbent 哪里, the user chose 那里, margin
// 0.88 -- and until now the record could not say whether 那里 was what the
// model preferred, which is the difference between "lower the margin" and
// "the model is wrong".
TEST(SerializeJsonl, CarriesTheModelsPickWhenNothingWasPromoted) {
  Event e;
  e.ts = "2026-08-20T22:18:09+0800";
  e.machine = "M1";
  e.sel = "那里";
  e.sel_idx = 1;
  LlmRecord llm;
  llm.incumbent = "哪里";
  llm.best = "那里";
  llm.best_from = 1;
  llm.margin = 0.8799f;
  llm.n_scored = 3;
  llm.skip = "margin";
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["v"], kSchemaVersion);
  EXPECT_EQ(j["llm"]["best"], "那里");
  EXPECT_EQ(j["llm"]["best_from"], 1);
  EXPECT_EQ(j["llm"]["text"], "");  // nothing was promoted
  EXPECT_EQ(j["llm"]["skip"], "margin");
}

// v7's latency split. `us` alone could not tell "the model was slow" from "the
// call waited for a background prefill to release the model lock", and those
// have opposite fixes -- see ScoreTiming (scorer.h) for the live 11.1ms
// against bench_scorer's 2.1ms that made the distinction worth recording.
TEST(SerializeJsonl, CarriesTheLatencySplitAndWhetherTheBatchDecoded) {
  Event e;
  e.ts = "2026-09-04T12:00:00+0800";
  e.machine = "M4Pro";
  e.sel = "那里";
  LlmRecord llm;
  llm.incumbent = "哪里";
  llm.best = "那里";
  llm.us = 11400;
  llm.lock_us = 9100;
  llm.work_us = 2200;
  llm.n_decoded = 4;
  llm.skip = "none";
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  // `us` keeps its meaning: the whole Score() call, which is what the p99
  // budget is written against and what every pre-v7 figure measures. The split
  // is added beside it, not in place of it.
  EXPECT_EQ(j["llm"]["us"], 11400);
  EXPECT_EQ(j["llm"]["lock_us"], 9100);
  EXPECT_EQ(j["llm"]["work_us"], 2200);
  EXPECT_EQ(j["llm"]["n_decoded"], 4);
}

// A batch of single-token candidates submits nothing to llama_decode and costs
// ~0.18ms against ~11ms for one that decodes. That zero is the measurement --
// the cheap mode this field exists to identify -- so it must reach the line,
// where an unmeasured field is omitted entirely.
TEST(SerializeJsonl, ADecodelessBatchRecordsZeroRatherThanNothing) {
  Event e;
  e.ts = "2026-09-04T12:00:00+0800";
  LlmRecord llm;
  llm.us = 180;
  llm.lock_us = 0;
  llm.work_us = 175;
  llm.n_decoded = 0;
  llm.skip = "none";
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_TRUE(j["llm"].contains("n_decoded"));
  EXPECT_EQ(j["llm"]["n_decoded"], 0);
  // Same rule for an uncontended lock: 0 microseconds of waiting is a finding.
  EXPECT_TRUE(j["llm"].contains("lock_us"));
  EXPECT_EQ(j["llm"]["lock_us"], 0);
}

// The other half of that rule. A record the scorer never filled leaves all
// three at -1, and -1 must not reach the log: a reader would have to know the
// sentinel, and "absent" already means "not measured" for every field added
// since v3. This is also what a v6 line looks like, so one rule covers both.
TEST(SerializeJsonl, OmitsTheLatencySplitWhenItWasNeverMeasured) {
  Event e;
  e.ts = "2026-09-04T12:00:00+0800";
  LlmRecord llm;
  llm.us = 0;
  llm.skip = "cold";
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_FALSE(j["llm"].contains("lock_us"));
  EXPECT_FALSE(j["llm"].contains("work_us"));
  EXPECT_FALSE(j["llm"].contains("n_decoded"));
  EXPECT_TRUE(j["llm"].contains("us"));  // unchanged, always written
}

// Agreement is a value, not an absence: best == incumbent is how the report
// tells a model-quality problem from a threshold one.
TEST(SerializeJsonl, RecordsAgreementWithTheHeadAsAValue) {
  Event e;
  LlmRecord llm;
  llm.incumbent = "阅读";
  llm.best = "阅读";
  llm.best_from = 0;
  llm.margin = 0.0f;
  llm.skip = "margin";
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["llm"]["best"], j["llm"]["incumbent"]);
  EXPECT_EQ(j["llm"]["best_from"], 0);
}

TEST(SerializeJsonl, CarriesTheSpanGatesLeftovers) {
  Event e;
  LlmRecord llm;
  llm.incumbent = "管理业";
  llm.best = "管理业";
  llm.skip = "margin";
  llm.dropped = {"管理", "惯例"};
  e.llm = llm;

  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["llm"]["dropped"].size(), 2u);
  EXPECT_EQ(j["llm"]["dropped"][0], "管理");
}

TEST(SerializeJsonl, WritesAnEmptyDroppedArrayRatherThanOmittingIt) {
  // Absent and empty are different evidence: absent is a v2 line, empty is a
  // v3 line where the gate removed nothing (or same_span_only is off).
  Event e;
  e.llm = LlmRecord{};
  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  ASSERT_TRUE(j["llm"].contains("dropped"));
  EXPECT_TRUE(j["llm"]["dropped"].empty());
}

TEST(SerializeJsonl, CarriesWhyTheModelDidNotEngage) {
  Event e;
  e.sel = "页";
  e.sel_idx = 1;
  e.llm_skip = "noctx";
  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["llm_skip"], "noctx");
  EXPECT_FALSE(j.contains("llm"));  // never engaged: no llm object at all
}

// Omitted rather than written empty: "no trace at all" and "a trace saying
// none" are different evidence, and an empty string is not one of the eight
// SkipReasonName values.
TEST(SerializeJsonl, OmitsLlmSkipWhenThereWasNoTrace) {
  Event e;
  e.sel = "的";
  const auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_FALSE(j.contains("llm_skip"));
}

// Hard cases are unconditional -- the accuracy number depends on misses being
// recorded in full, with no sampling and no scaling.
TEST(ShouldRecord, KeepsEveryMissAndEveryPromotionRegardlessOfSampling) {
  EXPECT_TRUE(ShouldRecord(3, false, 0, 0));
  EXPECT_TRUE(ShouldRecord(3, false, 7, 20));
  EXPECT_TRUE(ShouldRecord(0, true, 7, 20));
}

// Default off: a machine that upgrades records exactly what it records today
// until the key is set.
TEST(ShouldRecord, DropsPlainSuccessesWhenSamplingIsOff) {
  EXPECT_FALSE(ShouldRecord(0, false, 0, 0));
  EXPECT_FALSE(ShouldRecord(0, false, 99, 0));
}

TEST(ShouldRecord, KeepsOneSuccessInN) {
  EXPECT_TRUE(ShouldRecord(0, false, 0, 3));
  EXPECT_FALSE(ShouldRecord(0, false, 1, 3));
  EXPECT_FALSE(ShouldRecord(0, false, 2, 3));
  EXPECT_TRUE(ShouldRecord(0, false, 3, 3));
}

TEST(ShouldRecord, SampleOkOfOneKeepsEverySuccess) {
  EXPECT_TRUE(ShouldRecord(0, false, 0, 1));
  EXPECT_TRUE(ShouldRecord(0, false, 1, 1));
}

TEST(TelemetryEvent, SerializesFetchDepthAndTruncation) {
  Event e;
  e.ts = "2026-08-23T10:00:00+08:00";
  e.machine = "m";
  e.schema = "double_pinyin";
  e.src = "tmux";
  e.before_depth = 17;
  e.trunc = "config";

  auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_EQ(j["v"], kSchemaVersion);
  EXPECT_EQ(j["before_depth"], 17);
  EXPECT_EQ(j["trunc"], "config");
}

// An event with no trace behind it must not claim a depth of zero as if it had
// measured one: "unknown" is a value, 0 is a measurement.
TEST(TelemetryEvent, OmitsFetchDepthWhenNeverMeasured) {
  Event e;
  e.ts = "2026-08-23T10:00:00+08:00";
  auto j = nlohmann::json::parse(SerializeJsonl(e));
  EXPECT_FALSE(j.contains("trunc"));
  EXPECT_FALSE(j.contains("before_depth"));
}

// v6: `fetch_chars`. It exists because `trunc_counts["config"]` changed meaning
// under a fixed key -- it counted fetches cut at 8 until context_chars became a
// term in SurroundingPrefixChars, and counts fetches cut at the configured
// depth after. A count of "the cap bound" is uninterpretable without the cap.
TEST(SerializeStatsJsonl, FetchCharsIsWrittenWhenRecorded) {
  rime::telemetry::StatsLine s;
  s.ts = "2026-08-28T10:00:00+0800";
  s.segments = 40;
  s.fetch_chars = 32;
  auto j = nlohmann::json::parse(rime::telemetry::SerializeStatsJsonl(s));
  EXPECT_EQ(j["fetch_chars"], 32);
}

// Absent, not zero: 0 is not a depth any build ever fetched, so writing it
// would invent a state. An absent field says "predates the wiring", which is
// itself the answer -- the depth was 8.
TEST(SerializeStatsJsonl, AnUnrecordedFetchDepthIsOmittedNotZeroed) {
  rime::telemetry::StatsLine s;
  s.ts = "2026-08-28T10:00:00+0800";
  s.segments = 40;
  auto j = nlohmann::json::parse(rime::telemetry::SerializeStatsJsonl(s));
  EXPECT_FALSE(j.contains("fetch_chars"));
}
