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
// The file is a chronological transcript of the user's Chinese input, created
// 0600. It reaches another machine only by being copied whole into the user's
// own Rime sync directory -- SyncToDir below, on a timer, or
// tools/sync_telemetry.sh by hand -- from where iCloud carries it. The append
// itself stays local, which is the point: appending inside an iCloud
// directory would re-upload on every keystroke. Nothing here opens a socket,
// and nothing should: do not add network code.

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
  // Keep 1 in N plain successes for the eval set. 0 disables, which is
  // today's behaviour and the default, so an upgraded machine records exactly
  // what it recorded before until the key is set.
  int sample_ok = 0;
  // Copy the file into Rime's sync directory periodically, from inside the
  // plugin, so a machine's telemetry is never days stale when someone comes
  // to analyse it. Default false: this is the shipped behaviour, and it is
  // the user's own sync directory that fills up. See SyncToDir.
  bool auto_sync = false;
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

// Has `interval_sec` passed since `last_sync`?
//
// A `last_sync` of 0 returns false: it OPENS the first window rather than
// firing immediately, the same rule the stats flush uses (copilot.cc). A
// session too short to reach the interval is covered by the destructor, not
// by a sync on its way past.
//
// A clock that jumped backwards (NTP, a laptop resumed in another timezone)
// returns true rather than wedging the sync off until real time catches up.
bool ShouldSync(std::time_t last_sync, std::time_t now, std::time_t interval_sec);

// Copies THIS machine's telemetry -- `<machine>.jsonl` and its archives
// `.1`..`.keep_generations-1` -- from `src_dir` into `dest_dir`. Returns how
// many files were copied.
//
// Three things about it are load-bearing:
//
//  - Each file is written under a temporary name in `dest_dir` and renamed
//    into place. Another machine may be reading the destination through
//    iCloud while this runs, and must never see half a file.
//    (tools/sync_telemetry.sh uses a bare `cp` because it is run by hand.)
//  - The names are enumerated, never globbed. The destination holds EVERY
//    machine's telemetry; a glob would copy a file that arrived over iCloud
//    straight back out again, under this machine's name.
//  - An empty `dest_dir` is refused. `sync_dir` has to be filled in by hand
//    -- Squirrel never writes one -- so empty is the likely state, and
//    writing into the current directory instead would be worse than nothing.
//
// An absent source file is not an error: nothing has been recorded at that
// index yet. Telemetry must never break input, so nothing here throws.
int SyncToDir(const std::filesystem::path& src_dir, const std::filesystem::path& dest_dir,
              const std::string& machine, int keep_generations);

class Writer {
 public:
  Writer(std::filesystem::path dir, std::string machine, Options options);
  ~Writer();

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;

  // Appends `line` plus a newline. Silently does nothing when disabled or when
  // the file cannot be opened: telemetry must never break input.
  void Write(const std::string& line);

  // Copies this machine's files into `dest_dir` -- SyncToDir with the
  // writer's own directory, machine name and generation count, so the file
  // copied out can never disagree with the file being written.
  int SyncTo(const std::filesystem::path& dest_dir) const;

  const std::filesystem::path& path() const { return path_; }
  const Options& options() const { return options_; }

 private:
  // Opens (creating as needed) the live file for appending and re-reads its
  // size from the file itself. False when the file could not be opened.
  bool Open();
  void Rotate();

  std::filesystem::path dir_;
  std::filesystem::path path_;
  std::string machine_;
  Options options_;
  int fd_ = -1;
  int64_t size_ = 0;
};

}  // namespace telemetry
}  // namespace rime
