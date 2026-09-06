#pragma once

// UTF-8 codepoint indexing: SplitU8, the UTF8 class and CharCount, extracted
// out of history.{h,cc} so a translation unit that must not link glog (or
// anything else this plugin needs) can still count and slice UTF-8 text.
//
// This split exists for exactly one consumer today: scoring_form.h includes
// this header instead of history.h, because tools/score_candidates.cc links
// llama.cpp and nlohmann_json ONLY -- see scoring_form.h's own header comment
// for why that constraint exists and what it would cost to compile the whole
// plugin (or link glog) just to reach a UTF-8 index. history.cc still uses
// SplitU8 for CommitHistory and gets it from here too, so there is exactly
// one implementation.
//
// Everything below is `inline` on purpose: this header must never need a
// corresponding .cc, or the point of extracting it is lost the next time
// someone links it into a third llama-only tool.

#include <algorithm>
#include <cctype>
#include <string>
#include <string_view>
#include <vector>

namespace copilot {

// Byte length of each UTF-8 character in `input`, in order. A malformed lead
// byte is treated as a single byte and skipped, rather than discarding
// everything after it -- callers rely on this to stay in sync with whatever
// text follows a stray byte (see scoring_form.h's AlignToTrainingForm, which
// documents the same choice for its own pass over possibly-malformed input).
inline std::vector<size_t> SplitU8(const std::string& input) {
  std::vector<size_t> result;
  size_t i = 0;
  const size_t n = input.size();

  while (i < n) {
    unsigned char c = static_cast<unsigned char>(input[i]);
    size_t char_len = 1;

    if ((c & 0x80) == 0x00) {  // 0xxxxxxx, ASCII
      char_len = 1;
    } else if ((c & 0xE0) == 0xC0) {  // 110xxxxx, 2 bytes
      char_len = 2;
    } else if ((c & 0xF0) == 0xE0) {  // 1110xxxx, 3 bytes
      char_len = 3;
    } else if ((c & 0xF8) == 0xF0) {  // 11110xxx, 4 bytes
      char_len = 4;
    } else {
      // 遇到非法 utf8 字节，直接跳过1字节
      char_len = 1;
    }

    if (i + char_len <= n) {
      result.emplace_back(char_len);
    } else {
      result.emplace_back(n - i);
      break;
    }
    i += char_len;
  }

  return result;
}

// clang-format off
inline const std::vector<std::string_view> kUtf8IndexChinesePunct = {
    "，", "。", "！", "？", "；", "：", "（", "）",
    "【", "】", "《", "》", "、", "——", "……", "“", "”", "‘", "’"
};
// clang-format on

class UTF8 {
 public:
  // `data_` is a string_view over the ARGUMENT -- this class never copies the
  // text -- so the caller must keep it alive for the lifetime of the UTF8.
  // That contract is unenforceable against a temporary, and `UTF8(a + b)` or
  // `UTF8(f())` compiled fine and read freed memory. Deleting the rvalue
  // overload makes it a compile error instead of a runtime one; every call
  // site in the tree already passes an lvalue.
  explicit UTF8(std::string&&) = delete;
  explicit UTF8(const std::string& data) {
    data_ = data;
    auto lens = SplitU8(data);  // 每个字符的长度
    pos_.reserve(lens.size() + 1);

    size_t offset = 0;
    pos_.push_back(offset);  // 第0个字符起始位置是0
    for (size_t len : lens) {
      offset += len;
      pos_.push_back(offset);  // 第i+1个字符的起始位置
    }
  }

  size_t size() const { return pos_.size() - 1; }

  std::string_view operator[](int i) const {
    int n = size();
    if (i < 0) i += n;
    if (i < 0 || i >= n) return {};

    return std::string_view(data_.data() + pos_[i], pos_[i + 1] - pos_[i]);
  }

  std::string_view operator()(int start, int end) const {
    int n = size();
    // std::clamp(v, 0, n - 1) below is UNDEFINED when n == 0: lo > hi. It does
    // not merely return something odd -- measured, it throws out of the
    // string_view construction, and SelectCharacter reaches here with whatever
    // GetSelectedCandidate() returned. Answer "nothing" instead.
    if (n == 0) return {};

    if (start < 0) start += n;
    if (end < 0) end += n;

    // Clamp to [0, n - 1]（闭区间索引）
    start = std::clamp(start, 0, n - 1);
    end = std::clamp(end, 0, n - 1);

    if (start > end) return {};

    return std::string_view(data_.data() + pos_[start], pos_[end + 1] - pos_[start]);
  }

  std::string_view left() const {
    int n = size();
    for (int i = 0; i < n; ++i) {
      std::string_view ch = (*this)[i];
      // 英文/ASCII 标点（仅单字节）或 中文/全角标点
      const bool is_punct = (ch.size() == 1 && std::ispunct(static_cast<unsigned char>(ch[0]))) ||
                            std::find(kUtf8IndexChinesePunct.begin(), kUtf8IndexChinesePunct.end(),
                                      ch) != kUtf8IndexChinesePunct.end();
      if (is_punct) {
        // 标点在首位时前面什么都没有: (0, -1) 会被负索引解释成"整段",
        // 反而把标点本身也带出来.
        if (i == 0) return {};
        return (*this)(0, i - 1);
      }
    }

    // 没有遇到标点: 除最后一字之外的全部。一个字符时那是空的 -- (0, -2)
    // 解析成空区间, 而 clamp 会把它救成 (0, 0), 即整串。
    if (n < 2) return {};
    return (*this)(0, -2);
  }

  std::string_view right() const {
    int n = size();
    for (int i = 0; i < n; ++i) {
      std::string_view ch = (*this)[i];
      // ASCII 英文标点 或 中文/全角标点
      const bool is_punct = (ch.size() == 1 && std::ispunct(static_cast<unsigned char>(ch[0]))) ||
                            std::find(kUtf8IndexChinesePunct.begin(), kUtf8IndexChinesePunct.end(),
                                      ch) != kUtf8IndexChinesePunct.end();
      if (is_punct) {
        // 标点在末位时后面什么都没有: (n, -1) 的 start 会被 clamp 回 n-1,
        // 于是返回标点自己.
        if (i + 1 >= n) return {};
        return (*this)(i + 1, -1);
      }
    }

    // 未找到标点: 从第 1 位开始。一个字符时那是空的 -- 同 left() 的理由,
    // (1, -1) 解析成空区间而 clamp 把它救成整串。
    if (n < 2) return {};
    return (*this)(1, -1);
  }

 private:
  std::string_view data_;
  std::vector<size_t> pos_;
};

// Characters, not bytes and not UTF-16 units. The one answer to "how long is
// this string" for every consumer that reports a depth.
inline int CharCount(const std::string& s) {
  return s.empty() ? 0 : static_cast<int>(UTF8(s).size());
}

}  // namespace copilot
