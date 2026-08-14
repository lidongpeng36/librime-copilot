#include "telemetry.h"

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <system_error>

namespace rime {
namespace telemetry {

void ClampOptions(Options& options) {
  options.top_n = std::clamp(options.top_n, 1, 20);
  options.max_file_bytes =
      std::clamp<int64_t>(options.max_file_bytes, 64 * 1024, int64_t{256} * 1024 * 1024);
  options.keep_generations = std::clamp(options.keep_generations, 1, 10);
}

bool ShouldRotate(int64_t current_size, int64_t pending_bytes, int64_t max_bytes) {
  if (current_size <= 0) {
    return false;
  }
  return current_size + pending_bytes > max_bytes;
}

std::string FormatTimestamp(std::time_t t) {
  std::tm tm{};
  localtime_r(&t, &tm);
  char buf[32];
  // %z gives the basic-format offset (+0800). ISO 8601 allows it, and Python's
  // datetime.fromisoformat accepts it from 3.11 on.
  const size_t n = std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S%z", &tm);
  return std::string(buf, n);
}

Writer::Writer(std::filesystem::path dir, std::string machine, Options options)
    : dir_(std::move(dir)), options_(options) {
  ClampOptions(options_);
  path_ = dir_ / (machine + ".jsonl");
}

Writer::~Writer() {
  if (fd_ >= 0) {
    ::close(fd_);
  }
}

void Writer::Rotate() {
  if (fd_ >= 0) {
    ::close(fd_);
    fd_ = -1;
  }
  std::error_code ec;
  // keep_generations counts the live file, so the highest archive index is
  // keep_generations - 1. Drop it, then shift the rest down.
  const int last = options_.keep_generations - 1;
  if (last >= 1) {
    std::filesystem::remove(path_.string() + "." + std::to_string(last), ec);
    for (int i = last - 1; i >= 1; --i) {
      std::filesystem::rename(path_.string() + "." + std::to_string(i),
                              path_.string() + "." + std::to_string(i + 1), ec);
    }
    std::filesystem::rename(path_, path_.string() + ".1", ec);
  } else {
    std::filesystem::remove(path_, ec);
  }
  size_ = 0;
}

bool Writer::Open() {
  std::error_code ec;
  std::filesystem::create_directories(dir_, ec);
  fd_ = ::open(path_.c_str(), O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
  if (fd_ < 0) {
    // Telemetry must never break input. Give up quietly and try again on the
    // next write.
    return false;
  }
  // Always take the size from the file, including after a rotation. Rotate()
  // ignores the std::error_code from its renames, so a rename that fails
  // leaves the live file in place with its old contents; assuming 0 there
  // would let the file grow by max_file_bytes on every failed cycle, without
  // bound. Reading the real size makes that case self-correcting: the very
  // next write rotates again.
  const off_t end = ::lseek(fd_, 0, SEEK_END);
  size_ = end > 0 ? static_cast<int64_t>(end) : 0;
  return true;
}

void Writer::Write(const std::string& line) {
  if (!options_.enable) {
    return;
  }
  const std::string payload = line + "\n";
  const int64_t pending = static_cast<int64_t>(payload.size());

  if (fd_ < 0 && !Open()) {
    return;
  }

  if (ShouldRotate(size_, pending, options_.max_file_bytes)) {
    Rotate();
    if (!Open()) {
      return;
    }
  }

  // Write all bytes, handling partial writes and EINTR.
  size_t bytes_written = 0;
  while (bytes_written < payload.size()) {
    const ssize_t n = ::write(fd_, payload.data() + bytes_written, payload.size() - bytes_written);
    if (n < 0) {
      if (errno == EINTR) {
        continue;  // Retry on signal interrupt.
      }
      // Other errors: give up silently.
      return;
    }
    if (n == 0) {
      // Shouldn't happen with write, but be defensive.
      return;
    }
    bytes_written += n;
    size_ += n;
  }
}

}  // namespace telemetry
}  // namespace rime
