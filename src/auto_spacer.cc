#include "auto_spacer.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

#include <rime/menu.h>
#include <rime/schema.h>
#include <cctype>

#include "ime_bridge.h"

namespace rime {

namespace {

// 将 UTF-8 字符串转为 Unicode 码点
inline uint32_t Utf8ToCodepoint(const std::string& s) {
  uint32_t code = 0;
  const unsigned char* bytes = reinterpret_cast<const unsigned char*>(s.data());
  size_t len = s.size();

  if (len == 1) {
    code = bytes[0];
  } else if (len == 2) {
    code = ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
  } else if (len == 3) {
    code = ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
  } else if (len == 4) {
    code = ((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | ((bytes[2] & 0x3F) << 6) |
           (bytes[3] & 0x3F);
  }
  return code;
}

// 判断是否是中文标点符号
inline bool IsChinesePunctuation(const std::string& s) {
  if (s.empty() || s.size() > 4) return false;

  uint32_t cp = Utf8ToCodepoint(s);
  return (cp >= 0x3000 && cp <= 0x303F) ||  // CJK 符号和标点
         (cp >= 0xFF00 && cp <= 0xFFEF);    // 全角标点等
}

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

inline bool IsSpaceKey(int keycode) {
  return (keycode == XK_space || keycode == XK_Return || keycode == XK_KP_Enter ||
          keycode == XK_Tab || keycode == XK_ISO_Enter || keycode == XK_KP_Space);
}

inline std::string AddSpace(int keycode) {
  return " " + std::string(1, static_cast<char>(keycode));
}

inline int LastAsciiCharCode(const std::string& str) {
  if (str.empty()) return -1;

  int i = static_cast<int>(str.size()) - 1;
  // 回溯查找 UTF-8 字符的起始字节
  while (i >= 0 && (static_cast<uint8_t>(str[i]) & 0xC0) == 0x80) {
    --i;
  }
  if (i < 0) return -1;  // 非法 UTF-8 序列

  uint8_t c = static_cast<uint8_t>(str[i]);
  if (c < 0x80) {
    return c;  // 是 ASCII 字符，直接返回其数值
  }

  return -1;  // 非 ASCII 字符
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

AutoSpacer::AutoSpacer(const Ticket& ticket) : CopilotPlugin<AutoSpacer>(ticket) {
  if (auto* config = engine_->schema()->config()) {
    config->GetBool("copilot/auto_spacer/enable_right_space", &enable_right_space_);
  }
}

ProcessResult AutoSpacer::HandleNumberKey(Context* ctx, const KeyEvent& key_event) const {
  const auto& keycode = key_event.keycode();
  static const auto page_size = engine_->schema()->page_size();
  int num = keycode - XK_0;
  const auto& input = ctx->input();
  if (input.empty()) {
    return kNoop;
  }
  if (num == 0 || num > page_size) {
    // ctx->set_input(input + std::string(1, keycode));
    auto str = input + std::string(1, keycode);
    engine_->CommitText(NeedAddSpace(ctx, key_event) ? " " + str : str);
    ctx->Clear();
    return kAccepted;
  }
  int n_cand = -1;
  const auto& composition = ctx->composition();
  if (!composition.empty()) {
    int cand_count = composition.back().menu->candidate_count();
    if (cand_count) {
      int mod = cand_count % page_size;
      n_cand = mod == 0 ? page_size : mod;
    }
  }
  DLOG(INFO) << "Input Num=" << num << ", n_cand=" << n_cand;
  if (num > n_cand && !input.empty()) {
    auto str = input + std::string(1, keycode);
    engine_->CommitText(NeedAddSpace(ctx, key_event) ? " " + str : str);
    ctx->Clear();
    return kAccepted;
  }
  return kNoop;
}

std::optional<SurroundingText> AutoSpacer::GetSurroundingText() const {
#ifdef __APPLE__
  // Priority 1: IMK Client (macOS system query)
  if (auto context = GetIMKSurroundingText()) {
    DLOG(INFO) << "[AutoSpacer] Using IMK context: before='" << context->before << "', after='"
               << context->after << "'";
    return context;
  }
#endif

  // Priority 2: ImeBridge (clients like Neovim), used only when IMK has no context.
  if (auto context = ImeBridgeServer::Instance().GetActiveContext()) {
    DLOG(INFO) << "[AutoSpacer] Using ImeBridge context: before='" << context->before
               << "', after='" << context->after << "'";
    return context;
  }

  // Priority 3: fallback to commit_history
  return std::nullopt;
}

// Helper: Get last UTF-8 character from string
static std::string GetLastUtf8Char(const std::string& str) {
  if (str.empty()) return "";

  size_t len = str.size();
  size_t start = len - 1;

  // Find start of last UTF-8 character
  while (start > 0 && (static_cast<uint8_t>(str[start]) & 0xC0) == 0x80) {
    start--;
  }

  return str.substr(start);
}

static std::string GetFirstUtf8Char(const std::string& str) {
  if (str.empty()) return "";
  size_t len = str.size();
  size_t end = 1;
  unsigned char c = static_cast<unsigned char>(str[0]);
  if ((c & 0x80) == 0x00) {
    end = 1;
  } else if ((c & 0xE0) == 0xC0) {
    end = 2;
  } else if ((c & 0xF0) == 0xE0) {
    end = 3;
  } else if ((c & 0xF8) == 0xF0) {
    end = 4;
  }
  if (end > len) {
    end = len;
  }
  return str.substr(0, end);
}

static bool IsAsciiRightPunctCode(int c) {
  return c == '.' || c == ',' || c == '>' || c == ']' || c == ')' || c == '}' || c == '!' ||
         c == '?';
}

static bool IsAsciiRightPunctCodeForAsciiInput(int c) {
  // Keep punctuation-triggered spacing, but exclude '.' per latest behavior.
  return c == ',' || c == '>' || c == ']' || c == ')' || c == '}' || c == '!' || c == '?';
}

static bool IsAsciiAlphaNumCode(int c) {
  return c >= 0 && c < 0x80 && std::isalnum(static_cast<unsigned char>(c));
}

static bool IsChinesePunctuationChar(const std::string& s) {
  return !s.empty() && IsChinesePunctuation(s);
}

static bool IsCjkNonPunctuationChar(const std::string& s) {
  if (s.empty() || IsChinesePunctuationChar(s)) {
    return false;
  }
  return LastAsciiCharCode(s) < 0;
}

static bool IsPureAsciiText(const std::string& s) {
  if (s.empty()) {
    return false;
  }
  for (unsigned char c : s) {
    if (c >= 0x80) {
      return false;
    }
  }
  return true;
}

static bool NeedSpaceBefore(const std::string& before, bool content_is_ascii) {
  std::string ch = GetLastUtf8Char(before);
  if (ch.empty() || IsChinesePunctuationChar(ch) || ch == " ") {
    return false;
  }
  int ascii = LastAsciiCharCode(ch);
  if (content_is_ascii) {
    return IsCjkNonPunctuationChar(ch) || IsAsciiRightPunctCodeForAsciiInput(ascii);
  }
  return IsAsciiAlphaNumCode(ascii) || IsAsciiRightPunctCode(ascii);
}

static bool NeedSpaceAfter(const std::string& after, bool content_is_ascii) {
  std::string ch = GetFirstUtf8Char(after);
  if (ch.empty() || IsChinesePunctuationChar(ch)) {
    return false;
  }
  int ascii = LastAsciiCharCode(ch);
  if (content_is_ascii) {
    return IsCjkNonPunctuationChar(ch);
  }
  return IsAsciiAlphaNumCode(ascii);
}

static std::string DecorateCommitText(const std::string& text, const std::string& before,
                                      const std::string& after, bool content_is_ascii,
                                      bool enable_space_after) {
  if (text.empty()) {
    return text;
  }
  size_t begin = 0;
  size_t end = text.size();
  while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) {
    ++begin;
  }
  while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) {
    --end;
  }
  std::string result = text.substr(begin, end - begin);
  if (result.empty() || IsChinesePunctuationChar(result)) {
    return result;
  }

  if (NeedSpaceBefore(before, content_is_ascii) && (result.empty() || result.front() != ' ')) {
    result = " " + result;
  }
  if (enable_space_after && NeedSpaceAfter(after, content_is_ascii) &&
      (result.empty() || result.back() != ' ')) {
    result += " ";
  }
  return result;
}

static std::string ResolveBoundaryBefore(Context* ctx, const std::string& surrounding_before) {
  (void)ctx;
  // In surrounding-context path, trust client-provided boundary directly.
  // Empty before means true line/file start; do not fall back to history.
  return surrounding_before;
}

// Path 1: Process with real surrounding context (completely independent)
ProcessResult AutoSpacer::ProcessWithSurroundingContext(Context* ctx, const KeyEvent& key_event,
                                                        const SurroundingText& surrounding,
                                                        const std::string& client_key) {
  const auto keycode = key_event.keycode();
  const auto& input = ctx->input();
  const bool ascii_mode = ctx->get_option("ascii_mode");
  const std::string effective_client_key = client_key.empty() ? "__default__" : client_key;
  const std::string boundary_before_now = ResolveBoundaryBefore(ctx, surrounding.before);
  auto& client_state = client_states_[effective_client_key];
  const std::string boundary_after_now = surrounding.after;

  const auto& latest_text = ctx->commit_history().latest_text();
  DLOG(INFO) << "[SurroundingText] " << std::showbase << std::hex << " keycode=" << keycode << "("
             << string(1, keycode) << ")" << ", input='" << input << "'"
             << ", ascii_mode=" << ascii_mode << ", latest_text='" << latest_text << "'["
             << ctx->commit_history().back().type << "], modifier=" << key_event.modifier()
             << ", before='" << surrounding.before << "', after='" << surrounding.after << "'";

  if (key_event.modifier() != 0 || keycode >= XK_Shift_L) {
    return kNoop;
  }

  // ASCII mode: direct typing, only check left boundary.
  if (ascii_mode) {
    if (!input.empty()) {
      return kNoop;
    }
    if (!IsAlphabetKey(keycode)) {
      return kNoop;
    }
    if (NeedSpaceBefore(boundary_before_now, true)) {
      engine_->CommitText(AddSpace(keycode));
      return kAccepted;
    }
    return kNoop;
  }

  // Non-ASCII mode: cache boundary whenever not composing.
  if (input.empty()) {
    client_state.context_before_composition = boundary_before_now;
    client_state.context_after_composition = boundary_after_now;
    return kNoop;
  }

  const std::string before = client_state.context_before_composition.empty()
                                 ? boundary_before_now
                                 : client_state.context_before_composition;
  const std::string after = client_state.context_after_composition.empty()
                                ? boundary_after_now
                                : client_state.context_after_composition;

  // Keep behavior consistent with ProcessWithCommitHistory:
  // after Chinese full stop, force-refresh preedit on first letter key.
  if (IsLetterKey(keycode)) {
    const bool after_period = !ascii_mode && (latest_text == "。" || latest_text == ".");
    if ((!input.empty()) || after_period) {
      ctx->set_input(input + std::string(1, static_cast<char>(keycode)));
      return kAccepted;
    }
  }

  // Enter: raw commit as ASCII.
  if (keycode == XK_Return || keycode == XK_KP_Enter) {
    engine_->CommitText(DecorateCommitText(input, before, after, true, enable_right_space_));
    ctx->Clear();
    client_state.context_before_composition.clear();
    client_state.context_after_composition.clear();
    return kAccepted;
  }

  // Space: commit current selected candidate (usually CJK).
  if (keycode == XK_space) {
    std::string text = input;
    bool content_is_ascii = true;
    if (!ctx->composition().empty()) {
      auto cand = ctx->composition().back().GetSelectedCandidate();
      if (cand) {
        text = cand->text();
        content_is_ascii = false;
      }
    }
    engine_->CommitText(
        DecorateCommitText(text, before, after, content_is_ascii, enable_right_space_));
    ctx->Clear();
    client_state.context_before_composition.clear();
    client_state.context_after_composition.clear();
    return kAccepted;
  }

  if (!IsNumKey(keycode)) {
    return kNoop;
  }

  static const auto page_size = engine_->schema()->page_size();
  const int num = keycode - XK_0;

  // Number key fallback to raw ASCII commit.
  auto commit_raw = [&]() {
    std::string raw = input + std::string(1, static_cast<char>(keycode));
    engine_->CommitText(DecorateCommitText(raw, before, after, true, enable_right_space_));
    ctx->Clear();
    client_state.context_before_composition.clear();
    client_state.context_after_composition.clear();
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

  const bool cand_is_ascii = IsPureAsciiText(cand->text());
  engine_->CommitText(
      DecorateCommitText(cand->text(), before, after, cand_is_ascii, enable_right_space_));
  ctx->Clear();
  client_state.context_before_composition.clear();
  client_state.context_after_composition.clear();
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

    const bool is_space_punct =
        IsAsciiPunctuationCode(last_ascii_char) && last_ascii_char != '`';
    if ((IsAlphabetKey(last_ascii_char) || is_space_punct) &&
        !ascii_mode) {
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
      engine_->CommitText(AddSpace(keycode));
      return kAccepted;
    }
  }

  return kNoop;
}

ProcessResult AutoSpacer::Process(Context* ctx, const KeyEvent& key_event) {
  // Try to get real surrounding context first
  auto surrounding = GetSurroundingText();

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
