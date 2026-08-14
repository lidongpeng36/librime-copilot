#include <gtest/gtest.h>

#include "imk_client.h"

using namespace rime;

TEST(SurroundingSourceName, CoversEveryEnumerator) {
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kNone), "none");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kIMK), "imk");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kBridge), "bridge");
  EXPECT_STREQ(SurroundingSourceName(SurroundingSource::kTmux), "tmux");
}

// A snapshot nobody has attributed must not claim a source.
TEST(SurroundingText, DefaultsToNoSource) {
  SurroundingText s;
  EXPECT_EQ(s.source, SurroundingSource::kNone);
}
