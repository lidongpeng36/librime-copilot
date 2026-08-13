#pragma once

// Internals of the tmux surrounding source, exposed for the GTest suite.
//
// `tmux_source_util.h` holds everything that is pure; what is left in
// `tmux_source.cc` is the process machinery -- binary probing, posix_spawn,
// the read deadline and the reap loop -- and that is precisely the part that
// can hang or wedge the input thread. The suite must not stand up a live tmux
// server, but it can drive the runner against /bin/echo and /bin/sh, and it
// can drive the per-key-event memo through a stubbed runner. Nothing outside
// tmux_source.cc and test/ should include this header.
//
// The repo's sanctioned precedent for a non-pure test is
// test/ime_bridge_socket_test.cc (see CLAUDE.md).

#include <string>
#include <vector>

namespace rime {
namespace tmux_detail {

// Absolute paths probed when `copilot/tmux_source/binary` is empty. An IMK
// process inherits a minimal PATH, and on a Homebrew Mac `tmux` is not on it
// at all, so absolute probing is the only thing that works.
const std::vector<std::string>& TmuxBinaryCandidates();

// The probe itself, with no caching and no global state: an explicit path is
// honored or refused, an empty one falls back to the candidate list. "" means
// nothing executable was found.
std::string ProbeTmuxBinary(const std::string& configured);

// Spawn `bin` with `args`, collect stdout, and never block past `timeout_ms`.
// Runs on the input thread, so the deadline is load-bearing. Returns false on
// spawn failure, timeout, or a non-zero exit.
bool RunTmuxProcess(const std::string& bin, const std::vector<std::string>& args, int timeout_ms,
                    std::string* out);

// Test seams. Passing nullptr restores the real implementation; tests must
// restore before returning, since these are process-global.
using FrontmostBundleIdFn = std::string (*)();
using RunTmuxFn = bool (*)(const std::string& bin, const std::vector<std::string>& args,
                           int timeout_ms, std::string* out);
void SetTmuxTestHooks(FrontmostBundleIdFn frontmost, RunTmuxFn run);

}  // namespace tmux_detail
}  // namespace rime
