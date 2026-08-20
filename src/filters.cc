#include <rime/context.h>
#include <rime/engine.h>
#include <rime/filter.h>
#include <rime/ticket.h>
#include <rime/translation.h>
#include "rime/schema.h"

#include "filters.h"

#include <string>
#include <vector>

#include "raw_input_util.h"

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
  const auto& latest = ctx->commit_history().back();
  DLOG(INFO) << "[Filter] latest commit: '" << latest.text << "' [" << latest.type << "]";
  if (latest.type == "thru") {
    DLOG(INFO) << "[Filter] last commit is thru. skip";
    return translation;
  }
  const auto& last = latest.text;
  const auto& input = ctx->input();
  DLOG(INFO) << "[Filter] last_commit: '" << last << "'" << ", input:'" << ctx->input() << "'";

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
  // Peek the rest of the page, then place the raw candidate among it.
  //
  // The placement used to be decided inline, and got it wrong in both
  // directions at once: a `sentence` first candidate short-circuited the
  // letters to slot 0, so did any first candidate not spanning the whole
  // input, and the syllable count the feature is defined around
  // (`(input.size() + 1) / 2`) was computed and never read -- so a short
  // input got the candidate it does not need and a long one got it in the
  // one position it must never take. It is a pure decision now
  // (raw_input_util.h), tested without an engine in
  // test/raw_input_order_test.cc.
  std::vector<an<Candidate>> head{next};
  const int room = page_size_ > 1 ? page_size_ - 1 : 0;
  while (static_cast<int>(head.size()) < room) {
    auto more = translation_->Peek();
    if (!more) {
      break;
    }
    translation_->Next();
    head.push_back(more);
  }
  std::vector<std::string> texts;
  texts.reserve(head.size());
  for (const auto& candidate : head) {
    DLOG(INFO) << "[CAND] " << texts.size() << ": '" << candidate->text() << "'|"
               << candidate->type() << "|" << candidate->start() << "|" << candidate->end() << "|"
               << candidate->quality();
    texts.push_back(candidate->text());
  }
  // `slot` may equal head.size() -- the letters go last when the page is not
  // full -- so this walks one past the end.
  const size_t slot = raw_input_detail::Slot(texts, input_, page_size_);
  for (size_t i = 0; i <= head.size(); ++i) {
    if (i == slot) {
      cache_.push_back(New<SimpleCandidate>("raw", 0, input_.size(), input_));
    }
    if (i < head.size()) {
      cache_.push_back(head[i]);
    }
  }
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
