#pragma once

// The head/tail pair that Copilot::ProcessKeyEvent drives, factored out of the
// processor so it can be tested without standing up a Rime engine -- the same
// reason ComputeSpaceCommitText is a free function.

#include <functional>
#include <string>

#include "context_memory.h"

namespace rime {
namespace context_memory {

struct Options {
  bool enable = false;
  bool use_pane_command = true;
  int max_entries = 256;
  bool debug = false;
};

class Step {
 public:
  Step(Table* table, const Options& options) : table_(table), options_(options) {
    // Gated on `enable`, not merely on `table_`: the table is process-wide and
    // this Step is built in every Copilot -- i.e. once per session -- so an
    // ungated call has every session, including the ones with the feature off,
    // stamping its own max_entries onto state it shares with the others.
    // Nothing needs that write, and cross-session writes to shared state are
    // worth not having.
    if (table_ && options_.enable) {
      table_->set_max_entries(static_cast<size_t>(options_.max_entries));
    }
  }

  // Called at the head of a key event, with the key this event resolved to.
  // `set_mode` is called only when a remembered value exists AND differs.
  void OnHead(const std::string& key, bool current_ascii,
              const std::function<void(bool)>& set_mode) {
    if (!options_.enable || !table_) return;
    resolved_ = true;
    if (key != last_key_) {
      if (auto remembered = table_->Get(key)) {
        if (*remembered != current_ascii) set_mode(*remembered);
      }
      last_key_ = key;
    }
  }

  // Called when this event resolved no identity. Deliberately does NOT clear
  // last_key_: the user has probably not moved, and clearing would make the
  // next resolved event look like a fresh sighting and skip its restore.
  void OnUnresolved() { resolved_ = false; }

  // Called at the tail of a key event that resolved an identity.
  //
  // `mode_written_elsewhere` says that something outside this feature set
  // ascii_mode during THIS event. Today that is ImeBridge: its pending-action
  // queue carries no context, so an `InsertLeave` queued by pane A's nvim is
  // applied on whatever pane the next keystroke happens to be in. Precedence
  // still gives the bridge the mode -- that is the design's rule and this does
  // not change it -- but the bridge's decision must not be attributed to a
  // pane, or the misapplication stops being one keystroke long and becomes a
  // remembered, self-reinforcing fact about a pane nvim never spoke about.
  void OnTail(const std::string& key, bool current_ascii, bool mode_written_elsewhere) {
    if (!options_.enable || !table_ || !resolved_) return;
    if (mode_written_elsewhere) {
      resolved_ = false;
      return;
    }
    table_->Put(key, current_ascii);
  }

  // Called at the tail of a key event that did not.
  void OnTailUnresolved() { resolved_ = false; }

  const std::string& last_key() const { return last_key_; }

  // Whether the table already holds `key`, without the recency update Get
  // performs -- a read with a side effect is not a probe.
  //
  // Delegated rather than left to the caller to reach around to
  // context_memory::Instance(): that call is correct today only because the
  // Step this processor holds happens to have been built over that same
  // singleton, which nothing states and nothing enforces. Step owns the table
  // pointer; asking it is asking the right object.
  bool Contains(const std::string& key) const { return table_ && table_->Contains(key); }

 private:
  Table* table_ = nullptr;
  Options options_;
  // Per-session on purpose: it means "which context did THIS session last
  // serve", and each session has its own ascii_mode to protect. Global would
  // make two applications typed in alternately each see the other's key as a
  // change and fire a restore that was never needed.
  std::string last_key_;
  bool resolved_ = false;
};

}  // namespace context_memory
}  // namespace rime
