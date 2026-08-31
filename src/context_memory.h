#pragma once

// Per-context memory of `ascii_mode`.
//
// The terminal is one IMK client, so Squirrel gives it one session and one
// `ascii_mode` -- shared by every tmux pane inside it. This table is what
// splits that one variable into N. See
// docs/superpowers/specs/2026-08-28-context-ascii-memory-design.md.
//
// Header-only and free of any Rime dependency so it can be driven directly
// from a test, the way every other suite under test/ works.

#include <cstddef>
#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>

namespace rime {
namespace context_memory {

// Where the caret is, at a granularity below the application. Today only tmux
// panes produce one; see GetContextIdentity().
struct Identity {
  std::string socket;   // tmux socket tag; empty means tmux's default
  std::string pane_id;  // e.g. "%7"
  std::string command;  // pane_current_command; may be empty
  // The machine the pane lives on; empty means this one.
  //
  // LAST on purpose. Every existing `Identity{socket, pane, command}`
  // brace-init keeps compiling -- there are 11 of them in test/ alone --
  // and a field inserted anywhere earlier would silently re-bind all of
  // them to the wrong members rather than fail to compile.
  std::string host;
};

inline bool operator==(const Identity& a, const Identity& b) {
  return a.socket == b.socket && a.pane_id == b.pane_id && a.command == b.command &&
         a.host == b.host;
}
inline bool operator!=(const Identity& a, const Identity& b) { return !(a == b); }

// The single string form. BOTH identity rungs must route through this: a
// machine that adds the tmux hook after having used the polling fallback must
// find the keys it already remembered, not orphan them.
//
// This builds byte-identical output to tmux_detail::MakeClientKey
// (src/tmux_source_util.h) for the same socket and pane, and the duplication
// is deliberate: MakeClientKey's header includes history.h, which drags in
// glog, and this header must stay free of any Rime dependency so the pure
// test can drive it without an engine. Keep them in step by hand.
//
// The two could collapse into one the day tmux_source_util.h includes
// utf8_index.h instead -- history.h is there only to re-export
// copilot::UTF8/CharCount, which utf8_index.h provides on its own with no
// glog. Deliberately NOT done here; recorded so the next person does not
// re-derive why there are two of these.
inline std::string MakeKey(const Identity& id, bool use_pane_command) {
  const std::string socket_tag = id.socket.empty() ? "default" : id.socket;
  std::string key = "tmux:";
  // Inserted ONLY when non-empty, so every key already stored on every
  // machine is byte-identical afterwards. See
  // test/context_memory_test.cc's LocalKeyIsByteIdenticalAfterTheHostSegment,
  // which is a literal rather than a round-trip precisely so that a change
  // to this format cannot pass.
  if (!id.host.empty()) {
    key += id.host + ":";
  }
  key += socket_tag + ":" + id.pane_id;
  if (use_pane_command && !id.command.empty()) {
    key += "|" + id.command;
  }
  return key;
}

// Bounded LRU. Bounded because pane ids grow monotonically as panes are
// created and destroyed, so on a machine that is never rebooted an unbounded
// table grows without limit. LRU rather than a bare hash map because eviction
// must drop the least recently used key -- an arbitrary drop would most often
// take the pane the user is sitting in.
class Table {
 public:
  explicit Table(size_t max_entries = 256) : max_entries_(max_entries ? max_entries : 1) {}

  std::optional<bool> Get(const std::string& key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it == index_.end()) return std::nullopt;
    order_.splice(order_.begin(), order_, it->second);
    return it->second->second;
  }

  void Put(const std::string& key, bool ascii_mode) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(key);
    if (it != index_.end()) {
      it->second->second = ascii_mode;
      order_.splice(order_.begin(), order_, it->second);
      return;
    }
    order_.emplace_front(key, ascii_mode);
    index_[key] = order_.begin();
    EvictLocked();
  }

  // Membership without the recency update Get performs. For diagnostics that
  // want to know whether a key was remembered without themselves becoming a
  // reason it stays remembered -- a read with a side effect is not a probe.
  bool Contains(const std::string& key) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.find(key) != index_.end();
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
  }

  // Records that this pane has been served, and says whether that was news.
  //
  // Deliberately a second, separate structure rather than a prefix scan of
  // `index_`: a pane id is a prefix of longer pane ids (%3 of %30), so
  // "does any key belong to this pane" cannot be answered by matching a
  // prefix without re-deriving MakeKey's delimiter rule, and getting that
  // wrong is silent -- %3 would report itself already seen the moment %30
  // existed. Bounded by the same max_entries for the same reason the memory
  // is: pane ids grow monotonically as panes come and go.
  bool MarkPaneSeen(const std::string& pane_key) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = pane_index_.find(pane_key);
    if (it != pane_index_.end()) {
      pane_order_.splice(pane_order_.begin(), pane_order_, it->second);
      return false;
    }
    pane_order_.push_front(pane_key);
    pane_index_[pane_key] = pane_order_.begin();
    while (pane_index_.size() > max_entries_) {
      pane_index_.erase(pane_order_.back());
      pane_order_.pop_back();
    }
    return true;
  }

  void set_max_entries(size_t n) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = n ? n : 1;
    EvictLocked();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    order_.clear();
    index_.clear();
    pane_order_.clear();
    pane_index_.clear();
  }

 private:
  void EvictLocked() {
    while (index_.size() > max_entries_) {
      index_.erase(order_.back().first);
      order_.pop_back();
    }
  }

  using Entry = std::pair<std::string, bool>;
  mutable std::mutex mutex_;
  std::list<Entry> order_;  // front == most recently used
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
  // Which panes this process has served, for MarkPaneSeen. Separate from the
  // memory above because "have I served this pane" and "what mode did this
  // context end in" are different questions with different keys.
  std::list<std::string> pane_order_;
  std::unordered_map<std::string, std::list<std::string>::iterator> pane_index_;
  size_t max_entries_;
};

// Process-wide, deliberately NOT a member of Copilot: Copilot is per-session,
// and a member here would die with the session, so a terminal window closed
// and reopened would remember nothing.
inline Table& Instance() {
  static Table table;
  return table;
}

}  // namespace context_memory
}  // namespace rime
