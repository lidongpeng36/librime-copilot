//
// Read-only probe for copilot.db.
//
// Prints what the plugin sees for a context key: which continuations exist,
// their weights, their rank, and whether any are duplicated. The prediction and
// re-ranking paths both key off exactly this, so when candidates come out in a
// surprising order this is the first thing to look at.
//
//   dump_copilot <db> <key>...            # top continuations of each key
//   dump_copilot <db> --find 议 -- 建     # where a given continuation ranks
//
#include <rime/common.h>

#include <algorithm>
#include <iostream>
#include <map>
#include <string>
#include <vector>

#include "copilot_db.h"
#include "provider.h"

using namespace rime;

namespace {

constexpr size_t kShow = 15;

std::vector<::copilot::Entry> Continuations(CopilotDb& db, const std::string& key) {
  std::vector<::copilot::Entry> entries;
  auto* candidates = db.Lookup(key);
  if (!candidates) {
    return entries;
  }
  entries.reserve(candidates->size);
  auto* entry = candidates->begin();
  for (uint32_t i = 0; i < candidates->size; ++i, ++entry) {
    entries.push_back({db.GetEntryText(*entry), entry->weight, ::copilot::ProviderType::kDB});
  }
  return entries;
}

void Report(CopilotDb& db, const std::string& key, const std::vector<std::string>& find) {
  auto entries = Continuations(db, key);
  std::cout << "\n=== key '" << key << "' ===\n";
  if (entries.empty()) {
    std::cout << "  (no such key)\n";
    return;
  }
  std::sort(entries.begin(), entries.end(),
            [](const auto& a, const auto& b) { return a.weight > b.weight; });

  std::map<std::string, int> counts;
  for (const auto& entry : entries) {
    ++counts[entry.text];
  }
  size_t duplicated = 0;
  for (const auto& [text, n] : counts) {
    if (n > 1) {
      ++duplicated;
    }
  }
  std::cout << "  " << entries.size() << " continuations, " << counts.size() << " distinct";
  if (duplicated) {
    std::cout << ", " << duplicated << " duplicated";
  }
  std::cout << "\n";

  for (size_t i = 0; i < entries.size() && i < kShow; ++i) {
    printf("  %3zu. %-16s w=%.0f\n", i + 1, entries[i].text.c_str(), entries[i].weight);
  }
  if (entries.size() > kShow) {
    std::cout << "  ... " << entries.size() - kShow << " more\n";
  }
  for (const auto& wanted : find) {
    auto it = std::find_if(entries.begin(), entries.end(),
                           [&](const auto& e) { return e.text == wanted; });
    if (it == entries.end()) {
      std::cout << "  -> '" << wanted << "' is not a continuation of '" << key << "'\n";
    } else {
      printf("  -> '%s' ranks %zu of %zu, w=%.0f\n", wanted.c_str(),
             static_cast<size_t>(it - entries.begin()) + 1, entries.size(), it->weight);
    }
  }
}

}  // namespace

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "usage: " << argv[0] << " <db> [--find <text>]... [--] <key>...\n";
    return 1;
  }
  std::vector<std::string> find;
  std::vector<std::string> keys;
  for (int i = 2; i < argc; ++i) {
    std::string arg = argv[i];
    if (arg == "--find" && i + 1 < argc) {
      find.push_back(argv[++i]);
    } else if (arg == "--") {
      continue;
    } else {
      keys.push_back(arg);
    }
  }

  CopilotDb db{path(argv[1])};
  if (!db.Load()) {
    std::cerr << "failed to load " << argv[1] << "\n";
    return 1;
  }
  for (const auto& key : keys) {
    Report(db, key, find);
  }
  return 0;
}
