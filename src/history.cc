#include "history.h"

#include <algorithm>
#include <cassert>
#include <string>
#include <unordered_set>

#include <glog/logging.h>

namespace {
// 合法拼音音节集合（标准普通话合法音节）
// clang-format off
const std::unordered_set<std::string> valid_pinyin_syllables = {
  /* a */
    "a", "ai", "an", "ang", "ao",
  // b
    "ba", "bai", "ban", "bang", "bao", "bei", "ben", "beng", "bi", "bian", "biao", "bie", "bin", "bing", "bo", "bu",
  // c
    "ca", "cai", "can", "cang", "cao", "ce", "cen", "ceng",
  // ch
    "cha", "chai", "chan", "chang", "chao", "che", "chen", "cheng", "chi", "chong", "chou", "chu", "chuai", "chuan", "chuang", "chui", "chun", "chuo",
  // d
    "da", "dai", "dan", "dang", "dao", "de", "dei", "deng", "di", "dia", "dian", "diao", "die", "ding", "diu", "dong", "dou", "du", "duan", "dui", "dun", "duo",
  // e
    "e", "ei", "en", "eng", "er",
  // f
    "fa", "fan", "fang", "fei", "fen", "feng", "fo", "fou", "fu",
  // g
    "ga", "gai", "gan", "gang", "gao", "ge", "gei", "gen", "geng", "gong", "gou", "gu", "gua", "guai", "guan", "guang", "gui", "gun", "guo",
  // h
    "ha", "hai", "han", "hang", "hao", "he", "hei", "hen", "heng", "hong", "hou", "hu", "hua", "huai", "huan", "huang", "hui", "hun", "huo",
  // j
    "ji", "jia", "jian", "jiang", "jiao", "jie", "jin", "jing", "jiong", "jiu", "ju", "juan", "jue", "jun",
  // k
    "ka", "kai", "kan", "kang", "kao", "ke", "kei", "ken", "keng", "kong", "kou", "ku", "kua", "kuai", "kuan", "kuang", "kui", "kun", "kuo",
  // l
    "la", "lai", "lan", "lang", "lao", "le", "lei", "leng", "li", "lia", "lian", "liang", "liao", "lie", "lin", "ling", "liu", "long", "lou", "lu", "luan", "lue", "lun", "luo",
  // m
    "ma", "mai", "man", "mang", "mao", "me", "mei", "men", "meng", "mi", "mian", "miao", "mie", "min", "ming", "miu", "mo", "mou", "mu",
  // n
    "na", "nai", "nan", "nang", "nao", "ne", "nei", "nen", "neng", "ni", "nian", "niang", "niao", "nie", "nin", "ning", "niu", "nong", "nou", "nu", "nuan", "nue", "nuo",
  // o
    "o", "ou",
  // p
    "pa", "pai", "pan", "pang", "pao", "pei", "pen", "peng", "pi", "pian", "piao", "pie", "pin", "ping", "po", "pou", "pu",
  // q
    "qi", "qia", "qian", "qiang", "qiao", "qie", "qin", "qing", "qiong", "qiu", "qu", "quan", "que", "qun",
  // r
    "ran", "rang", "rao", "re", "ren", "reng", "ri", "rong", "rou", "ru", "rua", "ruan", "rui", "run", "ruo",
  // s
    "sa", "sai", "san", "sang", "sao", "se", "sen", "seng",
    "si", "song", "sou", "su", "suan", "sui", "sun", "suo",
  // sh
    "sha", "shai", "shan", "shang", "shao", "she", "shen", "sheng", "shi", "shou", "shu", "shua", "shuai", "shuan", "shuang", "shui", "shun", "shuo",
  // t
    "ta", "tai", "tan", "tang", "tao", "te", "teng", "ti", "tian", "tiao", "tie", "ting", "tong", "tou", "tu", "tuan", "tui", "tun", "tuo",
  // w
    "wa", "wai", "wan", "wang", "wei", "wen", "weng", "wo", "wu",
  // x
    "xi", "xia", "xian", "xiang", "xiao", "xie", "xin", "xing", "xiong", "xiu", "xu", "xuan", "xue", "xun",
  // y
    "ya", "yan", "yang", "yao", "ye", "yi", "yin", "ying", "yo", "yong", "you", "yu", "yuan", "yue", "yun",
  // z
    "za", "zai", "zan", "zang", "zao", "ze", "zei", "zen", "zeng",
    "zi", "zong", "zou", "zu", "zuan", "zui", "zun", "zuo",
  // zh
    "zha", "zhai", "zhan", "zhang", "zhao", "zhe", "zhen", "zheng", "zhi", "zhong", "zhou", "zhu", "zhua", "zhuai", "zhuan", "zhuang", "zhui", "zhun", "zhuo",
};
// clang-format on

// 查询函数：判断是不是合法音节
bool IsValidSyllable(const std::string& syllable) {
  return valid_pinyin_syllables.find(syllable) != valid_pinyin_syllables.end();
}
}  // namespace

namespace {

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
}  // namespace

namespace copilot {
UTF8::UTF8(const std::string& data) {
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

size_t UTF8::size() const { return pos_.size() - 1; }

std::string_view UTF8::operator[](int i) const {
  int n = size();
  if (i < 0) i += n;
  if (i < 0 || i >= n) return {};

  return std::string_view(data_.data() + pos_[i], pos_[i + 1] - pos_[i]);
}

std::string_view UTF8::operator()(int start, int end) const {
  int n = size();

  if (start < 0) start += n;
  if (end < 0) end += n;

  // Clamp to [0, n - 1]（闭区间索引）
  start = std::clamp(start, 0, n - 1);
  end = std::clamp(end, 0, n - 1);

  if (start > end) return {};

  return std::string_view(data_.data() + pos_[start], pos_[end + 1] - pos_[start]);
}
// clang-format off
static const std::vector<std::string_view> chinese_punct = {
    "，", "。", "！", "？", "；", "：", "（", "）",
    "【", "】", "《", "》", "、", "——", "……", "“", "”", "‘", "’"
};
// clang-format on

std::string_view UTF8::left() const {
  int n = size();
  for (int i = 0; i < n; ++i) {
    std::string_view ch = (*this)[i];

    // 英文/ASCII 标点（仅单字节）
    if (ch.size() == 1 && std::ispunct(static_cast<unsigned char>(ch[0]))) {
      return (*this)(0, i - 1);
    }

    // 中文/全角标点
    if (std::find(chinese_punct.begin(), chinese_punct.end(), ch) != chinese_punct.end()) {
      return (*this)(0, i - 1);
    }
  }

  // 没有遇到标点，返u整段
  return (*this)(0, -2);
}

std::string_view UTF8::right() const {
  int n = size();
  for (int i = 0; i < n; ++i) {
    std::string_view ch = (*this)[i];

    // ASCII 英文标点
    if (ch.size() == 1 && std::ispunct(static_cast<unsigned char>(ch[0]))) {
      return (*this)(i + 1, -1);
    }

    // 中文/全角标点
    if (std::find(chinese_punct.begin(), chinese_punct.end(), ch) != chinese_punct.end()) {
      return (*this)(i + 1, -1);
    }
  }

  // 未找到标点，默认从第1位开始
  return (*this)(1, -1);
}

}  // namespace copilot

namespace copilot {

std::ostream& operator<<(std::ostream& os, const History::Pos& pos) {
  os << "[|" << pos.total << "|";
  for (size_t i = 0; i < pos.pos.size(); ++i) {
    os << pos.pos[i];
    if (i != pos.pos.size() - 1) {
      os << ", ";
    }
  }
  os << "]";
  return os;
}

std::string History::debug_string() const {
  std::stringstream ss;
  ss << "[History] '" << input_ << "', #pos_:" << pos_.size() << ", { ";
  for (size_t i = 0; i < pos_.size(); ++i) {
    ss << pos_[i];
    if (i != pos_.size() - 1) {
      ss << ", ";
    }
  }
  ss << " }";
  return ss.str();
}

History::History(size_t n) : size_(n), capacity_(n * 2) {}

void History::cleanup() {
  DLOG(INFO) << "History::cleanup: " << debug_string();
  size_t n = 0;
  for (size_t i = 0; i < size_; ++i) {
    n += pos_[i].total;
    pos_.pop_front();
  }
  input_.erase(0, n);
}

void History::add(const std::string& input) {
  size_t n = input.size();
  input_.append(input);
  pos_.push_back({n, SplitU8(input)});
  DLOG(INFO) << "History::add: " << debug_string();
  assert(pos_.back().sum() == n);
  if (pos_.size() >= capacity_) {
    cleanup();
  }
}

void History::pop() {
  if (pos_.empty()) {
    return;
  }
  DLOG(INFO) << "* Before History::pop: " << debug_string();
  while (!pos_.empty()) {
    auto& last = pos_.back();
    auto n = last.pop_back();
    if (n < 0) {
      input_.erase(input_.end() + n, input_.end());
      pos_.pop_back();
      break;
    }
    if (n > 0) {
      input_.erase(input_.end() - n, input_.end());
      break;
    }
    pos_.pop_back();
  }
  DLOG(INFO) << "* After History::pop: " << debug_string();
}

std::string History::back() const {
  if (pos_.empty()) {
    return "";
  }
  if (pos_.back().pos.empty()) {
    return "";
  }
  int n = pos_.back().pos.back();
  return input_.substr(input_.size() - n);
}

std::string History::gets(size_t n) const {
  size_t skip = (n >= pos_.size()) ? 0 : pos_.size() - n;
  size_t pos = 0;
  for (size_t i = skip; i < pos_.size(); ++i) {
    pos += pos_[i].total;
  }
  return input_.substr(input_.size() - pos);
}

std::string History::get_chars(size_t n) const {
  if (pos_.empty()) {
    return "";
  }
  int pos = 0;
  for (int j = pos_.size() - 1; j >= 0; --j) {
    auto& p = pos_[j].pos;
    if (p.empty()) {
      continue;
    }
    if (p.size() <= n) {
      pos += pos_[j].total;
      n -= p.size();
      if (n == 0) {
        break;
      }
      continue;
    }
    for (size_t i = p.size() - n; i < p.size(); ++i) {
      pos += p[i];
    }
    break;
  }
  return input_.substr(input_.size() - pos);
}

std::string_view History::last() const {
  if (pos_.empty()) {
    return std::string_view();
  }
  const auto& pos = pos_.back();
  return std::string_view(input_).substr(input_.size() - pos.total);
}

}  // namespace copilot
