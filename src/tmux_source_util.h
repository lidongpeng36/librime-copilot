#pragma once

// Pure helpers that turn one tmux query's stdout into surrounding text.
//
// Kept free of any process/IO concern so the GTest suite can drive every rule
// without a tmux server, the same split `auto_spacer_util.h` uses.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <vector>

#include "auto_spacer_util.h"  // Utf8SequenceLength, Utf8ToCodepoint
#include "history.h"           // copilot::UTF8

namespace rime {
namespace tmux_detail {

// One query's worth of state, parsed out of the single exec's stdout.
struct Snapshot {
  std::string pane_id;
  int cursor_x = 0;  // display column, 0-based
  int cursor_y = 0;  // row within the visible pane, 0-based
  int pane_width = 0;
  std::vector<std::string> rows;           // capture-pane -p, one row per entry
  std::vector<long long> client_activity;  // one per attached client
  // tmux's global `focus-events`. Load-bearing whenever more than one client
  // is attached: it is what makes tmux's "current client" track macOS window
  // focus, and it is *off* by default. See JudgeClients.
  bool focus_events = false;
};

struct Context {
  std::string before;
  std::string after;
};

// Upper bound on a plausible `pane_width`. Real terminals top out in the low
// hundreds of columns (a 4K display at a tiny font is still under 1000); a
// few thousand is generously beyond anything real while staying far below
// where `pane_width` stops being able to bound `cursor_x` safely. Both come
// straight from `std::atoi` on a tmux header field (ParseTmuxOutput), which
// on a malformed or adversarial answer returns whatever -- e.g.
// `CUR|%0|2000000000|0|2000000000` passes `cursor_x <= pane_width` cleanly
// while still being ~2e9. Without this bound, ExtractContext's existing
// `cursor_x > pane_width` guard cannot catch that case, and
// SliceBeforeColumn would try to append billions of blank characters on the
// input thread.
constexpr int kMaxPaneWidth = 4096;

// East Asian Wide/Fullwidth occupy two cells; combining marks occupy none.
// tmux computes cursor_x with the same rule, so this must agree with it.
inline int DisplayWidth(uint32_t cp) {
  if (cp >= 0x0300 && cp <= 0x036F) return 0;
  if (cp == 0x200B) return 0;
  const bool wide = (cp >= 0x1100 && cp <= 0x115F) || (cp >= 0x2E80 && cp <= 0x303E) ||
                    (cp >= 0x3041 && cp <= 0x33FF) || (cp >= 0x3400 && cp <= 0x4DBF) ||
                    (cp >= 0x4E00 && cp <= 0x9FFF) || (cp >= 0xA000 && cp <= 0xA4CF) ||
                    (cp >= 0xA960 && cp <= 0xA97F) || (cp >= 0xAC00 && cp <= 0xD7A3) ||
                    (cp >= 0xF900 && cp <= 0xFAFF) || (cp >= 0xFE10 && cp <= 0xFE19) ||
                    (cp >= 0xFE30 && cp <= 0xFE6F) || (cp >= 0xFF00 && cp <= 0xFF60) ||
                    (cp >= 0xFFE0 && cp <= 0xFFE6) || (cp >= 0x1F300 && cp <= 0x1F64F) ||
                    (cp >= 0x1F900 && cp <= 0x1F9FF) || (cp >= 0x20000 && cp <= 0x3FFFD);
  return wide ? 2 : 1;
}

inline int DisplayWidthOf(const std::string& s) {
  int w = 0;
  size_t i = 0;
  while (i < s.size()) {
    size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(s[i]));
    if (len == 0 || i + len > s.size()) break;
    w += DisplayWidth(auto_spacer_detail::Utf8ToCodepoint(s.substr(i, len)));
    i += len;
  }
  return w;
}

// Everything strictly left of display column `col`.
inline std::string SliceBeforeColumn(const std::string& row, int col) {
  if (col <= 0) return "";
  std::string out;
  int w = 0;
  size_t i = 0;
  while (i < row.size() && w < col) {
    size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(row[i]));
    if (len == 0 || i + len > row.size()) break;
    std::string ch = row.substr(i, len);
    int cw = DisplayWidth(auto_spacer_detail::Utf8ToCodepoint(ch));
    if (w + cw > col) break;  // caret sits inside a wide glyph
    out += ch;
    w += cw;
    i += len;
  }
  // tmux trims trailing blanks off captured rows, so a column we could not
  // reach *because the row ran out* was really a blank cell: materialise it,
  // or AutoSpacer sees "nothing" and inserts a second space. But a column we
  // could not reach because the caret landed inside a wide glyph is a
  // different case entirely — the row still has content there, it just isn't
  // ours to include; no padding belongs in that gap.
  if (i >= row.size() && w < col) out.append(static_cast<size_t>(col - w), ' ');
  return out;
}

inline std::string SliceAfterColumn(const std::string& row, int col) {
  const std::string before = SliceBeforeColumn(row, col);
  // A `before` longer than the row means padding was synthesized, i.e. the
  // caret is past end-of-row and there is nothing after it.
  if (before.size() >= row.size()) return "";
  return row.substr(before.size());
}

// Last `n` UTF-8 characters. `copilot::UTF8` takes negative indices; u(-n, -1)
// is the documented tail idiom (see src/rerank.h:44).
inline std::string TailChars(const std::string& s, int n) {
  if (n <= 0 || s.empty()) return "";
  ::copilot::UTF8 u(s);
  if (static_cast<int>(u.size()) <= n) return s;
  return std::string(u(-n, -1));
}

// Prompt themes pad with exotic blanks (powerlevel10k uses U+00A0). They are
// blanks to the eye, so they must be blanks to the spacing rules too.
inline std::string NormalizeBlanks(const std::string& s) {
  std::string out;
  size_t i = 0;
  while (i < s.size()) {
    size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(s[i]));
    if (len == 0 || i + len > s.size()) break;
    const std::string ch = s.substr(i, len);
    const uint32_t cp = auto_spacer_detail::Utf8ToCodepoint(ch);
    const bool blank = cp == 0x00A0 || cp == 0x202F || cp == 0x2007 ||
                       (cp >= 0x2000 && cp <= 0x200A) || cp == 0x3000;
    out += blank ? " " : ch;
    i += len;
  }
  return out;
}

// `#{focus-events}` renders as tmux's boolean format ("1"/"0"), but
// `show-options`-style spellings exist for choice options and older tmux may
// not know the variable at all (rendering it empty). Anything we do not
// positively recognize as "on" counts as off, which only ever costs a refusal.
inline bool ParseTmuxFlag(const std::string& value) {
  return value == "1" || value == "on" || value == "true";
}

// `CLI|<activity>` and `FOC|<flag>` lines, then one
// `CUR|<pane>|<x>|<y>|<width>` header, then the pane dump. Rows after the
// header are taken verbatim, so a row that happens to start with "CUR|" stays
// a row.
inline std::optional<Snapshot> ParseTmuxOutput(const std::string& raw) {
  Snapshot snap;
  bool saw_header = false;
  size_t pos = 0;
  while (pos <= raw.size()) {
    const size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) {
      if (pos < raw.size() && saw_header) snap.rows.push_back(raw.substr(pos));
      break;
    }
    std::string line = raw.substr(pos, nl - pos);
    pos = nl + 1;

    if (saw_header) {
      snap.rows.push_back(std::move(line));
      continue;
    }
    if (line.rfind("CLI|", 0) == 0) {
      snap.client_activity.push_back(std::atoll(line.c_str() + 4));
      continue;
    }
    if (line.rfind("FOC|", 0) == 0) {
      snap.focus_events = ParseTmuxFlag(line.substr(4));
      continue;
    }
    if (line.rfind("CUR|", 0) == 0) {
      std::vector<std::string> f;
      size_t s = 0;
      while (true) {
        const size_t bar = line.find('|', s);
        f.push_back(line.substr(s, bar == std::string::npos ? bar : bar - s));
        if (bar == std::string::npos) break;
        s = bar + 1;
      }
      if (f.size() < 5) return std::nullopt;
      snap.pane_id = f[1];
      snap.cursor_x = std::atoi(f[2].c_str());
      snap.cursor_y = std::atoi(f[3].c_str());
      snap.pane_width = std::atoi(f[4].c_str());
      saw_header = true;
      continue;
    }
    // Anything else before the header means tmux errored; refuse.
    return std::nullopt;
  }
  if (!saw_header) return std::nullopt;
  return snap;
}

// client_activity is a unix timestamp in whole seconds. Two attached clients
// stamped in the same second are indistinguishable, and picking one would be a
// coin flip on which terminal the keyboard is pointed at. Refuse instead.
inline bool ClientsAreAmbiguous(std::vector<long long> activity) {
  if (activity.size() <= 1) return false;
  std::sort(activity.rbegin(), activity.rend());
  return activity[0] == activity[1];
}

// Whether the attached-client list lets us believe the pane tmux resolved is
// the one the keyboard is pointed at. Deliberately separate from
// `ClientsAreAmbiguous`: "no client at all" is absence, not ambiguity, and
// folding it into the same predicate is what let the detached-session case
// slip through.
enum class ClientVerdict {
  kAccept,
  kNoClientAttached,
  kFocusEventsOff,
  kAmbiguousClients,
};

inline const char* DescribeVerdict(ClientVerdict verdict) {
  switch (verdict) {
    case ClientVerdict::kAccept:
      return "accepted";
    case ClientVerdict::kNoClientAttached:
      return "no attached client; display-message would answer from a detached session";
    case ClientVerdict::kFocusEventsOff:
      return "multiple clients with focus-events off; tmux's current client does not track "
             "window focus";
    case ClientVerdict::kAmbiguousClients:
      return "attached clients tied on activity";
  }
  return "unknown";
}

inline ClientVerdict JudgeClients(const std::vector<long long>& activity, bool focus_events) {
  // An empty list means nothing is attached to this server. `display-message
  // -p` with no target still answers, falling back to the most-recently-used
  // session, so we would hand AutoSpacer a pane nobody is looking at -- e.g.
  // the user typing at a bare shell while a tmux session sits detached.
  if (activity.empty()) {
    return ClientVerdict::kNoClientAttached;
  }
  if (activity.size() > 1) {
    // With >1 client tmux answers for the most recently active one. That
    // tracks macOS window focus only because `focus-events on` makes tmux
    // enable DECSET 1004, so focusing a window writes a focus sequence to that
    // client's tty and bumps its client_activity. focus-events is off by
    // default; without it the "most recent" client is merely whichever one was
    // last typed into, and answering from it is exactly the cross-talk this
    // source exists to avoid -- confidently, which is worse than a tie.
    if (!focus_events) {
      return ClientVerdict::kFocusEventsOff;
    }
    if (ClientsAreAmbiguous(activity)) {
      return ClientVerdict::kAmbiguousClients;
    }
  }
  return ClientVerdict::kAccept;
}

inline std::optional<Context> ExtractContext(const Snapshot& snap, int prefix_chars) {
  if (snap.cursor_y < 0 || snap.cursor_y >= static_cast<int>(snap.rows.size())) {
    return std::nullopt;
  }
  // cursor_x is a pane-relative display column, so pane_width bounds it. The
  // bound is not cosmetic: SliceBeforeColumn materialises the blanks tmux
  // trimmed off the row end, so an unbounded column out of a malformed header
  // (`std::atoi` will happily return 2000000000) becomes a multi-gigabyte
  // append on the input thread. `== pane_width` is allowed: the caret legally
  // sits one past the last cell just before a wrap. pane_width itself must
  // also be bounded (kMaxPaneWidth): it is the same unchecked std::atoi
  // output, and a header with both fields equally huge
  // (`CUR|%0|2000000000|0|2000000000`) would pass the cursor_x <= pane_width
  // check while still driving the same multi-gigabyte append.
  if (snap.pane_width <= 0 || snap.pane_width > kMaxPaneWidth || snap.cursor_x < 0 ||
      snap.cursor_x > snap.pane_width) {
    return std::nullopt;
  }
  const std::string& row = snap.rows[snap.cursor_y];

  std::string before;
  if (snap.cursor_x == 0) {
    // Column 0 is either a fresh line or the continuation of a wrapped one.
    // Only a row above that fills the pane exactly is a wrap; treating a short
    // row as adjacent would glue two unrelated lines together.
    if (snap.cursor_y > 0) {
      const std::string& prev = snap.rows[snap.cursor_y - 1];
      if (DisplayWidthOf(prev) == snap.pane_width) before = prev;
    }
  } else {
    before = SliceBeforeColumn(row, snap.cursor_x);
  }

  Context ctx;
  ctx.before = NormalizeBlanks(TailChars(before, prefix_chars));
  ctx.after =
      NormalizeBlanks(auto_spacer_detail::GetFirstUtf8Char(SliceAfterColumn(row, snap.cursor_x)));
  return ctx;
}

// The one-exec argv: marker-tagged lines first (list-clients, focus-events),
// then the cursor header, then the raw pane dump — in that order so a pane
// dump line that happens to start with "CLI|", "FOC|" or "CUR|" can never be
// mistaken for a marker (ParseTmuxOutput only treats those prefixes specially
// before the header is seen). No `-t` anywhere: display-message and
// capture-pane must resolve the *same* current pane, and letting tmux decide
// that is the point.
//
// focus-events rides along in this same exec on purpose: a second spawn to
// read one option would double the per-key cost of the whole feature.
inline std::vector<std::string> BuildTmuxArgs(const std::string& socket) {
  std::vector<std::string> args;
  if (!socket.empty()) {
    args.push_back("-S");
    args.push_back(socket);
  }
  args.push_back("list-clients");
  args.push_back("-F");
  args.push_back("CLI|#{client_activity}");
  args.push_back(";");
  args.push_back("display-message");
  args.push_back("-p");
  args.push_back("-F");
  args.push_back("FOC|#{focus-events}");
  args.push_back(";");
  args.push_back("display-message");
  args.push_back("-p");
  args.push_back("-F");
  args.push_back("CUR|#{pane_id}|#{cursor_x}|#{cursor_y}|#{pane_width}");
  args.push_back(";");
  args.push_back("capture-pane");
  args.push_back("-p");
  return args;
}

// The pane id MUST be in the key: AutoSpacer indexes per-client state by it
// (src/auto_spacer.cc:282-286), so a constant key would let one pane's
// spacing state bleed into the next.
inline std::string MakeClientKey(const std::string& socket, const std::string& pane_id) {
  const std::string socket_tag = socket.empty() ? "default" : socket;
  return "tmux:" + socket_tag + ":" + pane_id;
}

}  // namespace tmux_detail
}  // namespace rime
