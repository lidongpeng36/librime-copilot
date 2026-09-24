#pragma once

// The word-head prior's table: for each character, how often the user's own
// corpus uses it as the FIRST character of a multi-character word rather than
// on its own. Written by `rime-copilot wordhead` (tools/rime_copilot/
// wordhead.py), read here; test/data/wordhead_golden.txt pins the two sides.
//
// Why a prior, and why it is gated on the whole window rather than per
// candidate: docs/superpowers/specs/2026-09-23-wordhead-prior-design.md (kept
// locally). The short version is that the scorer's P(安 | 可以) counts every
// word 安 begins, so among single characters the productive word heads score
// high for a reason that has nothing to do with the user's intent.

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <istream>
#include <string>
#include <unordered_map>
#include <vector>

#include "rerank_llm.h"  // llm_rerank::IsAllHan, ::copilot::UTF8

namespace rime {
namespace wordhead {

// Upper clamp on mass before log(1 - mass): a character the corpus has only
// ever seen as a word head would otherwise score -inf and make every ranking
// containing it degenerate. log(1 - 0.999) = -6.9.
inline constexpr float kMaxMass = 0.999f;

struct Table {
  std::unordered_map<std::string, float> mass;
  bool empty() const { return mass.empty(); }
};

// Lines are `<one Han character>\t<mass in [0, 1]>`; blank lines and `#`
// comments are skipped. Anything else is counted in `bad_lines` and dropped,
// never guessed at. A repeated character keeps its FIRST value and counts the
// rest as bad, so a hand-edited table cannot silently depend on which
// duplicate wins.
struct ParseResult {
  Table table;
  int bad_lines = 0;
};

inline bool IsOneHanCharacter(const std::string& text) {
  return llm_rerank::IsAllHan(text) && ::copilot::UTF8(text).size() == 1;
}

inline ParseResult Parse(std::istream& in) {
  ParseResult r;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') {
      line.pop_back();
    }
    if (line.empty() || line[0] == '#') {
      continue;
    }
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      ++r.bad_lines;
      continue;
    }
    const std::string key = line.substr(0, tab);
    const std::string value = line.substr(tab + 1);
    char* end = nullptr;
    const float mass = std::strtof(value.c_str(), &end);
    const bool numeric = !value.empty() && end == value.c_str() + value.size();
    if (!IsOneHanCharacter(key) || !numeric || !std::isfinite(mass) || mass < 0.0f || mass > 1.0f ||
        r.table.mass.count(key)) {
      ++r.bad_lines;
      continue;
    }
    r.table.mass.emplace(key, mass);
  }
  return r;
}

// false when the file cannot be opened; `*error` then names the path.
inline bool Load(const std::string& path, ParseResult* out, std::string* error) {
  std::ifstream in(path);
  if (!in) {
    if (error) {
      *error = "cannot open " + path;
    }
    return false;
  }
  *out = Parse(in);
  return true;
}

// weight * log(1 - mass(c)); 0 when `cand` is not one character or is absent.
inline float PriorOf(const Table& table, const std::string& cand, float weight) {
  if (weight <= 0.0f) {
    return 0.0f;
  }
  const auto it = table.mass.find(cand);
  if (it == table.mass.end()) {
    return 0.0f;
  }
  const float m = std::min(std::max(it->second, 0.0f), kMaxMass);
  return weight * std::log(1.0f - m);
}

// Priors parallel to `candidates`, or EMPTY -- which Decide() reads as no
// prior at all -- unless every all-Han candidate is exactly one character and
// there is at least one. Window-level on purpose: the measurement that
// justifies the prior only ever applied it between two single characters, and
// a per-candidate gate would penalise a character against a word in the six
// cross-length promotions the log holds, every one of them harmless.
// Non-Han candidates (the raw input) never block the window and always get 0.
inline std::vector<float> WindowPriors(const Table& table,
                                       const std::vector<std::string>& candidates, float weight) {
  if (weight <= 0.0f || table.empty()) {
    return {};
  }
  bool any_han = false;
  for (const auto& c : candidates) {
    if (!llm_rerank::IsAllHan(c)) {
      continue;
    }
    if (::copilot::UTF8(c).size() != 1) {
      return {};
    }
    any_han = true;
  }
  if (!any_han) {
    return {};
  }
  std::vector<float> priors;
  priors.reserve(candidates.size());
  for (const auto& c : candidates) {
    priors.push_back(llm_rerank::IsAllHan(c) ? PriorOf(table, c, weight) : 0.0f);
  }
  return priors;
}

}  // namespace wordhead
}  // namespace rime
