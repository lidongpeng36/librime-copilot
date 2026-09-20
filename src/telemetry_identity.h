#pragma once

#include <filesystem>
#include <string>

#include "telemetry_event.h"

namespace rime::telemetry {

// Content identifiers, not authentication. File hashing is only called on the
// model worker, never per keystroke. An unreadable file has no identity.
std::string Fingerprint(const std::string& bytes);
std::string FileFingerprint(const std::filesystem::path& path);
std::string BuildId();

// One processor lifetime, independent of wall-clock timestamp resolution.
// A failed flush leaves its window open and retains the same window identity.
class Session {
 public:
  Session();
  RecordMetadata Event(const std::string& config_id, const std::string& model_id);
  RecordMetadata Window(const std::string& config_id, const std::string& model_id) const;
  void CloseWindow() { ++window_; }

 private:
  std::string id_;
  uint64_t event_ = 0;
  uint64_t window_ = 1;
};

}  // namespace rime::telemetry
