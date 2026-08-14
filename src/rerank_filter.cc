#include "rerank_filter.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/schema.h>
#include <rime/segmentation.h>
#include <rime/ticket.h>
#include <rime/translation.h>

#include <algorithm>

#include "copilot_engine.h"
#include "history.h"  // copilot::UTF8
#include "prediction_context.h"
#include "rerank.h"
#include "surrounding_source.h"

namespace rime {

namespace {

// Reorders the head of a candidate list once, then gets out of the way.
class RerankTranslation : public PrefetchTranslation {
 public:
  RerankTranslation(an<Translation> translation, an<RerankContinuations> continuations,
                    const RerankOptions& options, an<RerankTraceStore> traces, std::string input,
                    std::string ctx, std::string src, TraceSpan span)
      : PrefetchTranslation(translation),
        continuations_(std::move(continuations)),
        options_(options),
        traces_(std::move(traces)),
        input_(std::move(input)),
        ctx_(std::move(ctx)),
        src_(std::move(src)),
        span_(span) {}

 protected:
  bool Replenish() override;

 private:
  an<RerankContinuations> continuations_;
  RerankOptions options_;
  an<RerankTraceStore> traces_;
  std::string input_;
  std::string ctx_;
  std::string src_;
  // The segment's own extent, captured in Apply(). A translation cannot reach
  // the Segment — it sees candidates — so it is handed in, exactly like input_,
  // ctx_ and src_.
  TraceSpan span_;
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

  // Record what re-ranking saw for this segment, even when it promotes
  // nothing: a non-first selection here is still worth attributing to the
  // context it was made against. Filled into a local and handed to the store
  // in one place at the end, so the interleaving of Apply() and Replenish()
  // across segments cannot publish a half-filled trace — and so one segment's
  // decision no longer overwrites another's.
  //
  // The span is the SEGMENT's, not the head candidate's. `head_end` above is
  // the head candidate's end, which is smaller whenever that candidate is a
  // partial match — and the commit-side lookup keys on the segment's extent
  // (telemetry_commit.cc, via TraceSpanOf). Keying the write on `head_end`
  // meant those lookups missed, the event was written with `rr` absent, and
  // analyze_telemetry.py filed it as "misrank": the bucket that blames the
  // translator instead of this filter. `head_end` still decides which
  // candidates may be reordered, above — that is a separate question and its
  // behaviour is unchanged.
  RerankTrace trace;
  trace.valid = true;
  trace.input = input_;
  trace.start = span_.start;
  trace.end = span_.end;
  trace.ctx = ctx_;
  trace.src = src_;

  // Back off from the most specific context key to the least.
  for (const auto& set : *continuations_) {
    auto promotion = PickPromotion(texts, set.entries, options_.max_rank);
    if (promotion.index < 0) {
      continue;
    }
    const size_t from = positions[static_cast<size_t>(promotion.index)];
    DLOG(INFO) << "[copilot] rerank: promoting '" << window[from]->text() << "' from " << from
               << " (rank=" << promotion.rank << ")";
    trace.record.key = set.key;
    trace.record.key_len = set.key_len;
    trace.record.n = static_cast<int>(set.entries.size());
    trace.record.text = window[from]->text();
    trace.record.from = static_cast<int>(from);
    trace.record.rank = promotion.rank;
    trace.record.level = promotion.level;
    if (from > 0) {
      auto promoted = window[from];
      window.erase(window.begin() + from);
      window.insert(window.begin(), promoted);
    }
    break;
  }

  if (traces_) {
    traces_->Record(trace);
  }

  for (auto& cand : window) {
    cache_.push_back(cand);
  }
  return !cache_.empty();
}

}  // namespace

CopilotRerankFilter::CopilotRerankFilter(const Ticket& ticket, const an<CopilotDb>& db,
                                         const RerankOptions& options, an<RerankTraceStore> traces)
    : Filter(ticket), db_(db), options_(options), traces_(std::move(traces)) {
  LOG(INFO) << "[copilot] rerank: enable=" << options_.enable
            << ", max_context_chars=" << options_.max_context_chars
            << ", window=" << options_.window << ", max_rank=" << options_.max_rank
            << ", db=" << (db_ ? "ok" : "missing");
}

bool CopilotRerankFilter::AppliesToSegment(Segment* segment) {
  // librime calls this immediately before Apply() for the same segment
  // (engine.cc:225-226 -> menu.cc:22-24), and Apply() is handed only a
  // Translation. This is therefore the one point where the segment's own extent
  // — the span the commit-side lookup will search for — can be captured.
  if (!segment) {
    pending_trace_span_.reset();
    return true;
  }
  if (segment->HasTag("copilot")) {
    // Declined: this segment's Apply() will never run, so nothing may be left
    // behind for the next segment's Apply() to misattribute it to.
    pending_trace_span_.reset();
    return false;
  }
  pending_trace_span_ = TraceSpanOf(*segment);
  return true;
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
      ContinuationSet set;
      set.key = *it;
      set.key_len = static_cast<int>(::copilot::UTF8(*it).size());
      set.entries = std::move(entries);
      cached_continuations_->push_back(std::move(set));
    }
  }
  return cached_continuations_;
}

an<Translation> CopilotRerankFilter::Apply(an<Translation> translation, CandidateList* candidates) {
  // Taken before any early return, so a span left by a segment this filter
  // declined can never be picked up by a later segment's translation.
  const std::optional<TraceSpan> span = pending_trace_span_;
  pending_trace_span_.reset();

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
  // With no span there is nothing a trace could safely be keyed on, so this
  // translation records none rather than guessing. Re-ranking itself runs
  // exactly the same either way.
  return New<RerankTranslation>(
      translation, continuations, options_, span ? traces_ : an<RerankTraceStore>(),
      engine_->context()->input(), context, SurroundingSourceName(surrounding->source),
      span.value_or(TraceSpan{}));
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
  an<RerankTraceStore> traces;
  if (options.enable && engine_factory_) {
    db = engine_factory_->GetDb(db_name);
    if (!db) {
      LOG(ERROR) << "[copilot] rerank: failed to load db " << db_name;
    }
    traces =
        engine_factory_->GetRerankTraces(ticket.schema ? ticket.schema->schema_id() : string());
  }
  return new CopilotRerankFilter(ticket, db, options, traces);
}

}  // namespace rime
