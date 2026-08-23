#pragma once

// The exact string the LLM scorer conditions on, in the form the model was
// trained to read.
//
// A NEW header rather than a home in rerank.h, and that is forced rather than
// chosen: one of the three call sites is tools/score_candidates.cc, which
// tools/CMakeLists.txt links against llama.cpp ONLY -- no rime library, no
// plugin objects, no glog -- and which includes no plugin header today.
// rerank.h includes <rime/candidate.h> and <rime/composition.h>, so putting
// these functions there would make the checkpoint-selection tool link the
// whole input method in order to truncate a string. Everything here depends
// on utf8_index.h and auto_spacer_util.h, both header-only with no `.cc` and
// no glog -- NOT history.h, which pulls in glog through history.cc for
// History's own logging and would make score_candidates require a build with
// NDEBUG defined (DLOG compiles to a real, glog-linking LOG() otherwise) just
// to reach a UTF-8 index. tools/CMakeLists.txt already puts ../src on the
// include path.
//
// The Python side of this contract is rime_train/normalize.py's `scoring_form`,
// and test/data/scoring_form_golden.jsonl is what holds the two together. The
// two implementations do not have the same shape and cannot be diffed directly;
// see that function's docstring and the 2026-08-23 surrounding-context design.

#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "auto_spacer_util.h"  // Utf8SequenceLength, Utf8ToCodepoint
#include "utf8_index.h"        // copilot::UTF8

namespace rime {

// What carries an EOS inside a std::string, so the warm cache goes on comparing
// plain strings and no consumer needs a token type. U+0002 (STX) cannot occur in
// real text: AlignToTrainingForm drops every control character before it inserts
// any of these.
inline constexpr char kEosCarrier = '\x02';

namespace scoring_form_detail {

// Characters `normalize()` DELETES. Python keeps `ch == "\n" or
// unicodedata.category(ch)[0] != "C"`, so **every character of major category
// C goes except the newline** -- that is Cc, Cf, Cs AND Co, not just Cc/Cf.
// (Cn, unassigned code points, is a deliberate exception -- see KNOWN LIMIT.)
//
// TAB, CR, VT, FF and NEL (U+0085) are category Cc. They are **deleted, not
// folded to a space** -- `"XX\t\tYY"` normalizes to `"XXYY"` with no space at
// all. That is the one thing this pair of predicates invites an implementer to
// get backwards (they are all `\s` to a regex, and `isspace()` says true for
// every one of them); it is verified against the real `normalize()` rather than
// inferred, and the golden fixture carries a case for it. Check
// `IsDroppedControl` BEFORE `IsFoldedWhitespace`.
//
// The ranges below are generated, not hand-picked, from:
//
//   python3 -c "
//   import unicodedata
//   def cat(cp):
//       try: return unicodedata.category(chr(cp))
//       except ValueError: return 'Cn'
//   runs=[]
//   for cp in range(0x110000):
//       if cp == 0x0A: continue
//       c = cat(cp)
//       drop = c in ('Cc','Cf','Cs','Co')
//       if drop:
//           if runs and runs[-1][1] == cp-1 and runs[-1][2] == c: runs[-1][1] = cp
//           else: runs.append([cp,cp,c])
//   for a,b,c in runs:
//       print(f'  if (cp >= 0x{a:04X} && cp <= 0x{b:04X}) return true;  // {c}' if a!=b
//             else f'  if (cp == 0x{a:04X}) return true;  // {c}')
//   "
//
// Generated against Unicode 16.0.0 (verified 2026-08-23 -- `unicodedata`'s
// version is `unicodedata.unidata_version` on the interpreter that ran the
// script above). Re-run this and diff if this list is ever suspected to have
// drifted from a newer Unicode version; do not hand-edit the ranges.
//
// KNOWN LIMIT: Cn (unassigned) is deliberately NOT covered. Closing it needs a
// full Unicode assignment table this header is not going to ship, and Cn is by
// definition code points no font renders and no input method emits today. The
// cost of leaving it open is small and one-directional: an unassigned code
// point that somehow reaches `before` survives as a single byte-fallback token
// in the training-unseen position, consuming ONE OR TWO characters of the
// `max_chars` budget, not always one. It is one when the surviving code point
// has no adjacent foldable whitespace to interfere with. It is two when it
// does, because the surviving code point also blocks the space collapse that
// would otherwise have happened around it: Python's normalize() deletes the
// Cn outright and then collapses the resulting run of whitespace to one
// space, so "a <Cn> b" comes out 3 characters ("a b"); this function never
// deletes the Cn, so the space on each side folds on its own instead of
// merging with its neighbour across the gap, and the same input comes out 5
// characters ("a", " ", <Cn>, " ", "b") -- two more than Python's answer, not
// one more. It does not corrupt neighbouring characters beyond that.
inline bool IsDroppedControl(uint32_t cp) {
  if (cp == 0x0A) return false;                     // the only control that survives
  if (cp >= 0x0000 && cp <= 0x0009) return true;    // Cc
  if (cp >= 0x000B && cp <= 0x001F) return true;    // Cc
  if (cp >= 0x007F && cp <= 0x009F) return true;    // Cc (DEL + C1, incl. NEL 0x85)
  if (cp == 0x00AD) return true;                    // Cf (soft hyphen)
  if (cp >= 0x0600 && cp <= 0x0605) return true;    // Cf
  if (cp == 0x061C) return true;                    // Cf
  if (cp == 0x06DD) return true;                    // Cf
  if (cp == 0x070F) return true;                    // Cf
  if (cp >= 0x0890 && cp <= 0x0891) return true;    // Cf
  if (cp == 0x08E2) return true;                    // Cf
  if (cp == 0x180E) return true;                    // Cf
  if (cp >= 0x200B && cp <= 0x200F) return true;    // Cf (ZWSP..RLM)
  if (cp >= 0x202A && cp <= 0x202E) return true;    // Cf (bidi embedding)
  if (cp >= 0x2060 && cp <= 0x2064) return true;    // Cf (word joiner, etc.)
  if (cp >= 0x2066 && cp <= 0x206F) return true;    // Cf (bidi isolates)
  if (cp >= 0xD800 && cp <= 0xDFFF) return true;    // Cs (surrogates)
  if (cp >= 0xE000 && cp <= 0xF8FF) return true;    // Co (BMP private use)
  if (cp == 0xFEFF) return true;                    // Cf (BOM)
  if (cp >= 0xFFF9 && cp <= 0xFFFB) return true;    // Cf
  if (cp == 0x110BD) return true;                   // Cf
  if (cp == 0x110CD) return true;                   // Cf
  if (cp >= 0x13430 && cp <= 0x1343F) return true;  // Cf
  if (cp >= 0x1BCA0 && cp <= 0x1BCA3) return true;  // Cf
  if (cp >= 0x1D173 && cp <= 0x1D17A) return true;  // Cf
  if (cp == 0xE0001) return true;                   // Cf
  if (cp >= 0xE0020 && cp <= 0xE007F) return true;  // Cf
  if (cp >= 0xF0000 && cp <= 0xFFFFD) return true;  // Co (plane 15 private use)
  return cp >= 0x100000 && cp <= 0x10FFFD;          // Co (plane 16 private use)
}

// Characters a run of which becomes ONE space: the newline, plus the Unicode
// space separators (Zs) and line/paragraph separators (Zl/Zp). These survive
// `normalize()`'s control filter and are then collapsed by `_WS.sub(" ")`.
//
// NOT `isspace()`, in either direction: `isspace()` says true for TAB and CR,
// which are deleted above, and false for U+00A0 and U+3000, which are exactly
// what powerlevel10k padding and full-width input produce.
inline bool IsFoldedWhitespace(uint32_t cp) {
  return cp == 0x0A || cp == 0x20 || cp == 0xA0 || cp == 0x1680 || (cp >= 0x2000 && cp <= 0x200A) ||
         cp == 0x2028 || cp == 0x2029 || cp == 0x202F || cp == 0x205F || cp == 0x3000;
}

// _SENT_END's character class. Commas and enumeration commas deliberately do
// NOT end a sentence -- see normalize.text_sentences.
inline bool IsSentenceEnder(uint32_t cp) {
  return cp == 0x3002 ||  // 。
         cp == 0xFF01 ||  // ！
         cp == 0xFF1F ||  // ？
         cp == 0xFF1B ||  // ；
         cp == '!' || cp == '?' || cp == ';';
}

}  // namespace scoring_form_detail

// The training stream's shape for one continuous string. Mirrors
// rime_train/normalize.py's `scoring_form` exactly; see that docstring for why
// each rule is what it is, and for the two selection rules deliberately NOT
// copied.
//
//   1. Cc/Cf characters DELETED (tab, CR, VT, FF and NEL among them -- they do
//      not become spaces); the newline and the Unicode space separators folded,
//      a run of them to one space; leading and trailing whitespace stripped
//   2. an EOS carrier after every sentence ender
//   3. no whitespace on either side of a carrier
//
// A newline folds to a space, NOT to an EOS: mapping it to EOS would render the
// user's line break as the symbol the model learned to read as a full stop.
inline std::string AlignToTrainingForm(const std::string& text) {
  // Pass 1: drop controls, collapse whitespace, strip both ends.
  std::string folded;
  folded.reserve(text.size());
  bool pending_space = false;
  size_t i = 0;
  while (i < text.size()) {
    const size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(text[i]));
    if (len == 0 || i + len > text.size()) {
      // Malformed or truncated UTF-8: drop this ONE byte and keep going,
      // matching `copilot::UTF8`'s `SplitU8` (utf8_index.h), which does the
      // same rather than discarding everything after the bad byte. `before`
      // comes from a tmux `capture-pane` slice or from IMK surrounding text,
      // and neither guarantees a well-formed boundary -- the text after a
      // stray byte is the caret-adjacent text this function exists to keep.
      ++i;
      continue;
    }
    const std::string ch = text.substr(i, len);
    const uint32_t cp = auto_spacer_detail::Utf8ToCodepoint(ch);
    i += len;
    // Order matters: TAB and CR are Cc and must be deleted, and they would
    // also satisfy any "is this whitespace" test written from isspace().
    if (scoring_form_detail::IsDroppedControl(cp)) continue;
    if (scoring_form_detail::IsFoldedWhitespace(cp)) {
      // Never leading: `folded.empty()` is what makes this the .strip().
      pending_space = !folded.empty();
      continue;
    }
    if (pending_space) {
      folded += ' ';
      pending_space = false;
    }
    folded += ch;
  }
  // `pending_space` still true here is trailing whitespace: dropped, not emitted.

  // Pass 2: insert the carrier after every sentence ender, and drop the space
  // that would otherwise sit beside it. Python gets this for free by stripping
  // every chunk; here it is explicit, and it is the rule an implementation gets
  // wrong by default.
  std::string out;
  out.reserve(folded.size() + 8);
  bool skip_space = false;
  size_t k = 0;
  while (k < folded.size()) {
    const size_t len =
        auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(folded[k]));
    if (len == 0 || k + len > folded.size()) {
      // Same policy as pass 1, and for the same reason -- see there. `folded`
      // is built only from valid UTF-8 substrings plus ASCII spaces and the
      // single-byte EOS carrier, so this branch should not be reachable in
      // practice; it exists so the two passes cannot silently disagree.
      ++k;
      continue;
    }
    const std::string ch = folded.substr(k, len);
    const uint32_t cp = auto_spacer_detail::Utf8ToCodepoint(ch);
    k += len;
    if (skip_space && cp == ' ') continue;
    skip_space = false;
    out += ch;
    if (scoring_form_detail::IsSentenceEnder(cp)) {
      out += kEosCarrier;
      skip_space = true;
    }
  }
  return out;
}

// The last `max_chars` characters of the aligned form.
//
// ALIGN FIRST, THEN TRUNCATE. Whitespace collapsing changes the character count,
// so truncating first would make max_chars mean "characters before alignment"
// rather than characters the model reads. The carrier counts as the one
// character it becomes.
inline std::string BuildScoringContext(const std::string& before, int max_chars) {
  if (before.empty() || max_chars <= 0) {
    return {};
  }
  const std::string aligned = AlignToTrainingForm(before);
  if (aligned.empty()) {
    return {};
  }
  ::copilot::UTF8 utf8(aligned);
  const int n = static_cast<int>(utf8.size());
  const int take = std::min(n, max_chars);
  std::string out;
  for (int i = n - take; i < n; ++i) {
    out += std::string(utf8[i]);
  }
  return out;
}

// Tokenize an aligned string, substituting the real EOS id for each carrier.
//
// This does NOT happen on its own. llama_tokenize's `parse_special` matches a
// special token's TEXT ("</s>"), not its id, so a raw 0x02 byte byte-falls-back
// to the <0x02> piece -- token 5 on this vocabulary (3 specials, then 256 byte
// tokens) -- an embedding as untrained as the BOS this change removes.
//
// Templated on the token type so this header stays free of <llama.h>, which is
// what keeps it usable from the plugin and from both llama-only tools.
//
// `raw` must tokenize one run with add_special = FALSE: BOS never appears in the
// training stream (train.py writes `sentence + EOS` repeated, so token 1 is
// neither input nor target and its embedding sits at initialization).
//
// Inserting EOS mid-context is the form the model knows: train.py cuts windows
// from one continuous stream with no document masking, so it was trained to
// attend across EOS rather than to treat it as a reset.
template <typename Token, typename RawTokenize>
inline std::vector<Token> TokenizeScoringForm(const std::string& aligned, Token eos_id,
                                              RawTokenize raw) {
  std::vector<Token> out;
  if (aligned.empty()) {
    return out;
  }
  size_t pos = 0;
  bool first = true;
  while (true) {
    const size_t stx = aligned.find(kEosCarrier, pos);
    const std::string piece =
        aligned.substr(pos, stx == std::string::npos ? std::string::npos : stx - pos);
    if (!first) {
      out.push_back(eos_id);
    }
    first = false;
    if (!piece.empty()) {
      const auto toks = raw(piece);
      out.insert(out.end(), toks.begin(), toks.end());
    }
    if (stx == std::string::npos) break;
    pos = stx + 1;
  }
  return out;
}

}  // namespace rime
