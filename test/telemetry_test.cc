#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "telemetry.h"

using namespace rime::telemetry;
namespace fs = std::filesystem;

namespace {

fs::path FreshDir(const std::string& name) {
  fs::path dir = fs::path(::testing::TempDir()) / ("copilot_telemetry_" + name);
  fs::remove_all(dir);
  fs::create_directories(dir);
  return dir;
}

std::vector<std::string> ReadLines(const fs::path& p) {
  std::vector<std::string> lines;
  std::ifstream in(p);
  std::string line;
  while (std::getline(in, line)) {
    lines.push_back(line);
  }
  return lines;
}

}  // namespace

TEST(TelemetryRotation, RotatesOnlyWhenTheCapWouldBeExceeded) {
  EXPECT_FALSE(ShouldRotate(0, 100, 1000));
  EXPECT_FALSE(ShouldRotate(500, 100, 1000));
  EXPECT_TRUE(ShouldRotate(950, 100, 1000));
}

// An empty file must never rotate, or a line longer than the cap would rotate
// forever and never be written at all.
TEST(TelemetryRotation, NeverRotatesAnEmptyFile) { EXPECT_FALSE(ShouldRotate(0, 999999, 1000)); }

TEST(TelemetryOptions, ClampsBothEnds) {
  Options o;
  o.top_n = 0;
  o.max_file_bytes = 1;
  o.keep_generations = 0;
  ClampOptions(o);
  EXPECT_GE(o.top_n, 1);
  EXPECT_GE(o.max_file_bytes, 64 * 1024);
  EXPECT_GE(o.keep_generations, 1);

  Options big;
  big.top_n = 9999;
  big.max_file_bytes = int64_t{1} << 40;
  big.keep_generations = 9999;
  ClampOptions(big);
  EXPECT_LE(big.top_n, 20);
  EXPECT_LE(big.max_file_bytes, int64_t{256} * 1024 * 1024);
  EXPECT_LE(big.keep_generations, 10);
}

TEST(TelemetryWriter, AppendsOneLinePerWrite) {
  fs::path dir = FreshDir("append");
  Options o;
  Writer w(dir, "MacBookPro-M4Pro", o);
  w.Write("{\"a\":1}");
  w.Write("{\"a\":2}");
  auto lines = ReadLines(dir / "MacBookPro-M4Pro.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "{\"a\":1}");
  EXPECT_EQ(lines[1], "{\"a\":2}");
}

TEST(TelemetryWriter, CreatesTheDirectoryItNeeds) {
  fs::path dir = FreshDir("mkdir") / "nested" / "deeper";
  Options o;
  Writer w(dir, "m", o);
  w.Write("{}");
  EXPECT_TRUE(fs::exists(dir / "m.jsonl"));
}

TEST(TelemetryWriter, RotatesAndKeepsTheConfiguredGenerations) {
  fs::path dir = FreshDir("rotate");
  Options o;
  o.max_file_bytes = 64 * 1024;  // the clamp floor
  o.keep_generations = 2;        // live + .1
  ClampOptions(o);
  Writer w(dir, "m", o);
  const std::string line(1024, 'x');
  for (int i = 0; i < 200; ++i) {
    w.Write(line);
  }
  EXPECT_TRUE(fs::exists(dir / "m.jsonl"));
  EXPECT_TRUE(fs::exists(dir / "m.jsonl.1"));
  EXPECT_FALSE(fs::exists(dir / "m.jsonl.2"));
  EXPECT_LE(fs::file_size(dir / "m.jsonl"), static_cast<uintmax_t>(o.max_file_bytes));
}

TEST(TelemetryWriter, ReopeningKeepsAppending) {
  fs::path dir = FreshDir("reopen");
  Options o;
  {
    Writer w(dir, "m", o);
    w.Write("first");
  }
  {
    Writer w(dir, "m", o);
    w.Write("second");
  }
  auto lines = ReadLines(dir / "m.jsonl");
  ASSERT_EQ(lines.size(), 2u);
  EXPECT_EQ(lines[0], "first");
  EXPECT_EQ(lines[1], "second");
}

TEST(TelemetryWriter, DisabledWritesNothing) {
  fs::path dir = FreshDir("disabled");
  Options o;
  o.enable = false;
  Writer w(dir, "m", o);
  w.Write("{}");
  EXPECT_FALSE(fs::exists(dir / "m.jsonl"));
}

TEST(TelemetryWriter, RotatesMultipleGenerationsWithContent) {
  fs::path dir = FreshDir("multi_gen");
  Options o;
  o.max_file_bytes = 64 * 1024;  // the clamp floor = 65536
  o.keep_generations = 3;        // live + .1 + .2
  ClampOptions(o);
  Writer w(dir, "m", o);

  // Write deterministic payloads so rotation happens predictably.
  // Each payload is ~40000 bytes. Since 40000 + 40000 > 65536, each write
  // after the first triggers rotation. This places:
  // - After write 1 (A): live=A
  // - After write 2 (B): .1=A, live=B
  // - After write 3 (C): .2=A, .1=B, live=C
  // - After write 4 (D): .2=B (A deleted), .1=C, live=D
  const int payload_size = 40000;
  const std::string marker_a =
      "A" + std::string(payload_size - 1 - 1, 'x');  // Account for marker and newline
  const std::string marker_b = "B" + std::string(payload_size - 1 - 1, 'x');
  const std::string marker_c = "C" + std::string(payload_size - 1 - 1, 'x');
  const std::string marker_d = "D" + std::string(payload_size - 1 - 1, 'x');

  w.Write(marker_a);  // Triggers no rotation (empty file)
  w.Write(marker_b);  // Triggers rotation: .1=A, live=B
  w.Write(marker_c);  // Triggers rotation: .2=A, .1=B, live=C
  w.Write(marker_d);  // Triggers rotation: .2=B, .1=C, live=D (A deleted)

  // Verify file existence
  EXPECT_TRUE(fs::exists(dir / "m.jsonl"));
  EXPECT_TRUE(fs::exists(dir / "m.jsonl.1"));
  EXPECT_TRUE(fs::exists(dir / "m.jsonl.2"));
  EXPECT_FALSE(fs::exists(dir / "m.jsonl.3"));

  // Verify exact content placement
  auto live_lines = ReadLines(dir / "m.jsonl");
  ASSERT_EQ(live_lines.size(), 1u);
  EXPECT_EQ(live_lines[0][0], 'D');  // First char is 'D' from marker_d

  auto arch1_lines = ReadLines(dir / "m.jsonl.1");
  ASSERT_EQ(arch1_lines.size(), 1u);
  EXPECT_EQ(arch1_lines[0][0], 'C');  // First char is 'C' from marker_c

  auto arch2_lines = ReadLines(dir / "m.jsonl.2");
  ASSERT_EQ(arch2_lines.size(), 1u);
  EXPECT_EQ(arch2_lines[0][0], 'B');  // First char is 'B' from marker_b
}

TEST(TelemetryWriter, KeepsGenerationOneWithDropRecreate) {
  fs::path dir = FreshDir("single_gen");
  Options o;
  o.max_file_bytes = 64 * 1024;  // the clamp floor
  o.keep_generations = 1;        // live only, no archives
  ClampOptions(o);
  Writer w(dir, "m", o);
  // Write 200 lines with padding to trigger rotations; with keep_generations=1,
  // archives should be dropped and recreated (not kept).
  const std::string padding(1000, 'x');
  for (int i = 0; i < 200; ++i) {
    w.Write("gen_" + std::to_string(i) + "_marker " + padding);
  }
  // Only the live file should exist; archives should be dropped.
  EXPECT_TRUE(fs::exists(dir / "m.jsonl"));
  EXPECT_FALSE(fs::exists(dir / "m.jsonl.1"));
  EXPECT_FALSE(fs::exists(dir / "m.jsonl.2"));
  // Verify the live file has content.
  auto lines = ReadLines(dir / "m.jsonl");
  ASSERT_GT(lines.size(), 0u);
  // The last line should end with our marker (content up to the last space and padding).
  EXPECT_TRUE(lines.back().find("gen_") != std::string::npos);
  EXPECT_LE(fs::file_size(dir / "m.jsonl"), static_cast<uintmax_t>(o.max_file_bytes));
}

// Rotate() ignores the std::error_code from its renames. When one fails the
// live file keeps its contents, so the writer must re-read the size from the
// file after reopening rather than assuming 0 — otherwise it believes the file
// is empty and quietly lets it grow by another max_file_bytes before it tries
// to rotate again.
//
// A non-empty directory sitting where the `.1` archive goes blocks both
// filesystem::remove and filesystem::rename, which is exactly the silent
// failure being guarded against.
TEST(TelemetryWriter, RecoversTheLiveSizeWhenRotationFails) {
  fs::path dir = FreshDir("rotate_fails");
  Options o;
  o.max_file_bytes = 64 * 1024;  // the clamp floor = 65536
  o.keep_generations = 2;
  ClampOptions(o);
  Writer w(dir, "m", o);

  const fs::path blocker = dir / "m.jsonl.1";
  fs::create_directories(blocker);
  {
    std::ofstream(blocker / "occupied") << "x";
  }

  const std::string payload(30000, 'x');  // 30001 bytes on disk with the newline
  w.Write("A" + payload.substr(1));       // live = 30001
  w.Write("B" + payload.substr(1));       // live = 60002, still under the cap
  w.Write("C" + payload.substr(1));       // rotation attempted and blocked; live = 90003
  ASSERT_TRUE(fs::is_directory(blocker));
  ASSERT_EQ(ReadLines(dir / "m.jsonl").size(), 3u);

  fs::remove_all(blocker);  // the next rotation can succeed
  w.Write("D" + payload.substr(1));

  // The tracked size was still 90003, over the cap, so this write rotates. With
  // the size wrongly restarted at 0 it would have been 30001 and no rotation
  // would have happened, leaving a live file of four lines and no archive.
  ASSERT_TRUE(fs::is_regular_file(dir / "m.jsonl.1"));
  auto live = ReadLines(dir / "m.jsonl");
  ASSERT_EQ(live.size(), 1u);
  EXPECT_EQ(live[0][0], 'D');
  EXPECT_EQ(ReadLines(dir / "m.jsonl.1").size(), 3u);
}

TEST(TelemetryTimestamp, IsIso8601WithOffset) {
  // 2026-08-14T02:23:45Z. The local rendering depends on the test machine's
  // zone, so assert the shape rather than the digits.
  const std::string ts = FormatTimestamp(static_cast<std::time_t>(1786501425));
  ASSERT_EQ(ts.size(), 24u);  // YYYY-MM-DDTHH:MM:SS+ZZZZ
  EXPECT_EQ(ts[4], '-');
  EXPECT_EQ(ts[7], '-');
  EXPECT_EQ(ts[10], 'T');
  EXPECT_EQ(ts[13], ':');
  EXPECT_EQ(ts[16], ':');
  EXPECT_TRUE(ts[19] == '+' || ts[19] == '-');
}
