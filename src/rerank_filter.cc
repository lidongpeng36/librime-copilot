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
#include <chrono>

#include "copilot_engine.h"
#include "history.h"  // copilot::UTF8
#include "prediction_context.h"
#include "rerank.h"
#include "surrounding_source.h"
#include "utils.h"  // copilot::IsACPowerConnected, RegisterPowerChange

namespace rime {

namespace {

// Reorders the head of a candidate list once, then gets out of the way.
class RerankTranslation : public PrefetchTranslation {
 public:
  RerankTranslation(an<Translation> translation, an<RerankContinuations> continuations,
                    const RerankOptions& options, an<RerankTraceStore> traces, std::string input,
                    std::string ctx, std::string llm_ctx, std::string src, TraceSpan span,
                    Scorer* llm_scorer, llm_rerank::SkipReason llm_skip)
      : PrefetchTranslation(translation),
        continuations_(std::move(continuations)),
        options_(options),
        traces_(std::move(traces)),
        input_(std::move(input)),
        ctx_(std::move(ctx)),
        llm_ctx_(std::move(llm_ctx)),
        src_(std::move(src)),
        span_(span),
        llm_scorer_(llm_scorer),
        llm_skip_(llm_skip) {}

 protected:
  bool Replenish() override;

 private:
  an<RerankContinuations> continuations_;
  RerankOptions options_;
  an<RerankTraceStore> traces_;
  std::string input_;
  std::string ctx_;
  // What the model conditions on. Separate from ctx_ because they are
  // different strings for different consumers: ctx_ is the Han-only tail the
  // db is keyed by, and is what the trace records (telemetry_event.h's
  // contract), while llm_ctx_ is the raw text before the caret that the model
  // was trained to read. See ScoringContext in rerank.h.
  std::string llm_ctx_;
  std::string src_;
  // The segment's own extent, captured in Apply(). A translation cannot reach
  // the Segment — it sees candidates — so it is handed in, exactly like input_,
  // ctx_ and src_.
  TraceSpan span_;
  bool reordered_ = false;
  // Non-null exactly when Apply() cleared every guard in the fallback chain
  // for this segment (rerank_llm.h) -- the filter owns the Scorer, this is a
  // borrowed pointer good for the lifetime of one Apply()'s translation.
  // Null means the LLM path was not consulted at all; llm_skip_ says why, for
  // the same "declined vs never ran" distinction rerank_llm.h documents.
  Scorer* llm_scorer_ = nullptr;
  llm_rerank::SkipReason llm_skip_ = llm_rerank::SkipReason::kDisabled;
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

  // Record what re-ranking saw for this segment, even when it promotes
  // nothing: a non-first selection here is still worth attributing to the
  // context it was made against. Filled into a local and handed to the store
  // in one place at the end, so the interleaving of Apply() and Replenish()
  // across segments cannot publish a half-filled trace — and so one segment's
  // decision no longer overwrites another's.
  RerankTrace trace;
  trace.valid = true;
  trace.input = input_;
  trace.start = span_.start;
  trace.end = span_.end;
  trace.ctx = ctx_;
  trace.src = src_;
  // kNone here means "eligible" (llm_scorer_ non-null): Apply()'s fallback
  // chain already cleared every guard before constructing this translation at
  // all, so llm_skip_ itself is kNone in that case too. When llm_scorer_ is
  // null, llm_skip_ carries the actual reason Apply() declined (kDisabled/
  // kBattery/kNoModel/kCold -- never kNoContext, since an empty context is an
  // early return in Apply() before a RerankTranslation is ever built). The
  // llm_scorer_ branch below can still override this back to kCold if the
  // warm cache was lost between IsWarm() and Score() (see the n_scored==0
  // comment there).
  trace.llm_skip = llm_scorer_ ? llm_rerank::SkipReason::kNone : llm_skip_;

  if (llm_scorer_) {
    // The LLM is the primary scoring source (design doc, decision 1); the db
    // loop below runs only when the fallback chain in Apply() could not reach
    // the model at all. A decline here (kNoHan/kMargin) is Decide's verdict,
    // not an outage, so it does NOT fall through to the db -- "declined" and
    // "never ran" must stay distinguishable (rerank_llm.h).
    //
    // Same-span restriction as the db loop below, and gated by the same
    // option (RerankOptions::same_span_only) so both branches can never
    // disagree about which candidates are eligible. It is not superseded by
    // "never touch bucket B" (rerank_llm.h), a different invariant about
    // which candidates may be considered at all, not about which spans may be
    // mixed together. Measured on the claude/dingtalk corpora
    // (final-fix-report.md, F2): 82.6% of top-4 windows contain a differing
    // end(), so this is decisive rather than marginal in either position.
    std::vector<size_t> ends;
    ends.reserve(window.size());
    for (const auto& cand : window) {
      ends.push_back(cand->end());
    }
    const std::vector<size_t> same_span_positions = EligibleBySpan(ends, options_.same_span_only);
    std::vector<an<Candidate>> same_span;
    same_span.reserve(same_span_positions.size());
    for (size_t i : same_span_positions) {
      same_span.push_back(window[i]);
    }
    std::vector<std::string> window_texts;
    window_texts.reserve(same_span.size());
    for (const auto& cand : same_span) {
      window_texts.push_back(cand->text());
    }
    // Truncated before scoring, not after: the PoC measured scoring the full
    // window at 8x the latency of top_n for no accuracy gain (design doc,
    // decision 3) -- Decide's own top_n limit is too late to save that cost.
    const size_t score_n = std::min(window_texts.size(), static_cast<size_t>(options_.llm.top_n));
    const std::vector<std::string> to_score(window_texts.begin(), window_texts.begin() + score_n);
    // The real per-scoring cost, for --wait-for-warm's measurement
    // (task-6-report.md): replay_copilot.cc's own `us.menu` timer wraps
    // WalkCandidates(), which for most segments reads an already-materialized
    // menu (measured: the actual Score() call happens earlier, synchronously
    // inside a preceding select_candidate()) -- that timer was reporting a
    // cache hit, not this. trace.score_us is the only place that measures the
    // real call.
    const auto score_t0 = std::chrono::steady_clock::now();
    const auto scored = llm_scorer_->Score(llm_ctx_, to_score);
    trace.score_us = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now() - score_t0)
                         .count();
    std::vector<float> raw_logprobs;
    std::vector<int> n_tokens;
    raw_logprobs.reserve(scored.size());
    n_tokens.reserve(scored.size());
    for (const auto& s : scored) {
      raw_logprobs.push_back(s.logprob);
      n_tokens.push_back(s.n_tokens);
    }
    const auto decision = llm_rerank::Decide(to_score, raw_logprobs, n_tokens, options_.llm);
    // A lost warm-cache race (LlmScorer::Score finds warmed_context_ != the
    // context it was asked to score under model_mutex_) returns {}, which
    // Decide reads as "no all-Han candidate" (kNoHan) exactly like a real
    // all-non-Han window -- n_scored == 0 is the only way to tell the two
    // apart. Left as kNone here, this would count as an engaged llm_acted
    // sample with ~0us latency and land in the "nohan" bucket, inflating both
    // the acted count and the appearance of a fast no-op. kCold instead:
    // scoring did not actually happen, for the same reason IsWarm()==false
    // means kCold above -- the cache was not hot when it mattered.
    trace.llm_skip =
        decision.n_scored > 0 ? llm_rerank::SkipReason::kNone : llm_rerank::SkipReason::kCold;
    // Carried into the trace regardless of promotion, not just on the
    // promoted branch below: a decline (kNoHan/kMargin) is still a real
    // outcome of consulting the model, and telemetry_commit.cc's `llm_engaged`
    // check (trace.llm_skip == kNone) is what gates whether this is worth
    // reporting, not whether it promoted anything.
    trace.llm.margin = decision.margin;
    trace.llm.n_scored = decision.n_scored;
    trace.llm.us = trace.score_us;
    trace.llm.skip = llm_rerank::SkipReasonName(decision.skip);
    if (decision.incumbent_index >= 0) {
      trace.llm.incumbent = to_score[static_cast<size_t>(decision.incumbent_index)];
    }
    if (decision.promote_index >= 0) {
      const size_t local_idx = static_cast<size_t>(decision.promote_index);
      const size_t from = same_span_positions[local_idx];  // position within `window`
      DLOG(INFO) << "[copilot] rerank llm: promoting '" << window[from]->text() << "' from " << from
                 << " (margin=" << decision.margin << ")";
      trace.llm.text = to_score[local_idx];
      trace.llm.from = static_cast<int>(from);
      if (from > 0) {
        auto promoted = window[from];
        window.erase(window.begin() + from);
        window.insert(window.begin(), promoted);
      }
    } else {
      DLOG(INFO) << "[copilot] rerank llm: skip=" << llm_rerank::SkipReasonName(decision.skip);
    }
  } else {
    if (llm_skip_ != llm_rerank::SkipReason::kNone) {
      DLOG(INFO) << "[copilot] rerank llm: skip=" << llm_rerank::SkipReasonName(llm_skip_);
    }

    // Only reorder among candidates covering the same input span as the
    // current first one — promoting across spans would change how much input
    // Space commits, which is far more surprising than a different character.
    // That judgement is now `copilot/rerank/same_span_only`; see its comment
    // in rerank_filter.h for what it costs and why it is a switch.
    std::vector<size_t> ends;
    ends.reserve(window.size());
    for (const auto& cand : window) {
      ends.push_back(cand->end());
    }
    const std::vector<size_t> positions = EligibleBySpan(ends, options_.same_span_only);
    std::vector<std::string> texts;
    texts.reserve(positions.size());
    for (size_t i : positions) {
      texts.push_back(window[i]->text());
    }

    // The span is the SEGMENT's, not the head candidate's. `head_end` above
    // is the head candidate's end, which is smaller whenever that candidate
    // is a partial match — and the commit-side lookup keys on the segment's
    // extent (telemetry_commit.cc, via TraceSpanOf). Keying the write on
    // `head_end` meant those lookups missed, the event was written with `rr`
    // absent, and analyze_telemetry.py filed it as "misrank": the bucket that
    // blames the translator instead of this filter. `head_end` still decides
    // which candidates may be reordered, above — that is a separate question
    // and its behaviour is unchanged.

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
  }

  if (traces_) {
    traces_->Record(trace);
  }

  for (auto& cand : window) {
    cache_.push_back(cand);
  }
  return !cache_.empty();
}

// Records a trace-only outcome for a segment this filter is about to skip
// entirely -- no RerankTranslation is built for it, so without this call
// nothing is ever written for it into `traces`. That silently drops it from
// telemetry: BuildCommitEvents (telemetry_commit.cc) looks the segment up and
// gets nullptr, and StatsAccumulator::Observe(nullptr) counts it in
// `segments` but in no skip bucket at all. Two consequences that matter: the
// design doc's stats example names a "noctx" bucket the code could never
// actually produce, and warm-hit rate = acted/(acted+cold) is biased upward
// by every dropped `cold` segment -- the one number the design says scales
// the entire benefit. `record` (the db side) is left at its default (empty
// text): the db branch never ran for these segments, so there is nothing
// promoted to report.
void RecordSkipTrace(const an<RerankTraceStore>& traces, const std::optional<TraceSpan>& span,
                     const std::string& input, const std::string& ctx, const std::string& src,
                     llm_rerank::SkipReason llm_skip) {
  if (!traces || !span) {
    return;
  }
  RerankTrace trace;
  trace.valid = true;
  trace.input = input;
  trace.start = span->start;
  trace.end = span->end;
  trace.ctx = ctx;
  trace.src = src;
  trace.llm_skip = llm_skip;
  traces->Record(trace);
}

}  // namespace

CopilotRerankFilter::CopilotRerankFilter(const Ticket& ticket, const an<CopilotDb>& db,
                                         const RerankOptions& options, an<RerankTraceStore> traces,
                                         an<CopilotEngine> copilot_engine)
    : Filter(ticket),
      db_(db),
      options_(options),
      traces_(std::move(traces)),
      copilot_engine_(std::move(copilot_engine)),
      scorer_(copilot_engine_ ? copilot_engine_->scorer() : nullptr) {
  LOG(INFO) << "[copilot] rerank: enable=" << options_.enable
            << ", max_context_chars=" << options_.max_context_chars
            << ", window=" << options_.window << ", max_rank=" << options_.max_rank
            << ", same_span_only=" << options_.same_span_only << ", db=" << (db_ ? "ok" : "missing")
            << ", llm.enable=" << options_.llm.enable
            << ", llm.model=" << (scorer_ ? "ok" : "missing");
  // Only worth tracking AC/battery state -- and paying for the monitor
  // callback -- when there is a scorer to gate at all, and the config cares
  // (battery_active already means "run regardless").
  if (scorer_ && options_.llm.enable && !options_.llm.battery_active) {
    is_on_ac_ = ::copilot::IsACPowerConnected();
    power_token_ =
        ::copilot::RegisterPowerChange([this](bool is_ac_power) { is_on_ac_ = is_ac_power; });
  }
}

CopilotRerankFilter::~CopilotRerankFilter() {
  // The monitor is a process-wide singleton: leaving the `this`-capturing
  // callback registered means the next plug/unplug writes into freed memory
  // (this filter dies on every schema redeploy) -- same hazard and same fix
  // as LLMProvider (llm_provider.cc:74-80).
  ::copilot::UnregisterPowerChange(power_token_);
  power_token_ = 0;
}

bool CopilotRerankFilter::AppliesToSegment(Segment* segment) {
  // librime calls this immediately before Apply() for the same segment
  // (engine.cc:225-226 -> menu.cc:22-24), and Apply() is handed only a
  // Translation. This is therefore the one point where the segment's own extent
  // — the span the commit-side lookup will search for — can be captured.
  if (!segment) {
    pending_trace_span_.reset();
    pending_segment_ = nullptr;
    return true;
  }
  if (segment->HasTag("copilot")) {
    // Declined: this segment's Apply() will never run, so nothing may be left
    // behind for the next segment's Apply() to misattribute it to.
    pending_trace_span_.reset();
    pending_segment_ = nullptr;
    return false;
  }
  pending_trace_span_ = TraceSpanOf(*segment);
  // Safe to keep past this call: per the class comment, this pointer is into
  // the live Composition (Engine::TranslateSegments iterates `*segments` by
  // reference), and Apply() for this same segment runs synchronously, before
  // anything could invalidate it.
  pending_segment_ = segment;
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
  const Segment* segment = pending_segment_;
  pending_segment_ = nullptr;

  if (!options_.enable || !translation) {
    return translation;
  }
  // No real text before the caret (Chrome/Electron, terminals, Linux) means we
  // would be guessing from commit history, which cannot see a mouse click.
  auto surrounding = GetSurroundingContext();
  if (!surrounding) {
    return translation;
  }
  // surrounding->before is real application text and stops at whatever is
  // already committed there — it cannot see segments the user has just
  // selected within the current, still-uncommitted composition (typing 这个顺
  // 序是故意的 as one run and selecting 这个 leaves 顺序's candidates re-ranked
  // with no idea 这个 was just chosen). ConfirmedPrefix supplies exactly that.
  const std::string confirmed_prefix = ConfirmedPrefix(engine_->context()->composition(), segment);
  const std::string before = surrounding->before + confirmed_prefix;
  // Two contexts, because the two scorers can use different things. The db is
  // keyed by Han sequences and can look up nothing else; the model reads
  // whatever is there. Collapsing them was costing the model 68.2% of
  // segments -- see ScoringContext in rerank.h.
  const std::string context = TrailingCjkRun(before, options_.max_context_chars);
  const std::string llm_context = ScoringContext(before, options_.llm.context_chars);

  // The LLM fallback chain, checked in the exact order
  // docs/superpowers/specs/2026-08-17-llm-rerank-design.md ("Fallback chain")
  // fixes: each unmet guard yields a distinct SkipReason, and the live
  // distribution of those reasons is the diagnostic that tells us where the
  // benefit leaks. Getting the order wrong does not break the feature; it
  // silently mislabels the diagnosis.
  //
  // Loaded() is checked AFTER the warm attempt, not before: WarmUp() is what
  // loads the model in the first place (EnsureLoaded(), llm_scorer.cc), so
  // gating the warm attempt behind Loaded() would mean the model could never
  // load at all -- nothing else here would ever call WarmUp() to make
  // Loaded() true. Loaded() gates only whether this segment may SCORE.
  llm_rerank::SkipReason llm_skip = llm_rerank::SkipReason::kNone;
  bool llm_eligible = false;
  if (!options_.llm.enable) {
    llm_skip = llm_rerank::SkipReason::kDisabled;
  } else if (!is_on_ac_ && !options_.llm.battery_active) {
    llm_skip = llm_rerank::SkipReason::kBattery;
  } else if (llm_context.empty()) {
    llm_skip = llm_rerank::SkipReason::kNoContext;
  } else if (!scorer_) {
    // No model configured at all -- nothing to warm or score against.
    llm_skip = llm_rerank::SkipReason::kNoModel;
  } else if (!scorer_->IsWarm(llm_context)) {
    llm_skip = llm_rerank::SkipReason::kCold;
    // Never block the input thread waiting for the model: post the prefill
    // and fall through to the db path for this segment. The only warming
    // trigger this filter owns -- Task 5 adds the other two (on commit, on
    // composition start), both earlier than "the user already typed into the
    // gap this would have filled".
    scorer_->WarmUp(llm_context);
  } else if (!scorer_->Loaded()) {
    // The warm attempt above (this call or an earlier one) never finished
    // loading the model -- e.g. a load failure, logged once inside
    // EnsureLoaded() and never retried.
    llm_skip = llm_rerank::SkipReason::kNoModel;
  } else {
    llm_eligible = true;
  }

  // Empty here means the DB has nothing to key on -- `context` is the Han-only
  // tail -- and it ALSO ends the LLM path, which is a constraint the model
  // does not have: it reads punctuation and Latin, and 68.2% of segments carry
  // no trailing Han (`llm_skip=noctx`) purely because the harness splits
  // requests at maximal Han runs.
  //
  // Lifting it was tried and reverted. Those newly-eligible segments are
  // exactly the ones where RawInputFilter has put the raw keystrokes first,
  // and promoting into them displaces that head: measured, bucket B+D fell
  // from 43.8% to 13.4% of segments. `Decide` enforces "never touch bucket B"
  // when choosing WHICH candidate wins (rerank_llm.h picks the first all-Han
  // as incumbent) but nothing enforces it on where the winner is INSERTED.
  // Reaching those segments needs that second half designed first; until then
  // this stays, and `llm_skip=noctx` is the honest measure of what it costs.
  if (context.empty()) {
    RecordSkipTrace(traces_, span, engine_->context()->input(), context,
                    SurroundingSourceName(surrounding->source), llm_skip);
    return translation;
  }
  // A missing db only takes the db branch down with it -- the LLM branch
  // above needs no db at all, and is the primary scoring source now.
  auto continuations = db_ ? LookupContinuations(context) : an<RerankContinuations>();
  if (!llm_eligible && (!continuations || continuations->empty())) {
    // Nothing either path can act on: the db has no continuations for this
    // context (or no db loaded at all), and the LLM guard chain above already
    // ruled out the LLM path.
    RecordSkipTrace(traces_, span, engine_->context()->input(), context,
                    SurroundingSourceName(surrounding->source), llm_skip);
    return translation;
  }
  DLOG(INFO) << "[copilot] rerank context: '" << context
             << "' keys=" << (continuations ? continuations->size() : 0)
             << " llm_eligible=" << llm_eligible
             << " llm_skip=" << llm_rerank::SkipReasonName(llm_skip);
  // With no span there is nothing a trace could safely be keyed on, so this
  // translation records none rather than guessing. Re-ranking itself runs
  // exactly the same either way.
  return New<RerankTranslation>(
      translation, continuations, options_, span ? traces_ : an<RerankTraceStore>(),
      engine_->context()->input(), context, llm_context, SurroundingSourceName(surrounding->source),
      span.value_or(TraceSpan{}), llm_eligible ? scorer_ : nullptr, llm_skip);
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
      config->GetBool("copilot/rerank/same_span_only", &options.same_span_only);
      config->GetBool("copilot/rerank/llm/enable", &options.llm.enable);
      config->GetString("copilot/rerank/llm/model", &options.llm.model);
      config->GetBool("copilot/rerank/llm/battery_active", &options.llm.battery_active);
      config->GetInt("copilot/rerank/llm/top_n", &options.llm.top_n);
      config->GetInt("copilot/rerank/llm/context_chars", &options.llm.context_chars);
      // Config has GetDouble but no GetFloat (rime/config/config_component.h)
      // -- read into a double, seeded with the struct default, then narrow.
      double margin_double = static_cast<double>(options.llm.margin);
      config->GetDouble("copilot/rerank/llm/margin", &margin_double);
      double exponent_double = static_cast<double>(options.llm.length_exponent);
      config->GetDouble("copilot/rerank/llm/length_exponent", &exponent_double);
      options.llm.margin = static_cast<float>(margin_double);
      options.llm.length_exponent = static_cast<float>(exponent_double);
    }
  }
  options.max_context_chars = std::clamp(options.max_context_chars, 1, 64);
  options.window = std::clamp(options.window, 1, 200);
  options.max_rank = std::clamp(options.max_rank, 1, 100000);
  options.llm.top_n = std::clamp(options.llm.top_n, 1, options.window);
  options.llm.margin = std::clamp(options.llm.margin, 0.0f, 100.0f);
  options.llm.length_exponent = std::clamp(options.llm.length_exponent, 0.0f, 2.0f);
  an<CopilotDb> db;
  an<RerankTraceStore> traces;
  // The Scorer lives on CopilotEngine now (Task 5), so the processor's
  // warm-cache triggers and this filter's Score() calls reach the same
  // instance -- see CopilotEngine::scorer(). GetInstance() also builds it
  // (from copilot/rerank/llm/*, kept in lockstep in
  // CopilotEngineComponent::Create) if no one has already, and this filter
  // keeps the returned an<> alive for as long as it lives, in case it is the
  // only component reaching this schema's CopilotEngine at all.
  an<CopilotEngine> copilot_engine;
  if (options.enable && engine_factory_) {
    db = engine_factory_->GetDb(db_name);
    if (!db) {
      LOG(ERROR) << "[copilot] rerank: failed to load db " << db_name;
    }
    traces =
        engine_factory_->GetRerankTraces(ticket.schema ? ticket.schema->schema_id() : string());
    copilot_engine = engine_factory_->GetInstance(ticket);
  }
  return new CopilotRerankFilter(ticket, db, options, traces, copilot_engine);
}

}  // namespace rime
