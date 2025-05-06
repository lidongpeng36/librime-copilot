#pragma once

#include <deque>
#include <string>

namespace copilot {

class History {
 public:
  explicit History(size_t size);
  void add(const std::string&);
  void pop();
  void clear() {
    input_.clear();
    pos_.clear();
  }

  size_t size() const { return pos_.size(); }
  bool empty() const { return pos_.empty(); }
  std::string back() const;
  std::string gets(size_t n) const;
  std::string get_chars(size_t n) const;

  struct Pos {
    size_t total;
    std::vector<size_t> pos;
    size_t sum() {
      size_t s = 0;
      for (size_t p : pos) {
        s += p;
      }
      return s;
    }
    int pop_back() {
      if (pos.empty()) {
        return 0;
      }
      size_t p = pos.back();
      pos.resize(pos.size() - 1);
      total -= p;
      return pos.empty() ? -p : p;
    }
  };

 private:
  std::string debug_string() const;
  void cleanup();
  size_t size_;
  size_t capacity_;
  std::string input_;
  std::deque<Pos> pos_;
};

}  // namespace copilot
