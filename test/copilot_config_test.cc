// The schema keys more than one component reads.
//
// This is the first test in the tree that drives real config. It can be,
// because rime::Config has a public default constructor and an exported
// LoadFromStream -- no engine, no schema, no deployer, so it stays inside the
// "pure logic, no Rime engine" rule the rest of test/ follows.
//
// What it is guarding is not the parsing (librime's job) but AGREEMENT: every
// key below used to be read, and two of them clamped, at two or three
// independent sites, kept in step by hand and by paragraphs in CLAUDE.md. The
// clamp cases in particular are the ones that were wrong in production before
// -- copilot/rerank/llm/context_chars was a hard-coded 32 in copilot.h against
// a filter that read config, agreeing only by coincidence.

#include <gtest/gtest.h>

#include <rime/config.h>

#include <sstream>

#include "copilot_config.h"
#include "rerank_filter.h"
#include "surrounding_source.h"  // kMaxSurroundingPrefixChars

namespace rime {
namespace {

// A Config built from a YAML string and nothing else.
std::unique_ptr<Config> ConfigFrom(const std::string& yaml) {
  auto config = std::make_unique<Config>();
  std::istringstream stream(yaml);
  EXPECT_TRUE(config->LoadFromStream(stream));
  return config;
}

TEST(CopilotSharedConfigTest, NullConfigYieldsDefaults) {
  const CopilotSharedConfig c = ReadCopilotSharedConfig(nullptr);
  EXPECT_EQ(c.db, "copilot.db");
  EXPECT_TRUE(c.rerank_enable);
  EXPECT_EQ(c.rerank_max_context_chars, 8);
  EXPECT_FALSE(c.llm_enable);
  EXPECT_EQ(c.llm_model, "");
  EXPECT_FALSE(c.llm_battery_active);
  EXPECT_EQ(c.llm_context_chars, 32);
}

// The defaults each of the three call sites carried before this function
// existed. An empty config must reproduce them exactly.
TEST(CopilotSharedConfigTest, EmptyConfigYieldsTheSameDefaults) {
  auto config = ConfigFrom("nothing: here\n");
  const CopilotSharedConfig c = ReadCopilotSharedConfig(config.get());
  EXPECT_EQ(c.db, "copilot.db");
  EXPECT_TRUE(c.rerank_enable);
  EXPECT_EQ(c.rerank_max_context_chars, 8);
  EXPECT_FALSE(c.llm_enable);
  EXPECT_EQ(c.llm_model, "");
  EXPECT_FALSE(c.llm_battery_active);
  EXPECT_EQ(c.llm_context_chars, 32);
}

TEST(CopilotSharedConfigTest, ReadsEveryKey) {
  auto config = ConfigFrom(R"(
copilot:
  db: "private/private.predict.db"
  rerank:
    enable: false
    max_context_chars: 12
    llm:
      enable: true
      model: "private/rime40m-v2-q8.gguf"
      battery_active: true
      context_chars: 48
)");
  const CopilotSharedConfig c = ReadCopilotSharedConfig(config.get());
  EXPECT_EQ(c.db, "private/private.predict.db");
  EXPECT_FALSE(c.rerank_enable);
  EXPECT_EQ(c.rerank_max_context_chars, 12);
  EXPECT_TRUE(c.llm_enable);
  EXPECT_EQ(c.llm_model, "private/rime40m-v2-q8.gguf");
  EXPECT_TRUE(c.llm_battery_active);
  EXPECT_EQ(c.llm_context_chars, 48);
}

// The flow-map shape Rime's deployer writes into build/*.schema.yaml, which is
// what the plugin actually reads at runtime -- not the block form a user
// writes in a .custom.yaml patch. Both must reach the same struct.
TEST(CopilotSharedConfigTest, ReadsTheDeployerFlowMapForm) {
  auto config = ConfigFrom(
      "copilot:\n"
      "  rerank:\n"
      "    llm: {battery_active: true, context_chars: 64, enable: true, "
      "model: \"private/rime40m-v2-q8.gguf\", top_n: 4}\n");
  const CopilotSharedConfig c = ReadCopilotSharedConfig(config.get());
  EXPECT_TRUE(c.llm_enable);
  EXPECT_TRUE(c.llm_battery_active);
  EXPECT_EQ(c.llm_context_chars, 64);
  EXPECT_EQ(c.llm_model, "private/rime40m-v2-q8.gguf");
}

// Both of these size a per-keystroke surrounding query since 2026-08-28, so a
// value the sources can never return would make every fetch look truncated by
// config forever. They were clamped at each reading site independently; the
// clamp is part of the read now, so there is nowhere left for two sites to
// disagree.
TEST(CopilotSharedConfigTest, ClampsBothContextLengthsToWhatSourcesCanFetch) {
  auto too_big = ConfigFrom(R"(
copilot:
  rerank:
    max_context_chars: 9999
    llm:
      context_chars: 9999
)");
  const CopilotSharedConfig high = ReadCopilotSharedConfig(too_big.get());
  EXPECT_EQ(high.rerank_max_context_chars, kMaxSurroundingPrefixChars);
  EXPECT_EQ(high.llm_context_chars, kMaxSurroundingPrefixChars);

  auto too_small = ConfigFrom(R"(
copilot:
  rerank:
    max_context_chars: 0
    llm:
      context_chars: -5
)");
  const CopilotSharedConfig low = ReadCopilotSharedConfig(too_small.get());
  EXPECT_EQ(low.rerank_max_context_chars, 1);
  EXPECT_EQ(low.llm_context_chars, 1);
}

TEST(TelemetryOptionsConfigTest, NullConfigYieldsClampedDefaults) {
  const telemetry::Options o = ReadTelemetryOptions(nullptr);
  EXPECT_TRUE(o.enable);
  EXPECT_EQ(o.top_n, 5);
  EXPECT_EQ(o.max_file_bytes, 8 * 1024 * 1024);
  EXPECT_EQ(o.keep_generations, 2);
  EXPECT_EQ(o.sample_ok, 0);
  EXPECT_FALSE(o.auto_sync);
}

TEST(TelemetryOptionsConfigTest, ReadsEveryKey) {
  auto config = ConfigFrom(R"(
copilot:
  telemetry:
    enable: false
    top_n: 10
    max_file_bytes: 1048576
    keep_generations: 3
    sample_ok: 20
    auto_sync: true
)");
  const telemetry::Options o = ReadTelemetryOptions(config.get());
  EXPECT_FALSE(o.enable);
  EXPECT_EQ(o.top_n, 10);
  EXPECT_EQ(o.max_file_bytes, 1048576);
  EXPECT_EQ(o.keep_generations, 3);
  EXPECT_EQ(o.sample_ok, 20);
  EXPECT_TRUE(o.auto_sync);
}

// telemetry::ClampOptions ran at both old sites; it must still run here, or a
// hostile value reaches Writer instead of being bounded.
TEST(TelemetryOptionsConfigTest, ClampsThroughClampOptions) {
  auto config = ConfigFrom(R"(
copilot:
  telemetry:
    top_n: 9999
    max_file_bytes: 1
    keep_generations: 0
    sample_ok: -1
)");
  const telemetry::Options o = ReadTelemetryOptions(config.get());
  EXPECT_EQ(o.top_n, 20);
  EXPECT_EQ(o.max_file_bytes, 64 * 1024);
  EXPECT_EQ(o.keep_generations, 1);
  EXPECT_EQ(o.sample_ok, 0);
}

TEST(WordHeadConfig, DefaultsToInertWithTheConventionalTablePath) {
  const RerankOptions r = ReadRerankOptions(nullptr);
  EXPECT_FLOAT_EQ(r.llm.word_head_weight, 0.0f);
  EXPECT_EQ(r.llm.word_head_table, "private/wordhead.txt");
}

TEST(WordHeadConfig, ReadsTheNestedKeys) {
  auto config = ConfigFrom(R"(
copilot:
  rerank:
    llm:
      word_head:
        weight: 1.0
        table: private/other.txt
)");
  const RerankOptions r = ReadRerankOptions(config.get());
  EXPECT_FLOAT_EQ(r.llm.word_head_weight, 1.0f);
  EXPECT_EQ(r.llm.word_head_table, "private/other.txt");
}

// What the plugin actually reads at runtime: the deployer writes flow maps
// into build/*.schema.yaml.
TEST(WordHeadConfig, ReadsTheDeployerFlowMapForm) {
  auto config = ConfigFrom(
      "copilot: {rerank: {llm: {word_head: {weight: 1.5, table: private/wordhead.txt}}}}\n");
  EXPECT_FLOAT_EQ(ReadRerankOptions(config.get()).llm.word_head_weight, 1.5f);
}

TEST(WordHeadConfig, ClampsTheWeightAtBothEnds) {
  auto low = ConfigFrom("copilot: {rerank: {llm: {word_head: {weight: -3}}}}\n");
  auto high = ConfigFrom("copilot: {rerank: {llm: {word_head: {weight: 50}}}}\n");
  EXPECT_FLOAT_EQ(ReadRerankOptions(low.get()).llm.word_head_weight, 0.0f);
  EXPECT_FLOAT_EQ(ReadRerankOptions(high.get()).llm.word_head_weight, 10.0f);
}

TEST(WordHeadConfig, TelemetryRecordsTheWeightButNeverThePath) {
  rime::Config config;
  std::istringstream yaml(
      "copilot: {rerank: {llm: {word_head: {weight: 1.0, table: /Users/x/private/w.txt}}}}\n");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  const auto recorded = rime::TelemetryConfig(&config, 64);
  EXPECT_EQ(recorded["word_head_weight"], 1.0);
  EXPECT_EQ(recorded.dump().find("/Users/x"), std::string::npos);
}

}  // namespace
}  // namespace rime

TEST(TelemetryConfig, CapturesEffectiveDefaultsClampsAndNeverCopiesPrivateFields) {
  rime::Config config;
  std::istringstream yaml(R"(
copilot:
  rerank:
    window: 3
    llm:
      top_n: 20
      margin: 1.0
      model: /private/local/model.gguf
  telemetry:
    sample_ok: 37
secret: never-record-this
)");
  ASSERT_TRUE(config.LoadFromStream(yaml));
  const auto recorded = rime::TelemetryConfig(&config, 64);
  EXPECT_EQ(recorded["top_n"], 3);
  EXPECT_EQ(recorded["sample_ok"], 37);
  EXPECT_EQ(recorded["model"], "model.gguf");
  EXPECT_EQ(recorded["require_han_context"], true);
  EXPECT_EQ(recorded["fetch_chars"], 64);
  EXPECT_EQ(recorded.dump().find("private/local"), std::string::npos);
  EXPECT_EQ(recorded.dump().find("never-record-this"), std::string::npos);
}
