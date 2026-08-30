#pragma once

// Reconstruct the text around the caret from what this input method has
// committed, for the case where nothing can tell us the real thing.
//
// This is the fourth rung of the caret-context chain (see
// docs/superpowers/specs/2026-08-29-caret-context-consolidation-design.md).
// It is not a degraded copy of the other three: it answers a strictly weaker
// question -- "what did I put there" rather than "what is there" -- and it is
// wrong the moment anything else edits the text.
//
// Header-only and free of any Rime dependency on purpose: it is what makes the
// equivalence table in test/ possible, since no test in this tree can stand up
// a Rime engine.

#include <string>

namespace rime {
namespace caret {

struct ReconstructInput {
  // ctx->commit_history().latest_text(): the text of the LAST record only
  // (librime commit_history.h:32 -- `back().text`, not a concatenation).
  // Matching that exactly is what makes this a behaviour-preserving move of
  // the logic that used to be inline in AutoSpacer.
  std::string latest_text;
};

struct Reconstructed {
  std::string before;
  std::string after;
  // False means "I cannot answer", NOT "the text before the caret is empty".
  // An empty `before` is a positive claim that the caret sits at the start of
  // the text, and NeedSpaceBefore acts on that claim by returning false --
  // suppressing a space that may well belong there. The two must not be
  // conflated.
  bool usable = false;
};

inline Reconstructed ReconstructFromHistory(const ReconstructInput& in) {
  Reconstructed out;
  if (in.latest_text.empty()) {
    return out;  // usable stays false
  }
  out.before = in.latest_text;
  // Deliberately empty: the commit history describes only what is behind the
  // caret. NeedSpaceAfter (auto_spacer_util.h) returns false on it at its own
  // first guard -- GetFirstUtf8Char("") is empty, and `ch.empty()` is the
  // first disjunct there -- which is exactly what the path this replaces did:
  // it never added a right-hand space.
  //
  // Named, not cited by line: this comment used to point at
  // auto_spacer_util.h:180-182, and task 6's IsAsciiPunctuationCode shifted
  // everything below it by five lines, so those numbers came to land inside
  // NeedSpaceBefore -- a different predicate with a guard that happens to look
  // the same. A number that has already rotted once will rot again.
  out.after.clear();
  out.usable = true;
  return out;
}

}  // namespace caret
}  // namespace rime
