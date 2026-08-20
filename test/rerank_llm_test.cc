#include <gtest/gtest.h>

#include "rerank_llm.h"

using namespace rime::llm_rerank;
using rime::LlmRerankOptions;

namespace {

LlmRerankOptions Opts() {
  LlmRerankOptions o;
  o.top_n = 4;
  o.margin = 2.0f;
  o.length_exponent = 0.7f;
  return o;
}

}  // namespace

TEST(IsAllHan, AcceptsHanRejectsEverythingElse) {
  EXPECT_TRUE(IsAllHan("故意"));
  EXPECT_TRUE(IsAllHan("的"));
  EXPECT_FALSE(IsAllHan("guyide"));  // the raw input candidate
  EXPECT_FALSE(IsAllHan("故意de"));  // mixed
  EXPECT_FALSE(IsAllHan("，"));      // CJK punctuation is not Han
  EXPECT_FALSE(IsAllHan(""));
}

// The incumbent is the first ALL-HAN candidate, never cands[0]. 39.6% of real
// segments have the raw input at 0 because RawInputFilter puts it there on
// purpose; comparing against it would make the model fight that policy.
TEST(Decide, IncumbentIsTheFirstHanCandidateNotIndexZero) {
  std::vector<std::string> cands{"guyide", "顾忌", "故意"};
  std::vector<float> lp{-1.0f, -20.0f, -1.0f};
  std::vector<int> nt{3, 2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.incumbent_index, 1);
  EXPECT_EQ(d.promote_index, 2);
}

TEST(Decide, NeverPromotesANonHanCandidate) {
  // The raw input scores best; it must still not be chosen.
  std::vector<std::string> cands{"顾忌", "guyide"};
  std::vector<float> lp{-20.0f, -0.1f};
  std::vector<int> nt{2, 3};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kMargin);
}

TEST(Decide, DeclinesWhenNoHanCandidateExists) {
  std::vector<std::string> cands{"guyide", "abc"};
  std::vector<float> lp{-1.0f, -2.0f};
  std::vector<int> nt{3, 1};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kNoHan);
}

TEST(Decide, DeclinesBelowTheMargin) {
  std::vector<std::string> cands{"顾忌", "故意"};
  std::vector<float> lp{-10.0f, -11.0f};  // challenger is worse
  std::vector<int> nt{2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kMargin);
}

// score = logprob / n_tokens^0.7, NOT the mean. Measured: the mean nets
// +12.0%, the 0.7 exponent nets +13.1%, the raw sum +9.2%.
TEST(Decide, UsesTheLengthExponentNotTheMean) {
  // Two candidates, equal raw logprob, different lengths. Under the mean the
  // longer one wins by a factor of 3; under ^0.7 by 3^0.7 ~= 2.16. Pick
  // numbers where the two rules disagree about crossing the margin.
  std::vector<std::string> cands{"的", "故意的"};
  std::vector<float> lp{-6.0f, -6.0f};
  std::vector<int> nt{1, 3};
  auto d = Decide(cands, lp, nt, Opts());
  // -6/1^0.7 = -6.0 ; -6/3^0.7 = -2.78 ; margin = 3.22 >= 2.0
  EXPECT_EQ(d.promote_index, 1);
  EXPECT_NEAR(d.margin, 3.22f, 0.05f);
}

TEST(Decide, ScoresAtMostTopN) {
  // The gold sits at index 5, outside top_n=4, so it is never considered.
  std::vector<std::string> cands{"顾忌", "估计", "故一", "古意", "孤意", "故意"};
  std::vector<float> lp{-20.0f, -20.0f, -20.0f, -20.0f, -20.0f, -0.1f};
  std::vector<int> nt{2, 2, 2, 2, 2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.n_scored, 4);
  EXPECT_EQ(d.promote_index, -1);
}

TEST(Decide, PromotingTheIncumbentItselfIsNotAPromotion) {
  std::vector<std::string> cands{"故意", "顾忌"};
  std::vector<float> lp{-0.1f, -20.0f};
  std::vector<int> nt{2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kMargin);
}

// The enum and the wire format must not drift. Telemetry serializes these
// exact strings (schema v2); a mapping defined in the telemetry header would
// let someone add an enumerator without a string.
TEST(SkipReasonName, CoversEveryEnumerator) {
  EXPECT_STREQ(SkipReasonName(SkipReason::kNone), "none");
  EXPECT_STREQ(SkipReasonName(SkipReason::kDisabled), "disabled");
  EXPECT_STREQ(SkipReasonName(SkipReason::kBattery), "battery");
  EXPECT_STREQ(SkipReasonName(SkipReason::kNoModel), "nomodel");
  EXPECT_STREQ(SkipReasonName(SkipReason::kNoContext), "noctx");
  EXPECT_STREQ(SkipReasonName(SkipReason::kCold), "cold");
  EXPECT_STREQ(SkipReasonName(SkipReason::kNoHan), "nohan");
  EXPECT_STREQ(SkipReasonName(SkipReason::kMargin), "margin");
}

TEST(Decide, HandlesEmptyInput) {
  auto d = Decide({}, {}, {}, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kNoHan);
  EXPECT_EQ(d.n_scored, 0);
}

// Decide is a public pure function; a caller passing mismatched parallel
// vectors must get a truncated decision, not an out-of-bounds read. ASAN in
// CI would catch the read, but only on the day it happens.
TEST(Decide, TruncatesToTheShortestParallelVector) {
  std::vector<std::string> cands{"顾忌", "故意", "估计"};
  std::vector<float> lp{-20.0f, -0.1f, -0.1f};
  std::vector<int> nt{2};  // deliberately short
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.n_scored, 1);
  EXPECT_EQ(d.incumbent_index, 0);
  EXPECT_EQ(d.promote_index, -1);
}

// `best` is what the model preferred, whether or not it cleared the margin.
// Without it a decline records how far short something fell but not what it
// was, and "the model agreed with the head" cannot be told apart from "the
// threshold blocked the model's pick" -- two findings whose fixes are
// opposite (lower the margin vs. retrain).
TEST(Decide, BestIndexIsSetWhenTheMarginBlocksThePromotion) {
  std::vector<std::string> cands{"哪里", "那里"};
  std::vector<float> lp{-4.0f, -3.5f};  // 那里 scores better, but not by 2.0
  std::vector<int> nt{2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, -1);
  EXPECT_EQ(d.skip, SkipReason::kMargin);
  EXPECT_EQ(d.best_index, 1);
  EXPECT_EQ(d.incumbent_index, 0);
}

// margin == 0.0 means the head IS the model's pick. Reading that as "lost by
// a hair" is the misreading this field exists to prevent.
TEST(Decide, BestIndexIsTheIncumbentWhenTheModelAgreesWithTheHead) {
  std::vector<std::string> cands{"阅读", "月度"};
  std::vector<float> lp{-2.0f, -9.0f};
  std::vector<int> nt{2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.skip, SkipReason::kMargin);
  EXPECT_EQ(d.best_index, d.incumbent_index);
  EXPECT_FLOAT_EQ(d.margin, 0.0f);
}

TEST(Decide, BestIndexStaysUnsetWithNoHanCandidate) {
  std::vector<std::string> cands{"nali", "ww"};
  std::vector<float> lp{-1.0f, -1.0f};
  std::vector<int> nt{2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.skip, SkipReason::kNoHan);
  EXPECT_EQ(d.best_index, -1);
}

TEST(Decide, BestIndexEqualsPromoteIndexOnAPromotion) {
  std::vector<std::string> cands{"guyide", "顾忌", "故意"};
  std::vector<float> lp{-1.0f, -20.0f, -1.0f};
  std::vector<int> nt{3, 2, 2};
  auto d = Decide(cands, lp, nt, Opts());
  EXPECT_EQ(d.promote_index, 2);
  EXPECT_EQ(d.best_index, 2);
}

// kNone means "the model ran". The db-context gate ends the segment before
// anything is scored, so kNone must never survive it: StatsAccumulator counts
// such a trace in llm_acted and pushes its us=0 into the latency samples,
// and telemetry_commit turns it into an `llm` object whose skip is "" --
// three untrue numbers from one value.
TEST(SkipForEmptyDbContext, ReplacesKNoneWithNoContext) {
  EXPECT_EQ(SkipForEmptyDbContext(SkipReason::kNone), SkipReason::kNoContext);
}

// A reason the fallback chain already established is the more specific truth
// and must survive: "the battery gate stopped this" is not "no context".
TEST(SkipForEmptyDbContext, KeepsAnyReasonTheChainAlreadyEstablished) {
  EXPECT_EQ(SkipForEmptyDbContext(SkipReason::kBattery), SkipReason::kBattery);
  EXPECT_EQ(SkipForEmptyDbContext(SkipReason::kCold), SkipReason::kCold);
  EXPECT_EQ(SkipForEmptyDbContext(SkipReason::kNoModel), SkipReason::kNoModel);
  EXPECT_EQ(SkipForEmptyDbContext(SkipReason::kDisabled), SkipReason::kDisabled);
}
