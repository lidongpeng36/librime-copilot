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
#include "history.h"           // copilot::UTF8, copilot::CharCount -- should be utf8_index.h; see
                               // MakeClientKey below. Not changed in this wave.
#include "imk_client.h"        // Truncation

namespace rime {
namespace tmux_detail {

// SGR attributes we can tell apart. Only the ones a TUI actually uses to mark
// text as "not yours" are here; anything unrecognized leaves the cell alone.
enum CellAttr : uint16_t {
  kAttrBold = 1 << 0,
  kAttrDim = 1 << 1,
  kAttrItalic = 1 << 2,
  kAttrUnderline = 1 << 3,
  kAttrReverse = 1 << 4,
};

// How one cell is rendered. Decoded, not raw escape bytes: a cell reached by
// "dim on, dim off" renders exactly like an untouched one, and comparing raw
// state would call those two different.
//
// Colours are packed rather than kept as strings so a whole row of these costs
// nothing to build on the input thread. The three spellings of a colour must
// not collide -- SGR 31, 38;5;1 and 38;2;.. are different renderings, and
// folding them together would let a style change read as "no change".
struct CellStyle {
  static constexpr uint32_t kDefault = 0;
  static constexpr uint32_t kIndexed = 0x01000000;
  static constexpr uint32_t kTrueColor = 0x02000000;
  static constexpr uint32_t kBasic = 0x03000000;

  uint32_t fg = kDefault;
  uint32_t bg = kDefault;
  uint16_t attrs = 0;

  bool operator==(const CellStyle& o) const { return fg == o.fg && bg == o.bg && attrs == o.attrs; }
  bool operator!=(const CellStyle& o) const { return !(*this == o); }
};

// A `capture-pane -e` row split into the characters it draws and how each one
// is drawn. `styles` has exactly one entry per UTF-8 character of `plain`.
struct StyledRow {
  std::string plain;
  std::vector<CellStyle> styles;
};

// One query's worth of state, parsed out of the single exec's stdout.
struct Snapshot {
  std::string pane_id;
  // `#{pane_current_command}`. Part of the context identity, not of the
  // surrounding text -- it costs nothing here because it rides the same
  // display-message the cursor header already needs.
  std::string pane_command;
  // `#{socket_path}`: the server socket this pane lives on, as an absolute
  // path. Asked for because it is the ONE thing that makes the two identity
  // rungs agree. The pushed rung (tools/rime_ctx_report.sh) can only know
  // ${TMUX%%,*}, an absolute path; the polled rung previously used
  // `copilot/tmux_source/socket`, which is EMPTY on the default socket and a
  // path otherwise -- so the two agreed only by the coincidence that
  // MakeKey's placeholder for empty happens to be the word "default", which
  // is also that socket's basename. Empty here when tmux is too old to know
  // the variable, in which case the caller falls back to the configured one.
  std::string socket_path;
  int cursor_x = 0;  // display column, 0-based
  int cursor_y = 0;  // row within the visible pane, 0-based
  int pane_width = 0;
  std::vector<std::string> rows;           // capture-pane -p -e, escapes stripped
  std::vector<long long> client_activity;  // one per attached client
  // Per-cell rendering of the caret row and the row above it -- the only two
  // rows a cell adjacent to the caret can live in (the row above is where
  // `before` comes from on a wrapped line). Keeping styles for the whole pane
  // would be ~100KB of churn per keystroke for rows nobody looks at.
  std::vector<CellStyle> caret_row_styles;
  std::vector<CellStyle> above_row_styles;
  // tmux's global `focus-events`. Load-bearing whenever more than one client
  // is attached: it is what makes tmux's "current client" track macOS window
  // focus, and it is *off* by default. See JudgeClients.
  bool focus_events = false;
};

struct Context {
  std::string before;
  std::string after;
  int before_depth = 0;
  int after_depth = 0;
  Truncation truncation = Truncation::kUnknown;
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

// The most SGR parameters one `ESC[...m` can carry before we stop reading.
// `38;2;r;g;b;48;2;r;g;b` plus a handful of attributes is already past
// anything real. Dropping the tail of an absurd sequence only ever costs a
// refusal, and the cap is what keeps the parameter buffer off the heap --
// this runs once per escape, per row, per keystroke.
constexpr size_t kMaxSgrParams = 24;

// Apply one `ESC[...m` parameter list to `style`. Params arrive already split
// on ';' so the 38/48 extended forms can consume their own operands.
inline void ApplySgr(const int* params, size_t count, CellStyle* style) {
  for (size_t i = 0; i < count; ++i) {
    const int p = params[i];
    auto colour = [&](uint32_t* slot) {
      // 38/48 ; 5 ; n   (indexed)   or   38/48 ; 2 ; r ; g ; b   (truecolor)
      if (i + 2 < count && params[i + 1] == 5) {
        *slot = CellStyle::kIndexed | static_cast<uint32_t>(params[i + 2] & 0xFF);
        i += 2;
      } else if (i + 4 < count && params[i + 1] == 2) {
        *slot = CellStyle::kTrueColor | (static_cast<uint32_t>(params[i + 2] & 0xFF) << 16) |
                (static_cast<uint32_t>(params[i + 3] & 0xFF) << 8) |
                static_cast<uint32_t>(params[i + 4] & 0xFF);
        i += 4;
      }
    };
    if (p == 0) {
      *style = CellStyle{};
    } else if (p == 1) {
      style->attrs |= kAttrBold;
    } else if (p == 2) {
      style->attrs |= kAttrDim;
    } else if (p == 3) {
      style->attrs |= kAttrItalic;
    } else if (p == 4) {
      style->attrs |= kAttrUnderline;
    } else if (p == 7) {
      style->attrs |= kAttrReverse;
    } else if (p == 22) {
      style->attrs &= static_cast<uint16_t>(~(kAttrBold | kAttrDim));
    } else if (p == 23) {
      style->attrs &= static_cast<uint16_t>(~kAttrItalic);
    } else if (p == 24) {
      style->attrs &= static_cast<uint16_t>(~kAttrUnderline);
    } else if (p == 27) {
      style->attrs &= static_cast<uint16_t>(~kAttrReverse);
    } else if (p == 38) {
      colour(&style->fg);
    } else if (p == 48) {
      colour(&style->bg);
    } else if (p == 39) {
      style->fg = CellStyle::kDefault;
    } else if (p == 49) {
      style->bg = CellStyle::kDefault;
    } else if (p >= 30 && p <= 37) {
      style->fg = CellStyle::kBasic | static_cast<uint32_t>(p - 30);
    } else if (p >= 90 && p <= 97) {
      style->fg = CellStyle::kBasic | static_cast<uint32_t>(p - 90 + 8);
    } else if (p >= 40 && p <= 47) {
      style->bg = CellStyle::kBasic | static_cast<uint32_t>(p - 40);
    } else if (p >= 100 && p <= 107) {
      style->bg = CellStyle::kBasic | static_cast<uint32_t>(p - 100 + 8);
    }
    // Anything else is an attribute we cannot tell apart; leaving the cell
    // unchanged only ever costs a refusal, never a wrong space.
  }
}

// Split a `capture-pane -e` row into its characters and their rendering.
//
// Escapes are removed, so `plain` is byte-identical to what plain
// `capture-pane` would have produced and every slicing rule above is
// unaffected by the switch to -e.
//
// `want_styles` is a real saving, not a micro-optimization: this runs on every
// row of the pane on every keystroke, while only two rows' styles are ever
// read. With it off the escapes are merely skipped -- no parameters decoded,
// no per-character vector -- which is most of the cost of the whole parse.
inline StyledRow SplitStyledRow(const std::string& raw, bool want_styles = true) {
  StyledRow out;
  out.plain.reserve(raw.size());
  if (want_styles) out.styles.reserve(raw.size());
  CellStyle style;
  size_t i = 0;
  while (i < raw.size()) {
    if (raw[i] == '\x1b') {
      if (i + 1 < raw.size() && raw[i + 1] == '[') {
        // CSI: parameter bytes, then one final byte in 0x40..0x7E.
        size_t j = i + 2;
        while (j < raw.size() && (static_cast<unsigned char>(raw[j]) < 0x40 ||
                                  static_cast<unsigned char>(raw[j]) > 0x7E)) {
          ++j;
        }
        if (want_styles && j < raw.size() && raw[j] == 'm') {
          int params[kMaxSgrParams];
          size_t count = 0;
          int value = 0;
          bool any = false;
          for (size_t k = i + 2; k <= j && count < kMaxSgrParams; ++k) {
            if (k < j && raw[k] >= '0' && raw[k] <= '9') {
              value = value * 10 + (raw[k] - '0');
              any = true;
              continue;
            }
            if (k == j || raw[k] == ';') {
              // An empty field is a spelled-out zero: `ESC[m` means `ESC[0m`.
              params[count++] = any ? value : 0;
              value = 0;
              any = false;
            }
          }
          ApplySgr(params, count, &style);
        }
        i = (j < raw.size()) ? j + 1 : raw.size();
        continue;
      }
      if (i + 1 < raw.size() && raw[i + 1] == ']') {
        // OSC (a hyperlink, a title): runs to BEL or ST. Never carries style.
        size_t j = i + 2;
        while (j < raw.size() && raw[j] != '\x07' &&
               !(raw[j] == '\x1b' && j + 1 < raw.size() && raw[j + 1] == '\\')) {
          ++j;
        }
        i = (j < raw.size() && raw[j] == '\x1b') ? j + 2 : j + 1;
        continue;
      }
      i += 2;  // two-byte escape
      continue;
    }
    // A run of ordinary bytes, copied in one go. Appending per character
    // instead cost more than everything else in the parse put together.
    const size_t start = i;
    while (i < raw.size() && raw[i] != '\x1b') ++i;
    out.plain.append(raw, start, i - start);
    if (!want_styles) continue;
    for (size_t k = start; k < i;) {
      const size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(raw[k]));
      if (len == 0 || k + len > i) break;
      out.styles.push_back(style);
      k += len;
    }
  }
  return out;
}

// How the character occupying display column `col` is rendered.
//
// A column inside a wide glyph belongs to that glyph. A column the row never
// reaches -- and a row we hold no style information for at all, which is every
// hand-built snapshot -- decodes as the default, so "no information" compares
// equal to "no information" and nothing starts refusing on its account.
inline CellStyle StyleAtColumn(const std::string& row, const std::vector<CellStyle>& styles,
                               int col) {
  if (col < 0) return CellStyle{};
  int w = 0;
  size_t i = 0;
  size_t index = 0;
  while (i < row.size()) {
    const size_t len = auto_spacer_detail::Utf8SequenceLength(static_cast<unsigned char>(row[i]));
    if (len == 0 || i + len > row.size()) break;
    const int cw = DisplayWidth(auto_spacer_detail::Utf8ToCodepoint(row.substr(i, len)));
    if (col < w + std::max(cw, 1)) {
      return index < styles.size() ? styles[index] : CellStyle{};
    }
    w += cw;
    i += len;
    ++index;
  }
  return CellStyle{};
}

// A foreground that reads as "greyed out" rather than as a colour choice.
//
// Measured, not guessed. Ghost text is drawn muted: codex uses SGR 2 (faint),
// zsh-autosuggestions defaults to `fg=8`, fish to a 256-colour grey. Syntax
// highlighting picks saturated hues instead -- `vim -u NONE -c 'syntax on'` on
// this very header emits 31, 34, 35 and 38;5;130 and nothing muted at all.
// That gap is what lets a de-emphasized run be told apart from a merely
// differently-coloured one.
inline bool IsMutedColor(uint32_t fg) {
  const uint32_t kind = fg & 0xFF000000u;
  const uint32_t value = fg & 0x00FFFFFFu;
  if (kind == CellStyle::kBasic) {
    return value == 8;  // bright black
  }
  if (kind == CellStyle::kIndexed) {
    // 8 is bright black; 232..255 is the xterm greyscale ramp.
    return value == 8 || (value >= 232 && value <= 255);
  }
  if (kind == CellStyle::kTrueColor) {
    const int r = static_cast<int>((value >> 16) & 0xFF);
    const int g = static_cast<int>((value >> 8) & 0xFF);
    const int b = static_cast<int>(value & 0xFF);
    return std::max({r, g, b}) - std::min({r, g, b}) <= 24;  // r ≈ g ≈ b
  }
  return false;  // kDefault
}

// Whether `caret` is drawn *dimmer* than `left`, not merely differently.
//
// The distinction is the whole point: refusing on any change at all would also
// refuse at every syntax-highlighting boundary that happens to fall under the
// caret, and cost a trailing space in an editor for no reason.
inline bool IsDeEmphasized(const CellStyle& caret, const CellStyle& left) {
  if ((caret.attrs & kAttrDim) && !(left.attrs & kAttrDim)) {
    return true;
  }
  return caret.fg != left.fg && IsMutedColor(caret.fg);
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
  // The cursor header is emitted before the pane dump (see BuildTmuxArgs), so
  // the caret's row index is already known while the rows stream past and only
  // the two rows that can neighbour it need their styles kept.
  auto take_row = [&snap](std::string line) {
    const int index = static_cast<int>(snap.rows.size());
    const bool is_caret = index == snap.cursor_y;
    const bool is_above = index == snap.cursor_y - 1;
    // Most rows are neither next to the caret nor styled at all -- blank rows,
    // plain output. Those cost exactly what they cost before -e was asked for.
    if (!is_caret && !is_above && line.find('\x1b') == std::string::npos) {
      snap.rows.push_back(std::move(line));
      return;
    }
    StyledRow styled = SplitStyledRow(line, is_caret || is_above);
    if (is_caret) {
      snap.caret_row_styles = std::move(styled.styles);
    } else if (is_above) {
      snap.above_row_styles = std::move(styled.styles);
    }
    snap.rows.push_back(std::move(styled.plain));
  };
  size_t pos = 0;
  while (pos <= raw.size()) {
    const size_t nl = raw.find('\n', pos);
    if (nl == std::string::npos) {
      if (pos < raw.size() && saw_header) take_row(raw.substr(pos));
      break;
    }
    std::string line = raw.substr(pos, nl - pos);
    pos = nl + 1;

    if (saw_header) {
      take_row(line);
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
    // Its own marker line rather than another CUR| field: a socket path is
    // arbitrary bytes, and appending it to the header would put a second
    // greedy field beside pane_current_command -- one of the two would have
    // to stop being able to hold a '|'. Everything after the marker is the
    // path, so this one can.
    if (line.rfind("SCK|", 0) == 0) {
      snap.socket_path = line.substr(4);
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
      // Everything past the fifth separator is the command, rejoined: it is
      // last in the format exactly so an embedded '|' cannot shift any field.
      // Absent in output from a build that predates this field, which parses
      // as before -- the size check above is still `< 5`.
      for (size_t k = 5; k < f.size(); ++k) {
        if (k > 5) snap.pane_command += "|";
        snap.pane_command += f[k];
      }
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
  // Whether the cell immediately left of the caret exists, and how it is drawn.
  // This is the same cell `before` ends with, so the two must be decided
  // together -- on a wrapped line both come from the row above.
  bool has_left_cell = false;
  CellStyle left_style;
  if (snap.cursor_x == 0) {
    // Column 0 is either a fresh line or the continuation of a wrapped one.
    // Only a row above that fills the pane exactly is a wrap; treating a short
    // row as adjacent would glue two unrelated lines together.
    if (snap.cursor_y > 0) {
      const std::string& prev = snap.rows[snap.cursor_y - 1];
      if (DisplayWidthOf(prev) == snap.pane_width) {
        before = prev;
        has_left_cell = true;
        left_style = StyleAtColumn(prev, snap.above_row_styles, snap.pane_width - 1);
      }
    }
  } else {
    before = SliceBeforeColumn(row, snap.cursor_x);
    has_left_cell = true;
    left_style = StyleAtColumn(row, snap.caret_row_styles, snap.cursor_x - 1);
  }

  // What is drawn right of the caret is only the user's text if it continues
  // the run the caret sits at the end of. A TUI's own ghost text -- codex's
  // dim placeholder, a shell's inline autosuggestion, an inline completion --
  // starts a NEW, *muted* run at exactly the caret, and that is the one signal
  // a screen scrape gets that those characters are not in the buffer at all.
  // Without it `after` was the placeholder's first letter, and AutoSpacer put
  // a space after every first CJK commit into an empty codex composer.
  //
  // Only de-emphasis refuses, not any difference: an editor colours its buffer
  // by syntax, and a hue boundary landing under the caret says nothing about
  // whose text it is. See IsDeEmphasized for the measurements behind that.
  const CellStyle caret_style = StyleAtColumn(row, snap.caret_row_styles, snap.cursor_x);
  const bool after_continues_before = has_left_cell && !IsDeEmphasized(caret_style, left_style);

  Context ctx;
  // Measured BEFORE the tail is taken: whether the config cut anything is a
  // statement about what the row held, not about what we kept.
  const int available = ::copilot::CharCount(before);
  ctx.before = NormalizeBlanks(TailChars(before, prefix_chars));
  ctx.before_depth = ::copilot::CharCount(ctx.before);
  // tmux never reports kFull in this step: it has no idea where the user's own
  // text starts, so the only thing that ever ends `before` other than the
  // budget is the beginning of the row -- a screen limit, not the end of an
  // input region. Step (c)'s style test is what makes kFull reachable here.
  ctx.truncation = (available > prefix_chars) ? Truncation::kByConfig : Truncation::kByScreen;
  ctx.after = after_continues_before ? NormalizeBlanks(auto_spacer_detail::GetFirstUtf8Char(
                                           SliceAfterColumn(row, snap.cursor_x)))
                                     : "";
  ctx.after_depth = ::copilot::CharCount(ctx.after);
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
  // Rides this same exec for the same reason focus-events does: a spawn of
  // its own would cost more than everything tmux does here put together
  // (measurement 3 in the context-ascii-memory design -- 2.75 ms of the
  // 3.37 ms is starting the process). An older tmux that does not know
  // `socket_path` expands it to nothing, which reads back as "unknown" and
  // falls back to the configured socket, i.e. to the previous behaviour.
  args.push_back("display-message");
  args.push_back("-p");
  args.push_back("-F");
  args.push_back("SCK|#{socket_path}");
  args.push_back(";");
  args.push_back("display-message");
  args.push_back("-p");
  args.push_back("-F");
  args.push_back("CUR|#{pane_id}|#{cursor_x}|#{cursor_y}|#{pane_width}|#{pane_current_command}");
  args.push_back(";");
  args.push_back("capture-pane");
  args.push_back("-p");
  // -e keeps the SGR sequences. Without them the dump is characters only, and
  // an app's ghost text reads exactly like the user's own buffer -- see the
  // rendering-continuity check in ExtractContext.
  args.push_back("-e");
  return args;
}

// The pane id MUST be in the key: AutoSpacer indexes per-client state by it
// (src/auto_spacer.cc:282-286), so a constant key would let one pane's
// spacing state bleed into the next.
//
// DELIBERATELY has no host segment, unlike context_memory::MakeKey since
// 2026-08-31. This keys AutoSpacer's client_states_ boundary cache, which
// caches text scraped from the LOCAL pane. An ssh pane's screen shows the
// remote's output, but the scrape, the caret and the boundary are all local,
// so the local pane is the correct key here and a host segment would be
// wrong rather than merely redundant. Note the "byte-identical" claim below
// was always conditional anyway -- MakeKey appends "|command" under
// use_pane_command and this never does.
//
// Byte-identical to context_memory::MakeKey (src/context_memory.h) for the
// same socket and pane, and duplicated on purpose: this header includes
// history.h, which drags in glog, and context_memory.h must stay Rime-free so
// the pure test can drive it without an engine. Keep them in step by hand;
// see the fuller note there, including what would let the two collapse into
// one (history.h here should be utf8_index.h -- it merely re-exports
// copilot::UTF8/CharCount).
inline std::string MakeClientKey(const std::string& socket, const std::string& pane_id) {
  const std::string socket_tag = socket.empty() ? "default" : socket;
  return "tmux:" + socket_tag + ":" + pane_id;
}

}  // namespace tmux_detail
}  // namespace rime
