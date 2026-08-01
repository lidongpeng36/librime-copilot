#include "rerank_filter.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/ticket.h>
#include <rime/translation.h>

#include <algorithm>

#include "copilot_engine.h"
#include "prediction_context.h"
#include "rerank.h"
#include "surrounding_source.h"

namespace rime {

namespace {

// Reorders the head of a candidate list once, then gets out of the way.
class RerankTranslation : public PrefetchTranslation {
 public:
  RerankTranslation(an<Translation> translation, an<RerankContinuations> continuations,
                    const RerankOptions& options)
      : PrefetchTranslation(translation),
        continuations_(std::move(continuations)),
        options_(options) {}

 protected:
  bool Replenish() override;

 private:
  an<RerankContinuations> continuations_;
  RerankOptions options_;
  bool reordered_ = false;
};

bool RerankTranslation::Replenish() {
  if (reordered_) {
    auto next = translation_->Peek();
    translation_->Next();
    if (next) {
      cache_.push_back(next);
    }
    return !cache_.empty();
  }
  reordered_ = true;

  vector<an<Candidate>> window;
  while (!translation_->exhausted() && window.size() < static_cast<size_t>(options_.window)) {
    auto cand = translation_->Peek();
    if (!cand) {
      break;
    }
    window.push_back(cand);
    if (!translation_->Next()) {
      break;
    }
  }
  if (window.empty()) {
    return false;
  }

  // Only reorder among candidates covering the same input span as the current
  // first one — promoting across spans would change how much input Space
  // commits, which is far more surprising than a different character.
  const size_t head_end = window.front()->end();
  std::vector<std::string> texts;
  std::vector<size_t> positions;
  for (size_t i = 0; i < window.size(); ++i) {
    if (window[i]->end() == head_end) {
      texts.push_back(window[i]->text());
      positions.push_back(i);
    }
  }

  // Back off from the most specific context key to the least.
  for (const auto& entries : *continuations_) {
    auto promotion = PickPromotion(texts, entries, options_.max_rank);
    if (promotion.index < 0) {
      continue;
    }
    const size_t from = positions[static_cast<size_t>(promotion.index)];
    DLOG(INFO) << "[copilot] rerank: promoting '" << window[from]->text() << "' from " << from
               << " (rank=" << promotion.rank << ")";
    if (from > 0) {
      auto promoted = window[from];
      window.erase(window.begin() + from);
      window.insert(window.begin(), promoted);
    }
    break;
  }

  for (auto& cand : window) {
    cache_.push_back(cand);
  }
  return !cache_.empty();
}

}  // namespace

CopilotRerankFilter::CopilotRerankFilter(const Ticket& ticket, const an<CopilotDb>& db,
                                         const RerankOptions& options)
    : Filter(ticket), db_(db), options_(options) {
  LOG(INFO) << "[copilot] rerank: enable=" << options_.enable
            << ", max_context_chars=" << options_.max_context_chars
            << ", window=" << options_.window << ", max_rank=" << options_.max_rank
            << ", db=" << (db_ ? "ok" : "missing");
}

bool CopilotRerankFilter::AppliesToSegment(Segment* segment) {
  return !segment || !segment->HasTag("copilot");
}

an<RerankContinuations> CopilotRerankFilter::LookupContinuations(const std::string& context) {
  if (cached_continuations_ && context == cached_context_) {
    return cached_continuations_;
  }
  cached_context_ = context;
  // A fresh object rather than clearing the old one: translations built for the
  // previous context keep holding it.
  cached_continuations_ = New<RerankContinuations>();
  // BuildLookupKeys is shortest-first; walk it backwards so the most specific
  // key is tried first.
  const auto keys = BuildLookupKeys(context, options_.max_context_chars);
  for (auto it = keys.rbegin(); it != keys.rend(); ++it) {
    std::vector<::copilot::Entry> entries;
    if (auto* candidates = db_->Lookup(*it)) {
      entries.reserve(candidates->size);
      auto* entry = candidates->begin();
      for (uint32_t i = 0; i < candidates->size; ++i, ++entry) {
        entries.push_back({db_->GetEntryText(*entry), entry->weight, ::copilot::ProviderType::kDB});
      }
    }
    if (!entries.empty()) {
      cached_continuations_->push_back(std::move(entries));
    }
  }
  return cached_continuations_;
}

an<Translation> CopilotRerankFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  if (!options_.enable || !db_ || !translation) {
    return translation;
  }
  // No real text before the caret (Chrome/Electron, terminals, Linux) means we
  // would be guessing from commit history, which cannot see a mouse click.
  auto surrounding = GetSurroundingContext();
  if (!surrounding) {
    return translation;
  }
  const std::string context = TrailingCjkRun(surrounding->before, options_.max_context_chars);
  if (context.empty()) {
    return translation;
  }
  auto continuations = LookupContinuations(context);
  if (!continuations || continuations->empty()) {
    return translation;
  }
  DLOG(INFO) << "[copilot] rerank context: '" << context << "' keys=" << continuations->size();
  return New<RerankTranslation>(translation, continuations, options_);
}

CopilotRerankFilterComponent::CopilotRerankFilterComponent(
    an<CopilotEngineComponent> engine_factory)
    : engine_factory_(engine_factory) {}

CopilotRerankFilterComponent::~CopilotRerankFilterComponent() {}

CopilotRerankFilter* CopilotRerankFilterComponent::Create(const Ticket& ticket) {
  RerankOptions options;
  string db_name = "copilot.db";
  if (auto* schema = ticket.schema) {
    if (auto* config = schema->config()) {
      config->GetString("copilot/db", &db_name);
      config->GetBool("copilot/rerank/enable", &options.enable);
      config->GetInt("copilot/rerank/max_context_chars", &options.max_context_chars);
      config->GetInt("copilot/rerank/window", &options.window);
      config->GetInt("copilot/rerank/max_rank", &options.max_rank);
    }
  }
  options.max_context_chars = std::clamp(options.max_context_chars, 1, 64);
  options.window = std::clamp(options.window, 1, 200);
  options.max_rank = std::clamp(options.max_rank, 1, 100000);
  an<CopilotDb> db;
  if (options.enable && engine_factory_) {
    db = engine_factory_->GetDb(db_name);
    if (!db) {
      LOG(ERROR) << "[copilot] rerank: failed to load db " << db_name;
    }
  }
  return new CopilotRerankFilter(ticket, db, options);
}

}  // namespace rime
