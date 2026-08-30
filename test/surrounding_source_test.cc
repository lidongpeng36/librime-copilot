#include <gtest/gtest.h>

#include "imk_client.h"
#include "surrounding_source.h"

using namespace rime;

TEST(SurroundingSourceName, CoversEveryEnumerator) {
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kNone), "none");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kIMK), "imk");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kBridge), "bridge");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kTmux), "tmux");
  // The fifth was added with the caret-context chain's rung 4 and pinned
  // nowhere: caret_context_test.cc asserts the enumerator, never its name.
  // The name is what reaches telemetry, so enum and string drifting apart is
  // a silent mislabel in the one place the rung is observable.
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kReconstructed), "reconstructed");
}

// A snapshot nobody has attributed must not claim a source.
TEST(SurroundingText, DefaultsToNoSource) {
  SurroundingText s;
  EXPECT_EQ(s.source, SurroundingSource::kNone);
}

// How deep the sources must fetch. `copilot/rerank/llm/context_chars` was a
// consumer declaration nothing honoured: the filter truncated with it and the
// warm cache keyed on it, but it was never a term in the max that sizes the
// fetch, so the sources stopped at 8 whatever the schema said. Telemetry
// measured the consequence directly -- 71% of fetches ended in kByConfig,
// "the source HAD more and prefix_chars cut it", on a machine configured 32.

TEST(SurroundingPrefixChars, LlmContextRaisesTheFetch) {
  SurroundingConsumers c;
  c.surrounding_context_chars = 8;
  c.rerank_max_context_chars = 8;
  c.llm_enable = true;
  c.llm_context_chars = 32;
  EXPECT_EQ(SurroundingPrefixChars(c), 32);
}

// The regression this exists to prevent: with the LLM path off, nothing reads
// 32 characters, so nothing should pay to fetch them.
TEST(SurroundingPrefixChars, LlmContextIsIgnoredWhenTheLlmPathIsOff) {
  SurroundingConsumers c;
  c.llm_enable = false;
  c.llm_context_chars = 64;
  EXPECT_EQ(SurroundingPrefixChars(c), 8);
}

// CopilotRerankFilter::Apply returns early on !enable, so neither re-ranking
// consumer can read anything -- an llm_enable left true underneath it must not
// keep the fetch deep.
TEST(SurroundingPrefixChars, RerankDisabledSuppressesBothRerankTerms) {
  SurroundingConsumers c;
  c.rerank_enable = false;
  c.rerank_max_context_chars = 16;
  c.llm_enable = true;
  c.llm_context_chars = 64;
  c.surrounding_context_chars = 4;
  EXPECT_EQ(SurroundingPrefixChars(c), 4);
}

// AutoSpacer needs the boundary character whatever else is off.
TEST(SurroundingPrefixChars, TheFloorIsOneNotZero) {
  SurroundingConsumers c;
  c.use_surrounding_context = false;
  c.rerank_enable = false;
  EXPECT_EQ(SurroundingPrefixChars(c), 1);
}

// Clamped per term, not after the max: an out-of-range key must not be able to
// raise the fetch on behalf of a consumer that would itself have clamped down.
TEST(SurroundingPrefixChars, EachTermIsClampedBeforeTheMax) {
  SurroundingConsumers c;
  c.llm_enable = true;
  c.llm_context_chars = 4096;
  EXPECT_EQ(SurroundingPrefixChars(c), kMaxSurroundingPrefixChars);

  SurroundingConsumers zeroed;
  zeroed.surrounding_context_chars = 0;
  zeroed.rerank_max_context_chars = -5;
  zeroed.llm_enable = true;
  zeroed.llm_context_chars = 0;
  EXPECT_EQ(SurroundingPrefixChars(zeroed), 1);
}

// The shipped schema: max_context_chars 8, llm context_chars 32, both paths on.
TEST(SurroundingPrefixChars, TheShippedSchemaFetches32) {
  SurroundingConsumers c;
  c.use_surrounding_context = true;
  c.surrounding_context_chars = 8;
  c.rerank_enable = true;
  c.rerank_max_context_chars = 8;
  c.llm_enable = true;
  c.llm_context_chars = 32;
  EXPECT_EQ(SurroundingPrefixChars(c), 32);
}
