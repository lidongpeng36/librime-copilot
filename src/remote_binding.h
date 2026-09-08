#pragma once

// Which remote tmux pane sits behind a local pane running ssh.
//
// Rime-free on purpose, exactly like context_memory.h: the whole point is
// that test/remote_binding_test.cc can drive it without standing up an
// engine. Nothing here touches AppKit, tmux, or a socket.
//
// Why this exists: a local pane switch fires no hook on the remote. Two
// local panes each ssh'd into a different remote tmux pane are, from the
// local hook's point of view, both just `%N|ssh`. This table is what tells
// them apart, populated whenever a remote pane switch arrives while the
// user is sitting in the local pane that carries it.

#include <list>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "context_memory.h"

namespace rime {
namespace context_memory {

// Whether a local pane running `command` is one this feature will look
// behind. Exact match, never a prefix: `sshfs` is not a remote shell.
inline bool IsRemoteCommand(const std::vector<std::string>& remote_commands,
                            const std::string& command) {
  if (command.empty()) {
    return false;
  }
  for (const auto& candidate : remote_commands) {
    if (candidate == command) {
      return true;
    }
  }
  return false;
}

// Bounded LRU, for the same reason context_memory::Table is bounded: pane
// ids grow monotonically as panes come and go, so an unbounded map on a
// machine that is never rebooted grows without limit. Eviction takes the
// least recently used entry -- an arbitrary drop would most often take the
// pane the user is sitting in.
class RemoteBinding {
 public:
  void SetMaxEntries(size_t max_entries) {
    std::lock_guard<std::mutex> lock(mutex_);
    max_entries_ = max_entries == 0 ? 1 : max_entries;
    EvictLocked();
  }

  void Bind(const std::string& local_pane_id, const std::string& local_command,
            const Identity& remote) {
    if (local_pane_id.empty()) {
      return;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(local_pane_id);
    if (it != index_.end()) {
      it->second->command = local_command;
      it->second->remote = remote;
      order_.splice(order_.begin(), order_, it->second);
    } else {
      order_.push_front(Entry{local_pane_id, local_command, remote});
      index_[local_pane_id] = order_.begin();
    }
    ReleaseOtherClaimsLocked(local_pane_id, remote);
    EvictLocked();
  }

  // NOT const: a hit splices the LRU, exactly as context_memory::Table::Get
  // does. If you want a peek with no side effect, add a Contains() rather
  // than making this const -- copilot.cc:404 records why that distinction
  // matters for the diagnostic path.
  std::optional<Identity> Lookup(const std::string& local_pane_id,
                                 const std::string& local_command) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = index_.find(local_pane_id);
    if (it == index_.end()) {
      return std::nullopt;
    }
    // The command guard. An entry outlives the ssh that created it; when the
    // pane goes back to a shell the command no longer matches and the entry
    // stops answering without anyone having to clear it.
    if (it->second->command != local_command) {
      return std::nullopt;
    }
    order_.splice(order_.begin(), order_, it->second);
    return it->second->remote;
  }

  size_t size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return index_.size();
  }

  void Clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    order_.clear();
    index_.clear();
  }

 private:
  struct Entry {
    std::string local_pane_id;
    std::string command;
    Identity remote;
  };

  // Callers hold mutex_.
  void EvictLocked() {
    while (index_.size() > max_entries_) {
      index_.erase(order_.back().local_pane_id);
      order_.pop_back();
    }
  }

  // The caret cannot be in two places, so two local panes claiming one remote
  // pane is a contradiction and the newer claim is the better evidence. This
  // is what stops a mis-bind from being permanent: the table is process-wide
  // and survives a redeploy, so before this the only cure was restarting
  // Squirrel.
  //
  // Keyed on (socket, host, pane_id), NEVER on (host, pane_id) alone. Two
  // local panes ssh'd to one host and attached to two DIFFERENT remote panes
  // is the case this whole feature exists for -- see
  // DistinctLocalPanesHoldDistinctRemotes, and TwoPanesOfOneHostAreNotAClaimOnEachOther
  // beside it. socket is in the key for the same reason: a host can run more
  // than one tmux server, each numbering its panes from scratch, so
  // `pane_id` alone is not unique across servers on one host --
  // see TwoTmuxServersOnOneHostAreNotAClaimOnEachOtherDespiteSamePaneId. This
  // must agree with what Identity::operator== and MakeKey (context_memory.h)
  // already treat as a pane's identity, or the eviction here disagrees with
  // what "the same pane" means everywhere else in this file. An empty host is
  // not a claim on anything: Bind is only ever called with a remote identity,
  // but a future caller passing one without a host must not evict every pane
  // that happens to share a socket and pane id.
  //
  // A consequence worth stating plainly: if a stray remote push ever binds
  // the WRONG local pane to a remote pane another local pane legitimately
  // holds, this does not just record the wrong binding -- it also erases the
  // correct pane's entry in the same call, so that pane falls back to its own
  // local key until it next receives a push of its own. That is still
  // strictly better than what this branch replaces (before, the two panes
  // would have SHARED one ascii_mode indefinitely; now at most one pane is
  // briefly wrong), so it is accepted rather than something to fix here.
  //
  // Callers hold mutex_.
  void ReleaseOtherClaimsLocked(const std::string& keep_local_pane, const Identity& remote) {
    if (remote.host.empty()) {
      return;
    }
    for (auto it = order_.begin(); it != order_.end();) {
      if (it->local_pane_id != keep_local_pane && it->remote.socket == remote.socket &&
          it->remote.host == remote.host && it->remote.pane_id == remote.pane_id) {
        index_.erase(it->local_pane_id);
        it = order_.erase(it);
      } else {
        ++it;
      }
    }
  }

  mutable std::mutex mutex_;
  size_t max_entries_ = 256;
  std::list<Entry> order_;
  std::unordered_map<std::string, std::list<Entry>::iterator> index_;
};

}  // namespace context_memory
}  // namespace rime
