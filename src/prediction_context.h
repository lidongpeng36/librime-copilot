#pragma once

// Pure helpers turning the caret's surrounding text into prediction context.
//
// The copilot db is keyed by short character sequences (see
// tools/make_copilot_data: word prefixes and the first word of a bigram), which
// is exactly the shape of "the last few characters before the caret". These
// functions do that conversion and nothing else — no Rime types, no IMK, no db
// — so they can be unit-tested directly (test/prediction_context_test.cc).

#include <algorithm>
#include <string>
#include <vector>

#include "history.h"  // copilot::UTF8

namespace rime {

// Combine the surrounding-text snapshot with the text this key event just
// committed.
//
// The IMK hook runs before the app processes the key (and is skipped entirely
// while composing), so the snapshot predates the commit that triggers the
// prediction. `committed` is appended unless the snapshot already ends with it,
// which guards against clients whose marked range updates early enough that the
// commit is already visible.
inline std::string BuildPredictionContext(const std::string& before, const std::string& committed) {
  if (committed.empty()) {
    return before;
  }
  if (before.size() >= committed.size() &&
      before.compare(before.size() - committed.size(), committed.size(), committed) == 0) {
    return before;
  }
  return before + committed;
}

// The db lookup keys for `context`: its last 1, 2, ... characters, shortest
// first (the order DBProvider has always issued them in).
//
// `max_hints` caps the longest key; <= 0 means "no cap", matching
// DBProvider::Config. Slicing is by UTF-8 character, never by byte.
inline std::vector<std::string> BuildLookupKeys(const std::string& context, int max_hints) {
  std::vector<std::string> keys;
  if (context.empty()) {
    return keys;
  }
  ::copilot::UTF8 utf8(context);
  const int n = static_cast<int>(utf8.size());
  const int limit = max_hints > 0 ? std::min(max_hints, n) : n;
  keys.reserve(static_cast<size_t>(limit));
  for (int len = 1; len <= limit; ++len) {
    keys.emplace_back(utf8(-len, -1));
  }
  return keys;
}

}  // namespace rime
