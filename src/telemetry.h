#pragma once

// Appending the telemetry line to disk, and nothing else.
//
// This is the only stateful part of the telemetry path, and the only part that
// touches the filesystem. Everything it decides that can be decided purely
// (rotation, clamping) is exposed as a free function so it can be tested
// without a file (test/telemetry_test.cc).
//
// One file per machine, named after Deployer::user_id. Two machines therefore
// never write the same file, which is what makes merging a concatenation with
// no deduplication, no conflict resolution and no ordering requirement.
//
// The file is a chronological transcript of the user's Chinese input. It is
// created 0600 and never leaves the machine from here: syncing is an
// out-of-process step (tools/sync_telemetry.sh). Do not add network code.

#include <cstdint>
#include <ctime>
#include <filesystem>
#include <string>

namespace rime {
namespace telemetry {

struct Options {
  bool enable = true;
  int top_n = 5;
  int64_t max_file_bytes = 8 * 1024 * 1024;
  // Counts the live file, so 2 means the live file plus one archive `.1`.
  int keep_generations = 2;
};

// Both ends, matching the precedent at copilot.cc:71 and
// rerank_filter.cc:183-185.
void ClampOptions(Options& options);

// Rotate before appending `pending_bytes`?
//
// An empty file never rotates: a single line longer than the cap would
// otherwise rotate forever and never be written.
bool ShouldRotate(int64_t current_size, int64_t pending_bytes, int64_t max_bytes);

// Local time as ISO 8601 with a numeric offset: 2026-08-14T10:23:45+0800.
std::string FormatTimestamp(std::time_t t);

class Writer {
 public:
  Writer(std::filesystem::path dir, std::string machine, Options options);
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Appends `line` plus a newline. Silently does nothing when disabled or when
  // the file cannot be opened: telemetry must never break input.
  void Write(const std::string& line);

  const std::filesystem::path& path() const { return path_; }
  const Options& options() const { return options_; }

 private:
  // Opens (creating as needed) the live file for appending and re-reads its
  // size from the file itself. False when the file could not be opened.
  bool Open();
  void Rotate();

  std::filesystem::path dir_;
  std::filesystem::path path_;
  Options options_;
  int fd_ = -1;
  int64_t size_ = 0;
};

}  // namespace telemetry
}  // namespace rime
