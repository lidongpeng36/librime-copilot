#include "auto_spacer_filter.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/filter.h>
#include <rime/ticket.h>
#include <rime/translation.h>

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
 public:
  AutoSpacerFilterTranslation(an<Translation> translation, AutoSpacerFilter* filter)
      : PrefetchTranslation(translation), filter_(filter) {}

 protected:
  virtual bool Replenish();

  AutoSpacerFilter* filter_;
};

bool AutoSpacerFilterTranslation::Replenish() {
  auto next = translation_->Peek();
  translation_->Next();
  if (next && !filter_->Convert(next, &cache_)) {
    cache_.push_back(next);
  }
  return !cache_.empty();
}

}  // namespace rime

namespace rime {

AutoSpacerFilter::AutoSpacerFilter(const Ticket& ticket) : Filter(ticket) {}

bool AutoSpacerFilter::Convert(const an<Candidate>& original, CandidateQueue* result) {
  auto& text = original->text();
  auto is_ascii = IsAsciiFirstChar(text);
  auto new_text = text;
  if (is_ascii != is_en_) {
    new_text = " " + text;
  }
  result->push_back(New<ShadowCandidate>(original, "autospacer", new_text));
  return true;
}

an<Translation> AutoSpacerFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  auto* ctx = engine_->context();
  if (!ctx || ctx->commit_history().empty() || !candidates) {
    return translation;
  }

  const auto& last = ctx->commit_history().back().text;
  const auto& input = ctx->input();
  DLOG(INFO) << "last_commit: '" << last << "'" << ", input:'" << ctx->input() << "'";

  if (last.empty() || std::isspace(static_cast<unsigned char>(last[0]))) {
    return translation;
  }
  if (input.empty() || std::isspace(static_cast<unsigned char>(input[0]))) {
    DLOG(INFO) << "[Filter] input first empty. skip";
    return translation;
  }
  is_en_ = IsAsciiLastChar(last);
  DLOG(INFO) << "[Filter] insert space for cands...";
  return New<AutoSpacerFilterTranslation>(translation, this);
}

}  // namespace rime
