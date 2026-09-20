#include "telemetry_identity.h"

#include <unistd.h>
#include <boost/uuid/detail/sha1.hpp>

#include <atomic>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <random>
#include <sstream>

namespace rime::telemetry {
namespace {
std::string Digest(boost::uuids::detail::sha1& hash) {
  boost::uuids::detail::sha1::digest_type digest;
  hash.get_digest(digest);
  std::ostringstream out;
  out << "sha1:" << std::hex << std::setfill('0');
  for (auto word : digest) {
    out << std::setw(sizeof(word) * 2) << static_cast<uint32_t>(word);
  }
  return out.str();
}
}  // namespace

std::string Fingerprint(const std::string& bytes) {
  boost::uuids::detail::sha1 hash;
  hash.process_bytes(bytes.data(), bytes.size());
  return Digest(hash);
}

std::string FileFingerprint(const std::filesystem::path& path) {
  std::ifstream file(path, std::ios::binary);
  if (!file) return {};
  boost::uuids::detail::sha1 hash;
  char buffer[65536];
  while (file.read(buffer, sizeof(buffer)) || file.gcount()) {
    hash.process_bytes(buffer, static_cast<size_t>(file.gcount()));
  }
  return file.eof() && !file.bad() ? Digest(hash) : std::string();
}

std::string BuildId() {
#ifdef COPILOT_BUILD_ID
  return COPILOT_BUILD_ID;
#else
  return "unknown";
#endif
}

Session::Session() {
  static std::atomic<uint64_t> sequence{0};
  std::ostringstream seed;
  seed << std::chrono::system_clock::now().time_since_epoch().count() << ':' << getpid() << ':'
       << ++sequence;
  try {
    std::random_device random;
    for (int i = 0; i < 4; ++i) seed << ':' << random();
  } catch (const std::exception&) {
    // Clock, pid and the process sequence still distinguish local sessions.
  }
  id_ = Fingerprint(seed.str()).substr(5);
}

RecordMetadata Session::Window(const std::string& config_id, const std::string& model_id) const {
  RecordMetadata m;
  m.session_id = id_;
  m.window_id = id_ + ":w:" + std::to_string(window_);
  m.record_id = m.window_id;
  m.build_id = BuildId();
  m.config_id = config_id;
  m.model_id = model_id;
  return m;
}

RecordMetadata Session::Event(const std::string& config_id, const std::string& model_id) {
  auto m = Window(config_id, model_id);
  m.record_id = id_ + ":e:" + std::to_string(++event_);
  return m;
}
}  // namespace rime::telemetry
