#include "auto_spacer.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

#include <rime/menu.h>
#include <rime/schema.h>

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

inline bool IsPunctKey(int keycode) {
  return keycode == XK_period || keycode == XK_comma || keycode == XK_colon ||
         keycode == XK_exclam || keycode == XK_question || keycode == XK_semicolon;
}

inline bool IsSpaceKey(int keycode) {
  return (keycode == XK_space || keycode == XK_Return || keycode == XK_KP_Enter ||
          keycode == XK_Tab || keycode == XK_ISO_Enter || keycode == XK_KP_Space);
}

inline bool IsSelectionKey(int keycode) { return IsNumKey(keycode) || IsSpaceKey(keycode); }

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
  if (key_event.modifier() == 0 && !input.empty()) {
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
  }
  return false;
}

}  // namespace

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

ProcessResult AutoSpacer::Process(Context* ctx, const KeyEvent& key_event) {
  const auto keycode = key_event.keycode();

  const auto& latest_text = ctx->commit_history().latest_text();

  const auto& input = ctx->input();
  const bool ascii_mode = ctx->get_option("ascii_mode");
  /*
  LOG(INFO) << "[AutoSpacer] " << std::showbase << std::hex << " keycode=" << keycode << "("
            << string(1, keycode) << ")" << ", input='" << input << "'"
            << ", prev_ascii_mode=" << ascii_mode_ << ", ascii_mode=" << ascii_mode
            << ", latest_text='" << latest_text << "', modifier=" << key_event.modifier();
  LOG(INFO) << "prev_input=" << input_ << ", input=" << input;
  LOG(INFO) << "[AutoSpacer] caret_pos=" << ctx->caret_pos()
            << ", composition=" << ctx->composition().GetDebugText();
  */

  if (IsDelete(key_event)) {
    DLOG(INFO) << "按键是 BackSpace 键，清除输入: " << keycode;
    ctx->commit_history().clear();
    return kNoop;
  }
  if (IsNavigating(key_event)) {
    DLOG(INFO) << "按键是导航键，跳过处理: " << keycode;
    if (!ctx->HasMenu()) {
      ctx->commit_history().clear();
    }
    return kNoop;
  }

  // TODO:(@dongpeng) .[中文]
  if (IsLetterKey(keycode)) {
    if ((!input.empty() && input[0] == ' ') || (!ascii_mode && latest_text == "。")) {
      DLOG(INFO) << "强制刷新";
      ctx->set_input(input + std::string(1, keycode));
      return kAccepted;
    }
  }

  if (IsNumKey(keycode)) {
    return HandleNumberKey(ctx, key_event);
  }

  if (latest_text.empty()) {
    return kNoop;
  }

  if (IsChinesePunctuation(latest_text)) {
    return kNoop;
  }

  if (IsSpaceKey(keycode)) {
    DLOG(INFO) << "按键是空格键，跳过处理: " << keycode;
    if (keycode == XK_Return || keycode == XK_KP_Enter) {
      if (NeedAddSpace(ctx, key_event)) {
        DLOG(INFO) << "[AutoSpacer] Add space for Enter";
        ctx->set_input(" " + input);
      }
      ctx->commit_history().push_back({"thru", std::string(1, keycode)});
    }
    return kNoop;
  }

  if (key_event.modifier()) {
    return kNoop;
  }

  const bool is_alphabet = IsAlphabetKey(keycode);
  if (!is_alphabet) {
    return kNoop;
  }

  const bool has_input = !ctx->input().empty();
  if (!has_input && latest_text != " ") {
    const auto last_ascii_char = LastAsciiCharCode(latest_text);
    
    // 检查是否是回车直接上屏的英文（type = "thru")
    // 如果是，不应该添加空格，因为这是连续的英文输入
    const auto& history = ctx->commit_history();
    bool is_thru_commit = false;
    if (!history.empty()) {
      const auto& last_record = history.back();
      // "thru" 类型表示按键直接上屏（如回车键让拼音直接上屏）
      if (last_record.type == "thru" || last_record.type == "raw") {
        is_thru_commit = true;
      }
    }
    
    if ((IsAlphabetKey(last_ascii_char) || IsPunctKey(last_ascii_char)) && !ascii_mode) {
      // 如果是回车直接上屏的英文，不添加空格
      if (is_thru_commit && IsAlphabetKey(last_ascii_char)) {
        DLOG(INFO) << "[AutoSpacer] Skip space: previous was thru/raw commit";
        return kNoop;
      }
      DLOG(INFO) << "为**中文**添加空格: " << string(1, keycode);
      ctx->set_input(AddSpace(keycode));
      return kAccepted;
    }

    if (last_ascii_char < 0 && ascii_mode) {
      // LOG(INFO) << "为 ascii mode 添加空格";
      engine_->CommitText(AddSpace(keycode));
      return kAccepted;
    }
  }

  return kNoop;
}

ProcessResult AutoSpacer::Process(const KeyEvent& key_event) {
  if (!engine_ || key_event.release()) {
    return kNoop;
  }
  auto* ctx = engine_->context();
  if (!ctx) {
    return kNoop;
  }
  auto ret = Process(ctx, key_event);
  ascii_mode_ = ctx->get_option("ascii_mode");
  const auto& keycode = key_event.keycode();
  if (keycode < XK_Shift_L) {
    keycode_ = keycode;
  }
  input_ = ctx->input();
  return ret;
}

}  // namespace rime
