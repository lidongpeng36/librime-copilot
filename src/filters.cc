#include <rime/context.h>
#include <rime/engine.h>
#include <rime/filter.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include "rime/schema.h"

#include "filters.h"

// AutoSpacerFilterTranslation
namespace rime {

namespace {
inline bool IsAsciiFirstChar(const std::string& str) {
  if (str.empty()) return false;
  unsigned char c = static_cast<unsigned char>(str[0]);
  return (c & 0x80) == 0;  // 0xxxxxxx -> ASCII 字符
}
inline bool IsAsciiLastChar(const std::string& str) {
  if (str.empty()) return false;

  // 从最后一个字节开始向前查找 UTF-8 字符的起始字节
  int i = static_cast<int>(str.size()) - 1;
  // 找到首字节标志 (最高位不为 10 的字节)
  while (i >= 0 && (static_cast<unsigned char>(str[i]) & 0xC0) == 0x80) {
    --i;
  }
  if (i < 0) return false;  // 非法 UTF-8 序列

  unsigned char c = static_cast<unsigned char>(str[i]);
  return (c & 0x80) == 0;  // 0xxxxxxx -> ASCII
}
}  // namespace

class AutoSpacerFilterTranslation : public PrefetchTranslation {
 protected:
  bool Replenish() override;

 private:
  friend struct TranslationCreator<AutoSpacerFilterTranslation>;
  AutoSpacerFilterTranslation(an<Translation> translation, const std::string& last);
  bool is_en_;
};

template <>
struct TranslationCreator<AutoSpacerFilterTranslation> {
  an<Translation> operator()(const an<Translation>& translation, const Engine* engine);
};

an<Translation> TranslationCreator<AutoSpacerFilterTranslation>::operator()(
    const an<Translation>& translation, const Engine* engine) {
  const auto* ctx = engine->context();
  if (ctx->commit_history().empty()) {
    return translation;
  }
  const auto& last = ctx->commit_history().back().text;
  const auto& input = ctx->input();
  DLOG(INFO) << "last_commit: '" << last << "'" << ", input:'" << ctx->input() << "'";

  if (last.empty() || std::isspace(static_cast<unsigned char>(last.back()))) {
    return translation;
  }
  if (!input.empty() && std::isspace(static_cast<unsigned char>(input[0]))) {
    DLOG(INFO) << "[Filter] input has space. skip";
    return translation;
  }
  DLOG(INFO) << "[Filter] insert space for cands...";
  return std::shared_ptr<AutoSpacerFilterTranslation>(
      new AutoSpacerFilterTranslation(translation, last));
}

AutoSpacerFilterTranslation::AutoSpacerFilterTranslation(an<Translation> translation,
                                                         const std::string& last)
    : PrefetchTranslation(translation), is_en_(IsAsciiLastChar(last)) {}

bool AutoSpacerFilterTranslation::Replenish() {
  auto next = translation_->Peek();
  translation_->Next();
  if (next) {
    cache_.push_back(is_en_ != IsAsciiFirstChar(next->text())
                         ? New<ShadowCandidate>(next, "autospacer", " " + next->text())
                         : next);
  }
  return !cache_.empty();
}

}  // namespace rime

// RawInputFilterTranslation
namespace rime {
class RawInputFilterTranslation : public PrefetchTranslation {
 protected:
  bool Replenish() override;

 private:
  friend struct TranslationCreator<RawInputFilterTranslation>;
  RawInputFilterTranslation(an<Translation> translation, const std::string& input,
                            int page_size = 0);

  std::string input_;
  bool inserted_ = false;
  int page_size_ = 0;
};

RawInputFilterTranslation::RawInputFilterTranslation(an<Translation> translation,
                                                     const std::string& input, int page_size)
    : PrefetchTranslation(translation), input_(input), page_size_(page_size) {
  DLOG(INFO) << "[RawInputFilter] input: '" << input << "' page_size: " << page_size;
}

bool RawInputFilterTranslation::Replenish() {
  auto next = translation_->Peek();
  translation_->Next();
  if (!next) {
    return !cache_.empty();
  }
  if (inserted_) {
    cache_.push_back(next);
    return !cache_.empty();
  }
  if (next->start() > 0) {
    inserted_ = true;
    cache_.push_back(next);
    return !cache_.empty();
  }

  inserted_ = true;
  auto raw = New<SimpleCandidate>("raw", 0, input_.size(), input_);
  if (next->type() == "sentence") {
    cache_.push_back(raw);
    cache_.push_back(next);
    return true;
  }
  size_t n = (input_.size() + 1) / 2;
  for (int i = 0; i < page_size_ - 1; ++i) {
    DLOG(INFO) << "[CAND] " << i << ": '" << next->text() << "'|" << next->type() << "|"
               << next->start() << "|" << next->end() << "|" << next->quality();
    if (next->text() == input_) {
      cache_.push_back(next);
      return true;
    }
    if (next->end() < input_.size()) {
      cache_.push_back(raw);
      cache_.push_back(next);
      return true;
    }
    cache_.push_back(next);
    next = translation_->Peek();
    translation_->Next();
    if (!next) {
      break;
    }
  }
  cache_.push_back(raw);
  return true;
}

template <>
struct TranslationCreator<RawInputFilterTranslation> {
  an<Translation> operator()(const an<Translation>& translation, const Engine* engine);
};

an<Translation> TranslationCreator<RawInputFilterTranslation>::operator()(
    const an<Translation>& translation, const Engine* engine) {
  auto ctx = engine->context();
  const auto& input = ctx->input();
  if (input.empty()) {
    return translation;
  }
  auto page_size = engine->schema()->page_size();
  return std::shared_ptr<RawInputFilterTranslation>(
      new RawInputFilterTranslation(translation, input, page_size));
}

}  // namespace rime

namespace rime {

template class ChainFilter<AutoSpacerFilterTranslation>;
template class ChainFilter<RawInputFilterTranslation>;

template class ChainFilter<RawInputFilterTranslation, AutoSpacerFilterTranslation>;

}  // namespace rime
