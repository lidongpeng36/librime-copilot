#include "plugin_commit.h"

#include <rime/composition.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/segmentation.h>

namespace rime {

bool NotifyForLearning(Context* ctx) {
  // Same guard Context::Commit() itself uses (context.cc:19-20) -- notifying
  // Memory::OnCommit with nothing composed is pointless work, not a bug, but
  // there is no reason to pay for it.
  if (!ctx->IsComposing()) {
    return false;
  }
  // Mark the segment the user just confirmed as confirmed, or Memory will
  // queue it and never save it.
  //
  // ScriptTranslator::ProcessSegmentOnCommit (script_translator.cc:273-287)
  // pushes each recognized phrase into a MEMBER queue_ and flushes it only
  // when `!recognized || seg.status >= Segment::kConfirmed`. That status is
  // assigned in exactly one place in all of librime -- ConcreteEngine::OnSelect
  // (engine.cc:264) -- reached only through select_notifier_, and AutoSpacer's
  // commit paths go through neither Context::Select() nor
  // ConfirmCurrentSelection() (the number-key site assigns seg.selected_index
  // directly, precisely to avoid Rime's select path). Rime's own Space handling
  // does confirm, which is why the machine predating the surrounding-text
  // sources learned normally.
  //
  // Left unmarked the phrase is not merely unsaved: it sits in the queue until
  // some later commit has an unrecognized candidate, and is then written as ONE
  // entry spanning several unrelated commits -- the cross-word-boundary
  // fragment class tools/rime_copilot/clean.py exists to prune, generated into
  // the user dictionary rather than imported into it. Measured before this
  // line existed: two single-segment Space commits produced two EMPTY LevelDB
  // WriteBatches and learned nothing, while an earlier multi-commit sentence
  // was memorised as a single 30-character run.
  //
  // Setting the flag rather than calling ConfirmCurrentSelection() is
  // deliberate: that fires select_notifier_ -> ConcreteEngine::OnSelect, which
  // also runs seg.Close() and composition().Forward(), and under `_auto_commit`
  // calls ctx->Commit() -- committing again text the caller has already emitted
  // itself. The flag is the whole of what Memory reads, and it is true: Space
  // IS the user confirming this segment.
  if (!ctx->composition().empty()) {
    Segment& last = ctx->composition().back();
    if (last.status < Segment::kConfirmed) {
      last.status = Segment::kConfirmed;
    }
  }
  // Restored by scope exit, not by a trailing statement: the notifier reaches
  // Memory::Memorize and through it LevelDB, and if anything there throws, a
  // `dumb` left set makes Context::GetCommitText() return "" for the life of
  // the context -- the input method would stop committing text, silently and
  // with no symptom pointing here.
  struct DumbRestorer {
    Context* ctx;
    bool previous;
    ~DumbRestorer() { ctx->set_option("dumb", previous); }
  } restorer{ctx, ctx->get_option("dumb")};
  ctx->set_option("dumb", true);
  // Deliberately NOT ctx->Commit(): see the declaration comment in
  // plugin_commit.h for why this stops short of the Clear() that Commit()
  // would perform, and leaves it to RunCommitSequence below.
  ctx->commit_notifier()(ctx);
  return true;
}

void RunCommitSequence(CommitSteps& steps, const std::string& text, bool learn) {
  if (text.empty()) {
    return;
  }
  steps.CommitText(text);
  if (learn) {
    steps.NotifyForLearning();
    // Load-bearing, and the reason this function exists. NotifyForLearning
    // reaches ConcreteEngine::OnCommit, which appends per-segment records;
    // Clear() below then fires update_notifier_ synchronously into a reader
    // that takes commit_history().back().text. Without this re-assert that
    // reader gets a per-segment record instead of what was committed.
    steps.PushRecord(text);
  }
  steps.Clear();
}

namespace {

class RimeCommitSteps : public CommitSteps {
 public:
  RimeCommitSteps(Engine* engine, Context* ctx) : engine_(engine), ctx_(ctx) {}

  void CommitText(const std::string& text) override { engine_->CommitText(text); }
  void NotifyForLearning() override { rime::NotifyForLearning(ctx_); }
  void PushRecord(const std::string& text) override {
    ctx_->commit_history().push_back({"raw", text});
  }
  void Clear() override { ctx_->Clear(); }

 private:
  Engine* engine_;
  Context* ctx_;
};

}  // namespace

ProcessResult CommitThroughPlugin(Engine* engine, Context* ctx, const std::string& text,
                                  bool learn) {
  RimeCommitSteps steps(engine, ctx);
  RunCommitSequence(steps, text, learn);
  return text.empty() ? kNoop : kAccepted;
}

}  // namespace rime
