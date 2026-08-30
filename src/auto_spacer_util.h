#pragma once

// Pure, dependency-free helpers for AutoSpacer's spacing decisions.
//
// These functions were extracted verbatim from auto_spacer.cc so they can be
// unit-tested in isolation. They operate only on std::string / int and do not
// depend on any Rime type. Behavior must stay identical to the originals.

#include <cctype>
#include <cstdint>
#include <ostream>
#include <string>

namespace rime {
namespace auto_spacer_detail {

// 将 UTF-8 字符串转为 Unicode 码点
inline uint32_t Utf8ToCodepoint(const std::string& s) {
  uint32_t code = 0;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(s.data());
  size_t len = s.size();

  if (len == 1) {
    code = bytes[0];
  } else if (len == 2) {
    code = ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
  } else if (len == 3) {
    code = ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
  } else if (len == 4) {
    code = ((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | ((bytes[2] & 0x3F) << 6) |
           (bytes[3] & 0x3F);
  }
  return code;
}

// UTF-8 首字节声明的序列长度; 0 表示不是合法首字节.
inline size_t Utf8SequenceLength(unsigned char lead) {
  if ((lead & 0x80) == 0x00) return 1;
  if ((lead & 0xE0) == 0xC0) return 2;
  if ((lead & 0xF0) == 0xE0) return 3;
  if ((lead & 0xF8) == 0xF0) return 4;
  return 0;  // 续字节 (10xxxxxx) 或非法首字节
}

// s 是否恰好是一个结构合法的 UTF-8 字符.
inline bool IsSingleUtf8Char(const std::string& s) {
  if (s.empty()) return false;
  size_t len = Utf8SequenceLength(static_cast<unsigned char>(s[0]));
  if (len == 0 || len != s.size()) return false;
  for (size_t i = 1; i < len; ++i) {
    if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) return false;
  }
  return true;
}

// 判断是否是中文标点符号
inline bool IsChinesePunctuation(const std::string& s) {
  // 必须是单个合法 UTF-8 字符: 否则 "3@x" 这样的短 ASCII 串会被当成 3 字节
  // 序列解码成 U+3020, 落进 CJK 标点区间, 导致上屏时跳过自动加空格.
  if (!IsSingleUtf8Char(s)) return false;

  uint32_t cp = Utf8ToCodepoint(s);
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK 符号和标点
         (cp >= 0xFF00 && cp <= 0xFFEF);    // 全角标点等
}

inline int LastAsciiCharCode(const std::string& str) {
  if (str.empty()) return -1;

  int i = static_cast<int>(str.size()) - 1;
  // 回溯查找 UTF-8 字符的起始字节
  while (i >= 0 && (static_cast<uint8_t>(str[i]) & 0xC0) == 0x80) {
    --i;
  }
  if (i < 0) return -1;  // 非法 UTF-8 序列

  uint8_t c = static_cast<uint8_t>(str[i]);
  if (c < 0x80) {
    return c;  // 是 ASCII 字符，直接返回其数值
  }

  return -1;  // 非 ASCII 字符
}

// Helper: Get last UTF-8 character from string
inline std::string GetLastUtf8Char(const std::string& str) {
  if (str.empty()) return "";

  size_t len = str.size();
  size_t start = len - 1;

  // Find start of last UTF-8 character
  while (start > 0 && (static_cast<uint8_t>(str[start]) & 0xC0) == 0x80) {
    start--;
  }

  return str.substr(start);
}

inline std::string GetFirstUtf8Char(const std::string& str) {
  if (str.empty()) return "";
  size_t len = str.size();
  size_t end = 1;
  unsigned char c = static_cast<unsigned char>(str[0]);
  if ((c & 0x80) == 0x00) {
    end = 1;
  } else if ((c & 0xE0) == 0xC0) {
    end = 2;
  } else if ((c & 0xF0) == 0xE0) {
    end = 3;
  } else if ((c & 0xF8) == 0xF0) {
    end = 4;
  }
  if (end > len) {
    end = len;
  }
  return str.substr(0, end);
}

inline bool IsAsciiRightPunctCode(int c) {
  return c == '.' || c == ',' || c == '>' || c == ']' || c == ')' || c == '}' || c == '!' ||
         c == '?';
}

inline bool IsAsciiRightPunctCodeForAsciiInput(int c) {
  // Keep punctuation-triggered spacing, but exclude '.' per latest behavior.
  return c == ',' || c == '>' || c == ']' || c == ')' || c == '}' || c == '!' || c == '?';
}

inline bool IsAsciiAlphaNumCode(int c) {
  return c >= 0 && c < 0x80 && std::isalnum(static_cast<unsigned char>(c));
}

inline bool IsAsciiPunctuationCode(int c) {
  return c >= 0 && c < 0x80 && std::ispunct(static_cast<unsigned char>(c));
}

inline bool IsChinesePunctuationChar(const std::string& s) {
  return !s.empty() && IsChinesePunctuation(s);
}

inline bool IsCjkNonPunctuationChar(const std::string& s) {
  if (s.empty() || IsChinesePunctuationChar(s)) {
    return false;
  }
  return LastAsciiCharCode(s) < 0;
}

inline bool IsPureAsciiText(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (unsigned char c : s) {
    if (c >= 0x80) {
      return false;
    }
  }
  return true;
}

// Any ASCII whitespace is a spacing boundary, not just U+0020.
//
// `before` grows real newlines once a source can walk past the caret's line
// (step (c) of the 2026-08-23 surrounding-context design). Until then this is
// unreachable, which is exactly why it is written now: a predicate that answers
// a new input correctly by falling through to IsAsciiRightPunctCode('\n') is
// correct by accident, and this tree has been bitten by that shape before.
inline bool IsAsciiSpaceChar(const std::string& ch) {
  return ch.size() == 1 && std::isspace(static_cast<unsigned char>(ch[0])) != 0;
}

inline bool NeedSpaceBefore(const std::string& before, bool content_is_ascii) {
  std::string ch = GetLastUtf8Char(before);
  if (ch.empty() || IsChinesePunctuationChar(ch) || IsAsciiSpaceChar(ch)) {
    return false;
  }
  int ascii = LastAsciiCharCode(ch);
  if (content_is_ascii) {
    return IsCjkNonPunctuationChar(ch) || IsAsciiRightPunctCodeForAsciiInput(ascii);
  }
  return IsAsciiAlphaNumCode(ascii) || IsAsciiRightPunctCode(ascii);
}

inline bool NeedSpaceAfter(const std::string& after, bool content_is_ascii) {
  std::string ch = GetFirstUtf8Char(after);
  if (ch.empty() || IsChinesePunctuationChar(ch) || IsAsciiSpaceChar(ch)) {
    return false;
  }
  int ascii = LastAsciiCharCode(ch);
  if (content_is_ascii) {
    return IsCjkNonPunctuationChar(ch);
  }
  return IsAsciiAlphaNumCode(ascii);
}

inline std::string DecorateCommitText(const std::string& text, const std::string& before,
                                      const std::string& after, bool content_is_ascii,
                                      bool enable_space_after) {
  if (text.empty()) {
    return text;
  }
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  std::string result = text.substr(begin, end - begin);
  if (result.empty() || IsChinesePunctuationChar(result)) {
    return result;
  }

  if (NeedSpaceBefore(before, content_is_ascii) && (result.empty() || result.front() != ' ')) {
    result = " " + result;
  }
  if (enable_space_after && NeedSpaceAfter(after, content_is_ascii) &&
      (result.empty() || result.back() != ' ')) {
    result += " ";
  }
  return result;
}

}  // namespace auto_spacer_detail

// The history path's spacing decision, lifted verbatim out of
// ProcessWithCommitHistory (auto_spacer.cc) so it can be driven without a Rime
// engine.
//
// It now applies the same predicates as the surrounding path
// (`NeedSpaceBefore`), so there is one set of spacing rules rather than two.
// What that substitution cost is recorded, row by row, in the two tables of
// test/history_spacing_table_test.cc.
enum class HistorySpaceAction { kNone, kPrependSpaceToInput, kCommitWithSpace };

// Without this gtest prints an enum class as its raw bytes
// ("4-byte object <01-00 00-00>"), which is unreadable exactly when it matters:
// a table row that moves is reported as old and new *values*.
inline void PrintTo(HistorySpaceAction action, std::ostream* os) {
  switch (action) {
    case HistorySpaceAction::kNone:
      *os << "kNone";
      return;
    case HistorySpaceAction::kPrependSpaceToInput:
      *os << "kPrependSpaceToInput";
      return;
    case HistorySpaceAction::kCommitWithSpace:
      *os << "kCommitWithSpace";
      return;
  }
  *os << "HistorySpaceAction(" << static_cast<int>(action) << ")";
}

struct HistorySpaceInput {
  std::string before;  // commit_history().latest_text()
  bool ascii_mode = false;
  bool has_input = false;  // !ctx->input().empty()
};

inline HistorySpaceAction DecideHistorySpacing(const HistorySpaceInput& in) {
  if (in.has_input || in.before == " ") {
    return HistorySpaceAction::kNone;
  }
  // The same predicates the surrounding path applies to real text. They
  // additionally handle right-hand punctuation and exclude Chinese
  // punctuation, which the hand-rolled tests they replace did not.
  if (!in.ascii_mode &&
      auto_spacer_detail::NeedSpaceBefore(in.before, /*content_is_ascii=*/false)) {
    return HistorySpaceAction::kPrependSpaceToInput;
  }
  if (in.ascii_mode && auto_spacer_detail::NeedSpaceBefore(in.before, /*content_is_ascii=*/true)) {
    return HistorySpaceAction::kCommitWithSpace;
  }
  return HistorySpaceAction::kNone;
}

}  // namespace rime
