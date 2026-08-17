#include <gtest/gtest.h>

#include "warm_cache.h"

using rime::WarmCache;

TEST(WarmCache, StartsCold) {
  WarmCache c;
  EXPECT_EQ(c.Lookup("高屋建"), WarmCache::State::kCold);
}

TEST(WarmCache, BeginWarmThenPublishMakesItHot) {
  WarmCache c;
  c.BeginWarm("高屋建");
  EXPECT_EQ(c.Lookup("高屋建"), WarmCache::State::kWarming);
  c.Publish("高屋建");
  EXPECT_EQ(c.Lookup("高屋建"), WarmCache::State::kHot);
}

// Invalidation needs no detection. A mouse click, a window switch and an arrow
// key all change the context, which changes the key, which misses.
TEST(WarmCache, ADifferentKeyIsCold) {
  WarmCache c;
  c.BeginWarm("高屋建");
  c.Publish("高屋建");
  EXPECT_EQ(c.Lookup("这个顺序是"), WarmCache::State::kCold);
}

TEST(WarmCache, BeginWarmForANewKeyDiscardsTheOld) {
  WarmCache c;
  c.BeginWarm("A");
  c.Publish("A");
  c.BeginWarm("B");
  EXPECT_EQ(c.Lookup("A"), WarmCache::State::kCold);
  EXPECT_EQ(c.Lookup("B"), WarmCache::State::kWarming);
}

// A warm that finished for a key that is no longer the pending one must not
// publish: the context moved on while the forward pass was running.
TEST(WarmCache, PublishForAStaleKeyIsIgnored) {
  WarmCache c;
  c.BeginWarm("A");
  c.BeginWarm("B");
  c.Publish("A");
  EXPECT_EQ(c.Lookup("A"), WarmCache::State::kCold);
  EXPECT_EQ(c.Lookup("B"), WarmCache::State::kWarming);
}

TEST(WarmCache, BeginWarmOnTheKeyAlreadyHotIsANoOp) {
  WarmCache c;
  c.BeginWarm("A");
  c.Publish("A");
  c.BeginWarm("A");
  EXPECT_EQ(c.Lookup("A"), WarmCache::State::kHot);
}

TEST(WarmCache, EmptyKeyIsAlwaysCold) {
  WarmCache c;
  c.BeginWarm("");
  c.Publish("");
  EXPECT_EQ(c.Lookup(""), WarmCache::State::kCold);
}
