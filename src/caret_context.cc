#include "caret_context.h"

#include <rime/context.h>

#include "caret_reconstruct.h"
#include "surrounding_source.h"

namespace rime {
namespace {

SurroundingFn g_surrounding_hook = nullptr;
HistoryTextFn g_history_hook = nullptr;

std::optional<SurroundingText> Surrounding() {
  if (g_surrounding_hook) return g_surrounding_hook();
  return GetSurroundingContext();
}

}  // namespace

void SetCaretContextTestHooks(SurroundingFn surrounding, HistoryTextFn history) {
  g_surrounding_hook = surrounding;
  g_history_hook = history;
}

std::optional<CaretContext> GetCaretContext(Context* ctx, AllowReconstruction allow) {
  // Rungs 1-3: the real text, ranked by surrounding_source.cc.
  if (auto s = Surrounding()) {
    CaretContext c;
    c.before = s->before;
    c.after = s->after;
    c.source = s->source;
    c.client_key = s->client_key;
    c.before_depth = s->before_depth;
    c.truncation = s->truncation;
    return c;
  }

  if (allow == AllowReconstruction::kNo) {
    return std::nullopt;
  }

  // Rung 4: what this input method put there. Weaker than the three above --
  // wrong as soon as anything else edits the text -- which is why it is opt-in
  // per consumer. Per-session (the history lives on ctx), unlike rungs 1-3.
  std::string latest;
  if (g_history_hook) {
    latest = g_history_hook();
  } else if (ctx) {
    latest = ctx->commit_history().latest_text();
  }
  auto r = caret::ReconstructFromHistory(caret::ReconstructInput{latest});
  if (!r.usable) {
    // Deliberately nullopt rather than an empty CaretContext: an empty
    // `before` is a positive claim that the caret sits at the start of the
    // text, and NeedSpaceBefore acts on that claim.
    return std::nullopt;
  }
  CaretContext c;
  c.before = r.before;
  c.after = r.after;
  c.source = SurroundingSource::kReconstructed;
  return c;
}

}  // namespace rime
