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
  EXPECT_FALSE(ShouldRecord(0, false));  // ordinary typing
  EXPECT_TRUE(ShouldRecord(0, true));    // promotion accepted
  EXPECT_TRUE(ShouldRecord(3, true));    // promotion rejected
  EXPECT_TRUE(ShouldRecord(3, false));   // translator mis-ranking
}
