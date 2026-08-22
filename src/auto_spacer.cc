#include "auto_spacer.h"

#include <rime/candidate.h>
#include <rime/config.h>
#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

#include <rime/menu.h>
#include <rime/schema.h>
#include <cctype>

#include "auto_spacer_util.h"
#include "surrounding_source.h"

namespace rime {

using namespace auto_spacer_detail;

namespace {

inline bool IsNumKey(int keycode) { return (keycode >= XK_0 && keycode <= XK_9); }

inline bool IsLetterKey(int keycode) {
  return (keycode >= XK_a && keycode <= XK_z) || (keycode >= XK_A && keycode <= XK_Z);
}

inline bool IsAlphabetKey(int keycode) { return (IsNumKey(keycode) || IsLetterKey(keycode)); }

// { [ ( < `
inline bool IsLeftPunctKey(int keycode) {
  return keycode == XK_bracketleft || keycode == XK_parenleft || keycode == XK_braceleft ||
         keycode == XK_less || keycode == XK_quoteleft;
}

// } ] ) > `
inline bool IsRightPunctKey(int keycode) {
  return keycode == XK_bracketright || keycode == XK_parenright || keycode == XK_braceright ||
         keycode == XK_greater || keycode == XK_quoteright;
}

inline bool IsPairPunctKey(int keycode) {
  return IsLeftPunctKey(keycode) || IsRightPunctKey(keycode);
}

// ! ? :
inline bool IsModifierPunctKey(int keycode) {
  return keycode == XK_exclam || keycode == XK_question || keycode == XK_colon ||
         IsPairPunctKey(keycode);
}

inline bool IsAsciiPunctuationCode(int keycode) {
  return keycode >= 0 && keycode < 0x80 && std::ispunct(static_cast<unsigned char>(keycode));
}

// 从 schema 的 punctuator/full_shape 里查找 keycode 对应的"默认" 中文标点.
// 优先顺序: full_shape → symbols. 返回空串表示没有匹配的中文映射.
// 对于各种 ConfigItem 类型, 选择规则与 Rime 的 Punctuator 一致:
//   - ConfigValue: 整个值
//   - ConfigList:  第 0 项 (AlternatingPunct 的默认)
//   - ConfigMap.commit: commit 值 (AutoCommitPunct)
//   - ConfigMap.pair:   pair[0] (PairedPunct 的默认)
inline std::string ExtractPunctDefault(an<ConfigItem> item) {
  if (!item) return {};
  if (auto v = As<ConfigValue>(item)) {
    return v->str();
  }
  if (auto list = As<ConfigList>(item)) {
    if (list->size() > 0) {
      if (auto v = list->GetValueAt(0)) {
        return v->str();
      }
    }
    return {};
  }
  if (auto map = As<ConfigMap>(item)) {
    if (auto commit = map->Get("commit")) {
      if (auto v = As<ConfigValue>(commit)) {
        return v->str();
      }
    }
    if (auto pair = map->Get("pair")) {
      if (auto pair_list = As<ConfigList>(pair)) {
        if (pair_list->size() > 0) {
          if (auto v = pair_list->GetValueAt(0)) {
            return v->str();
          }
        }
      }
    }
  }
  return {};
}

inline std::string LookupFullShapePunct(Engine* engine, int keycode) {
  if (!engine || !engine->schema()) return {};
  Config* config = engine->schema()->config();
  if (!config) return {};
  std::string key(1, static_cast<char>(keycode));
  // full_shape 优先
  if (auto m = config->GetMap("punctuator/full_shape")) {
    if (auto text = ExtractPunctDefault(m->Get(key)); !text.empty()) {
      return text;
    }
  }
  // 回退到 symbols
  if (auto m = config->GetMap("punctuator/symbols")) {
    if (auto text = ExtractPunctDefault(m->Get(key)); !text.empty()) {
      return text;
    }
  }
  return {};
}

inline bool IsSpaceKey(int keycode) {
  return (keycode == XK_space || keycode == XK_Return || keycode == XK_KP_Enter ||
          keycode == XK_Tab || keycode == XK_ISO_Enter || keycode == XK_KP_Space);
}

inline std::string AddSpace(int keycode) {
  return " " + std::string(1, static_cast<char>(keycode));
}

inline bool IsDelete(const KeyEvent& key_event) {
  const auto keycode = key_event.keycode();
  if (keycode == XK_BackSpace || keycode == XK_Delete || keycode == XK_KP_Delete ||
      keycode == XK_Clear) {
    return true;
  }
  if (!key_event.ctrl()) {
    return false;
  }
  return (keycode == XK_h || keycode == XK_k);
}

inline bool IsNavigating(const KeyEvent& key_event) {
  const auto keycode = key_event.keycode();
  if ((keycode >= XK_Left && keycode <= XK_Down) || (keycode == XK_Tab) ||
      (keycode == XK_ISO_Left_Tab)) {
    return true;
  }
  if (!key_event.ctrl()) {
    return false;
  }
  return (keycode == XK_a || keycode == XK_b || keycode == XK_e || keycode == XK_f ||
          keycode == XK_n || keycode == XK_p);
}

inline bool IsPunctString(const std::string latest_text) {
  if (latest_text.size() != 1) {
    return false;
  }
  const auto& c = latest_text.front();
  DLOG(INFO) << "[AutoSpacer] IsPunctString: c=" << std::showbase << std::hex
             << static_cast<int>(c);
  return (c >= XK_space && c <= XK_slash) || (c >= XK_bracketleft && c <= XK_quoteleft);
}

inline bool NeedAddSpace(Context* ctx, const KeyEvent& key_event) {
  const auto& history = ctx->commit_history();
  const auto& latest_text = history.latest_text();
  const auto& input = ctx->input();
  DLOG(INFO) << "[AutoSpacer] NeedAddSpace: latest_text='" << latest_text << "', input='" << input
             << "'";
  if (latest_text.empty() || input.empty()) {
    return false;
  }
  if (key_event.modifier() != 0) {
    return false;
  }
  if (input[0] == ' ' && IsPunctString(latest_text)) {
    auto strip = input.substr(1);
    ctx->set_input(strip);
    DLOG(INFO) << "strip space";
    return false;
  }
  if (input[0] != ' ' && !IsPunctString(latest_text)) {
    // 检查是否是连续的 raw/thru 英文上屏，如果是则不加空格
    if (!history.empty()) {
      const auto& last_record = history.back();
      if (last_record.type == "raw" || last_record.type == "thru") {
        // 如果上一次是直接上屏的 ASCII 内容，不加空格
        int last_char = LastAsciiCharCode(latest_text);
        if (IsAlphabetKey(last_char)) {
          DLOG(INFO) << "[AutoSpacer] NeedAddSpace: skip for consecutive raw ASCII";
          return false;
        }
      }
    }
    return true;
  }
  return false;
}

}  // namespace

AutoSpacer::AutoSpacer(const Ticket& ticket, CommitCallback on_commit)
    : CopilotPlugin<AutoSpacer>(ticket), on_commit_(std::move(on_commit)) {
  if (auto* config = engine_->schema()->config()) {
    config->GetBool("copilot/auto_spacer/enable_right_space", &enable_right_space_);
  }
}

ProcessResult AutoSpacer::HandleNumberKey(Context* ctx, const KeyEvent& key_event) const {
  const auto& keycode = key_event.keycode();
  // Not static: a function-local static would pin the first schema's page
  // size for the whole process, so switching to a schema with a different
  // page_size would mis-handle the number keys.
  const auto page_size = engine_->schema()->page_size();
  int num = keycode - XK_0;
  const auto& input = ctx->input();
  if (input.empty()) {
    return kNoop;
  }
  if (num == 0 || num > page_size) {
    // ctx->set_input(input + std::string(1, keycode));
    auto str = input + std::string(1, keycode);
    auto commit_str = NeedAddSpace(ctx, key_event) ? " " + str : str;
    engine_->CommitText(commit_str);
    if (!commit_str.empty()) ctx->commit_history().push_back({"raw", commit_str});
    ctx->Clear();
    return kAccepted;
  }
  int n_cand = -1;
  const auto& composition = ctx->composition();
  // A segment can carry no menu (e.g. the copilot placeholder before the
  // translators run), and Rime's own Segment accessors all null-check it.
  if (!composition.empty() && composition.back().menu) {
    int cand_count = composition.back().menu->candidate_count();
    if (cand_count) {
      int mod = cand_count % page_size;
      n_cand = mod == 0 ? page_size : mod;
    }
  }
  DLOG(INFO) << "Input Num=" << num << ", n_cand=" << n_cand;
  if (num > n_cand && !input.empty()) {
    auto str = input + std::string(1, keycode);
    auto commit_str = NeedAddSpace(ctx, key_event) ? " " + str : str;
    engine_->CommitText(commit_str);
    if (!commit_str.empty()) ctx->commit_history().push_back({"raw", commit_str});
    ctx->Clear();
    return kAccepted;
  }
  return kNoop;
}

bool SelectionLeavesUnconvertedInput(Context* ctx, const an<Candidate>& cand) {
  if (!ctx || !cand) {
    return false;
  }
  // Compare against the composition's own input: that is the string
  // Composition::GetCommitText() slices the unconverted tail from.
  return cand->end() < ctx->composition().input().length();
}

std::string ComputeSpaceCommitText(Context* ctx, const std::string& before,
                                   const std::string& after, bool enable_right_space) {
  // Default: commit the raw input (composing English, no candidate).
  std::string text = ctx->input();
  // When there is a selected candidate, commit the FULL composition text: all
  // selected segments concatenated (Context::GetCommitText), not just the last
  // segment's candidate. Committing only composition().back() would drop the
  // earlier selections of a long, multi-segment input so only the last
  // candidate reaches the screen.
  if (!ctx->composition().empty() && ctx->composition().back().GetSelectedCandidate()) {
    text = ctx->GetCommitText();
  }
  // Pick the spacing rules from the actual committed text, not from "is a
  // candidate selected": an ASCII candidate (e.g. an English word chosen in the
  // middle of CJK) must be spaced on both sides like raw English, whereas a CJK
  // candidate must not.
  bool content_is_ascii = IsPureAsciiText(text);
  return DecorateCommitText(text, before, after, content_is_ascii, enable_right_space);
}

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
  // auto_spacer.h for why this stops short of the Clear() that Commit() would
  // perform, and leaves it to the caller.
  ctx->commit_notifier()(ctx);
  return true;
}

// Path 1: Process with real surrounding context (completely independent)
ProcessResult AutoSpacer::ProcessWithSurroundingContext(Context* ctx, const KeyEvent& key_event,
                                                        const SurroundingText& surrounding,
                                                        const std::string& client_key) {
  const auto keycode = key_event.keycode();
  const auto& input = ctx->input();
  const bool ascii_mode = ctx->get_option("ascii_mode");
  const std::string effective_client_key = client_key.empty() ? "__default__" : client_key;
  const std::string& raw_before = surrounding.before;
  const std::string& raw_after = surrounding.after;

  auto& client_state = client_states_[effective_client_key];
  const auto& latest_text = ctx->commit_history().latest_text();
  DLOG(INFO) << "[SurroundingText]" << std::showbase << std::hex << " keycode=" << keycode << "("
             << string(1, keycode) << ")" << ", input='" << input << "'"
             << ", ascii_mode=" << ascii_mode << ", latest_text='" << latest_text << "'["
             << ctx->commit_history().back().type << "], modifier=" << key_event.modifier()
             << ", raw_before='" << raw_before << "', raw_after='" << raw_after
             << "', client_before='" << client_state.before << "', client_after='"
             << client_state.after << "'";

  // 带 Ctrl/Alt/Super 的通常是快捷键, 不走标点/输入处理. Shift 要放行, 因为
  // ASCII 标点键本身就依赖 Shift (例如 '@' = Shift+2, '#' = Shift+3).
  if (key_event.ctrl() || key_event.alt() || key_event.super() || keycode >= XK_Shift_L) {
    return kNoop;
  }

  // 非 ASCII 模式下, 强制用"全角 / 中文" 标点直接上屏, 绕过 Rime 的
  // Punctuator. 这样做的理由:
  //   1. 许多 schema (如 rime_ice / double_pinyin_flypy) 默认 full_shape =
  //      半角, 其 half_shape 映射里一些键 (如 '@' → "@", '#' → "#") 仍然
  //      是 ASCII 原字符, 导致"中文模式下按 @ 上屏英文 @" 的尴尬.
  //   2. 多形标点 (ConfigList, 如 full_shape '@' → ["＠", "☯"]) 的候选框,
  //      用户在连续输入中并不希望看到.
  // 这里直接从 schema 读取 punctuator/full_shape 下的映射, 取默认候选
  // (ConfigValue 本身 / ConfigList 第 0 项 / ConfigMap 的 commit 或 pair[0])
  // 通过 sink 直接上屏, 并写入 commit_history 为 "punct" 类型.
  // 注意: 此分支在 input empty / composing / ascii_mode 等检查之前, 所以无论
  // 当前 composing 状态如何, 只要非 ASCII 且非英文标点模式, 标点键一律走
  // 强制上屏路径 (composition 如果存在, Rime 的 Punctuator 本来也会在
  // PushInput 后自动提交上一段).
  if (input.empty() && !ascii_mode && !ctx->get_option("ascii_punct") &&
      IsAsciiPunctuationCode(keycode) && !key_event.release()) {
    std::string punct_text = LookupFullShapePunct(engine_, keycode);
    DLOG(INFO) << "[AutoSpacer] force-punct key='" << static_cast<char>(keycode) << "' (0x"
               << std::hex << keycode << std::dec << ") modifier=0x" << std::hex
               << key_event.modifier() << std::dec << " -> '" << punct_text << "'";
    if (!punct_text.empty()) {
      engine_->sink()(punct_text);
      ctx->commit_history().push_back({"punct", punct_text});
      return kAccepted;
    }
    // 该键在 full_shape / symbols 里都没有中文映射 (如纯英文符号):
    // 让后续默认流程处理.
  }

  // ASCII mode: direct typing, only check left boundary.
  if (ascii_mode) {
    if (!input.empty()) {
      return kNoop;
    }
    if (!IsAlphabetKey(keycode)) {
      return kNoop;
    }
    if (NeedSpaceBefore(raw_before, true)) {
      auto commit_str = AddSpace(keycode);
      engine_->CommitText(commit_str);
      if (!commit_str.empty()) ctx->commit_history().push_back({"raw", commit_str});
      return kAccepted;
    }
    return kNoop;
  }

  // Non-ASCII mode: cache boundary whenever not composing.
  if (input.empty()) {
    client_state.before = raw_before;
    client_state.after = raw_after;
    if (IsLetterKey(keycode)) {
      const bool after_period = !ascii_mode && (latest_text == "。" || latest_text == ".");
      if (after_period) {
        ctx->set_input(std::string(1, static_cast<char>(keycode)));
        return kAccepted;
      }
    }
    return kNoop;
  }

  const std::string before = client_state.before;
  const std::string after = client_state.after.empty() ? raw_after : client_state.after;
  DLOG(INFO) << "[SurroundingText] " << "Before='" << before << "', After='" << after << "'";

  // Keep behavior consistent with ProcessWithCommitHistory:
  // after Chinese full stop, force-refresh preedit on first letter key.
  if (IsLetterKey(keycode)) {
    const bool after_period = !ascii_mode && (latest_text == "。" || latest_text == ".");
    if (!input.empty() || after_period) {
      ctx->set_input(input + std::string(1, static_cast<char>(keycode)));
      return kAccepted;
    }
  }

  // Enter: raw commit as ASCII.
  if (keycode == XK_Return || keycode == XK_KP_Enter) {
    auto decorated_text = DecorateCommitText(input, before, after, true, enable_right_space_);
    engine_->CommitText(decorated_text);
    if (!decorated_text.empty()) ctx->commit_history().push_back({"raw", decorated_text});
    // This bypasses Context::Commit() -- commit_notifier_ never fires for it
    // -- so Copilot::OnCommit can never warm the scorer for it. This is the
    // substitute; see the constructor comment (auto_spacer.h). `false`: this
    // is a bail-out -- the user committed raw ASCII input, discarding
    // whatever candidate the composition still shows as highlighted.
    if (on_commit_) on_commit_(ctx, decorated_text, false);
    // No NotifyForLearning here, deliberately. `false` above means the user
    // discarded every candidate and committed raw ASCII; the composition
    // still shows a highlighted one, and Memory::ProcessSegmentOnCommit
    // memorises exactly that (memory.cc:111-126). Learning here would train
    // the user dictionary on the answers its owner turned down.
    ctx->Clear();
    client_state.before.clear();
    client_state.after.clear();
    return kAccepted;
  }

  // Space: commit the whole composition (usually CJK).
  if (keycode == XK_space) {
    // ...unless the highlighted candidate converts only part of the input, in
    // which case Space means "confirm this segment, keep composing the rest".
    // Let Rime's Selector do that; committing here would flush the tail raw.
    if (!ctx->composition().empty() &&
        SelectionLeavesUnconvertedInput(ctx, ctx->composition().back().GetSelectedCandidate())) {
      return kNoop;
    }
    auto decorated_text = ComputeSpaceCommitText(ctx, before, after, enable_right_space_);
    engine_->CommitText(decorated_text);
    // `true`: this commits the actual selected candidate(s)
    // (ComputeSpaceCommitText), not a bail-out -- when there is no selected
    // candidate at all it falls back to raw input, but BuildCommitEvents
    // already skips a segment with no GetSelectedCandidate(), so that case
    // cannot be misreported either way.
    //
    // Before NotifyForLearning: this callback's telemetry reads
    // GetSelectedCandidate() off the live composition (see the CommitCallback
    // contract in auto_spacer.h), and NotifyForLearning does not clear.
    if (on_commit_) on_commit_(ctx, decorated_text, true);
    // Space is the dominant commit key in this configuration, so this is the
    // site that matters most for learning. `true` above is exactly the
    // predicate learning needs: on a bail-out the composition still shows a
    // highlighted candidate the user rejected, and Memory::OnCommit would
    // memorise it (memory.cc:111-126). Still before the history push and the
    // Clear() below: Memory::OnCommit also reads the live composition.
    NotifyForLearning(ctx);
    // After NotifyForLearning, before Clear(): NotifyForLearning does not
    // clear (deliberately -- see its declaration comment), so the decorated
    // record pushed here is still what Clear()'s update_notifier_ sees at
    // back() -- not the undecorated per-segment record
    // ConcreteEngine::OnCommit would otherwise have left there.
    if (!decorated_text.empty()) ctx->commit_history().push_back({"raw", decorated_text});
    ctx->Clear();
    client_state.before.clear();
    client_state.after.clear();
    return kAccepted;
  }

  if (!IsNumKey(keycode)) {
    return kNoop;
  }

  // Not static: a function-local static would pin the first schema's page
  // size for the whole process, so switching to a schema with a different
  // page_size would mis-handle the number keys.
  const auto page_size = engine_->schema()->page_size();
  const int num = keycode - XK_0;

  // Number key fallback to raw ASCII commit.
  auto commit_raw = [&]() {
    std::string raw = input + std::string(1, static_cast<char>(keycode));
    auto decorated_text = DecorateCommitText(raw, before, after, true, enable_right_space_);
    engine_->CommitText(decorated_text);
    if (!decorated_text.empty()) ctx->commit_history().push_back({"raw", decorated_text});
    // `false`: a bail-out, same reasoning as Enter above -- num was out of
    // range or the candidate at that slot did not exist, so whatever the
    // composition still shows highlighted was never committed.
    if (on_commit_) on_commit_(ctx, decorated_text, false);
    // No NotifyForLearning here, deliberately. `false` above means the user
    // discarded every candidate and committed raw ASCII; the composition
    // still shows a highlighted one, and Memory::ProcessSegmentOnCommit
    // memorises exactly that (memory.cc:111-126). Learning here would train
    // the user dictionary on the answers its owner turned down.
    ctx->Clear();
    client_state.before.clear();
    client_state.after.clear();
    return kAccepted;
  };

  if (num == 0 || num > page_size || ctx->composition().empty()) {
    return commit_raw();
  }

  auto& seg = ctx->composition().back();
  const size_t page_no = seg.selected_index / page_size;
  const size_t idx = page_no * page_size + static_cast<size_t>(num - 1);
  auto cand = seg.GetCandidateAt(idx);
  if (!cand) {
    return commit_raw();
  }

  // Same as Space: a candidate covering only a prefix of the input means the
  // composition continues, so defer the selection to Rime instead of
  // committing the unconverted tail along with it.
  if (SelectionLeavesUnconvertedInput(ctx, cand)) {
    return kNoop;
  }

  // Make the number-chosen candidate the current selection, then commit the
  // whole composition through the SAME path as the Space key (which is correct).
  // Deferring to Rime here committed the candidate with no auto-spacing, so an
  // English word chosen in the middle of CJK via a number key lost its trailing
  // space (`你 test好`). Selecting + committing here mirrors Space, so both
  // behave identically. ComputeSpaceCommitText concatenates all selected
  // segments (Context::GetCommitText), so multi-segment input is preserved.
  seg.selected_index = idx;
  auto decorated_text = ComputeSpaceCommitText(ctx, before, after, enable_right_space_);
  engine_->CommitText(decorated_text);
  // `true`: the number key just selected `cand` above, so this genuinely
  // commits the candidate the user picked -- not a bail-out. Before
  // NotifyForLearning for the same reason as the Space site: this callback
  // reads the live composition, and NotifyForLearning does not clear it.
  if (on_commit_) on_commit_(ctx, decorated_text, true);
  // Still before the history push and Clear() below: Memory::OnCommit also
  // reads the live composition.
  NotifyForLearning(ctx);
  // After NotifyForLearning, before Clear(): so AutoSpacer's decorated record
  // is still what's at back() when Clear()'s update_notifier_ fires, not the
  // undecorated per-segment one ConcreteEngine::OnCommit would otherwise have
  // left there.
  if (!decorated_text.empty()) ctx->commit_history().push_back({"raw", decorated_text});
  ctx->Clear();
  client_state.before.clear();
  client_state.after.clear();
  return kAccepted;
}

// Path 2: Process with commit_history (original logic)
ProcessResult AutoSpacer::ProcessWithCommitHistory(Context* ctx, const KeyEvent& key_event) {
  const auto keycode = key_event.keycode();

  const auto& latest_text = ctx->commit_history().latest_text();

  const auto& input = ctx->input();
  const bool ascii_mode = ctx->get_option("ascii_mode");
  DLOG(INFO) << "[AutoSpacer] " << std::showbase << std::hex << " keycode=" << keycode << "("
             << string(1, keycode) << ")" << ", input='" << input << "'"
             << ", ascii_mode=" << ascii_mode << ", latest_text='" << latest_text << "'["
             << ctx->commit_history().back().type << "], modifier=" << key_event.modifier();

  if (IsDelete(key_event)) {
    if (input.empty()) {
      DLOG(INFO) << "[SKIP] 按键是 BackSpace 键，输入为空, 清除输入";
      ctx->commit_history().clear();
    }
    return kNoop;
  }
  if (IsNavigating(key_event)) {
    DLOG(INFO) << "[SKIP] 按键是导航键，跳过处理: " << keycode;
    if (!ctx->HasMenu()) {
      ctx->commit_history().clear();
    }
    return kNoop;
  }

  // TODO:(@dongpeng) .[中文]
  if (IsLetterKey(keycode)) {
    const bool after_period = !ascii_mode && (latest_text == "。" || latest_text == ".");
    if ((!input.empty() && input[0] == ' ') || after_period) {
      DLOG(INFO) << "[ADD] 强制刷新";
      ctx->set_input(input + std::string(1, keycode));
      return kAccepted;
    }
  }

  if (IsNumKey(keycode)) {
    return HandleNumberKey(ctx, key_event);
  }

  if (latest_text.empty()) {
    DLOG(INFO) << "[SKIP] 历史为空";
    return kNoop;
  }

  if (IsChinesePunctuation(latest_text)) {
    DLOG(INFO) << "[SKIP] 上次输入为中文标点: '" << latest_text << "'";
    return kNoop;
  }

  if (IsSpaceKey(keycode)) {
    DLOG(INFO) << "[SKIP] 按键是空格键，跳过处理: " << keycode;
    if (keycode == XK_Return || keycode == XK_KP_Enter) {
      if (NeedAddSpace(ctx, key_event)) {
        DLOG(INFO) << "[ADD] Add space for Enter";
        ctx->set_input(" " + input);
      }
      ctx->commit_history().push_back({"thru", std::string(1, keycode)});
    }
    return kNoop;
  }

  if (IsModifierPunctKey(keycode)) {
    // XK_comma 和 XK_period 自动加入了, 不知道为什么
    ctx->commit_history().push_back({"thru", std::string(1, keycode)});
    return kNoop;
  }

  if (key_event.modifier()) {
    DLOG(INFO) << "[SKIP] 修饰键，跳过处理: " << keycode;
    return kNoop;
  }

  const bool is_alphabet = IsAlphabetKey(keycode);
  if (!is_alphabet) {
    DLOG(INFO) << "[SKIP] 非 Alphabet";
    return kNoop;
  }

  const bool has_input = !ctx->input().empty();
  if (!has_input && latest_text != " ") {
    int last_ascii_char = LastAsciiCharCode(latest_text);
    bool is_thru_commit = false;

    // 检查是否是回车直接上屏的英文（type = "thru")
    // 如果是，不应该添加空格，因为这是连续的英文输入
    const auto& history = ctx->commit_history();
    if (!history.empty()) {
      const auto& last_record = history.back();
      // "thru" 类型表示按键直接上屏（如回车键让拼音直接上屏）
      if (last_record.type == "thru" || last_record.type == "raw") {
        DLOG(INFO) << "[SKIP] 最后输入为 thru, 跳过";
        is_thru_commit = true;
      }
    }

    const bool is_space_punct = IsAsciiPunctuationCode(last_ascii_char) && last_ascii_char != '`';
    if ((IsAlphabetKey(last_ascii_char) || is_space_punct) && !ascii_mode) {
      // 如果是回车直接上屏的英文，不添加空格
      if (is_thru_commit && IsAlphabetKey(last_ascii_char)) {
        DLOG(INFO) << "[SKIP] previous was thru/raw commit";
        return kNoop;
      }
      DLOG(INFO) << "[ADD] 为**中文**添加空格 (from history): " << string(1, keycode);
      ctx->set_input(AddSpace(keycode));
      return kAccepted;
    }

    if (last_ascii_char < 0 && ascii_mode) {
      DLOG(INFO) << "[ADD] 为 ascii mode 添加空格 (from history)";
      auto commit_str = AddSpace(keycode);
      engine_->CommitText(commit_str);
      if (!commit_str.empty()) ctx->commit_history().push_back({"raw", commit_str});
      return kAccepted;
    }
  }

  return kNoop;
}

ProcessResult AutoSpacer::Process(Context* ctx, const KeyEvent& key_event) {
  // Try to get real surrounding context first
  auto surrounding = GetSurroundingContext();

  // Path 1: Use real surrounding context (completely independent)
  if (surrounding.has_value()) {
    return ProcessWithSurroundingContext(ctx, key_event, surrounding.value(),
                                         surrounding->client_key);
  }

  // Path 2: Fallback to commit_history (original logic)
  return ProcessWithCommitHistory(ctx, key_event);
}

ProcessResult AutoSpacer::Process(const KeyEvent& key_event) {
  if (!engine_ || key_event.release()) {
    return kNoop;
  }
  auto* ctx = engine_->context();
  if (!ctx) {
    return kNoop;
  }
  return Process(ctx, key_event);
}

}  // namespace rime
