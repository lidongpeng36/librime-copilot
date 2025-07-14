#include "auto_spacer.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

#include <rime/menu.h>
#include <rime/schema.h>

namespace rime {

namespace {

inline bool IsNumKey(int keycode) { return (keycode >= XK_0 && keycode <= XK_9); }

inline bool IsLetterKey(int keycode) {
  return (keycode >= XK_a && keycode <= XK_z) || (keycode >= XK_A && keycode <= XK_Z);
}

inline bool IsAlphabetKey(int keycode) { return (IsNumKey(keycode) || IsLetterKey(keycode)); }

inline bool IsPunctKey(int keycode) {
  return keycode == XK_period || keycode == XK_comma || keycode == XK_colon;
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

}  // namespace

ProcessResult AutoSpacer::HandleNumberKey(Context* ctx, const int keycode) const {
  static const auto page_size = engine_->schema()->page_size();
  int num = keycode - XK_0;
  const auto& input = ctx->input();
  if (input.empty()) {
    return kNoop;
  }
  if (num == 0 || num > page_size) {
    // ctx->set_input(input + std::string(1, keycode));
    engine_->CommitText(input + std::string(1, keycode));
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
    engine_->CommitText(input + std::string(1, keycode));
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
  if (input[0] == ' ' && IsLetterKey(keycode)) {
    DLOG(INFO) << "强制刷新";
    ctx->set_input(input + std::string(1, keycode));
    return kAccepted;
  }

  if (IsNumKey(keycode)) {
    return HandleNumberKey(ctx, keycode);
  }

  if (latest_text.empty()) {
    return kNoop;
  }

  if (IsSpaceKey(keycode)) {
    DLOG(INFO) << "按键是空格键，跳过处理: " << keycode;
    if (keycode == XK_Return || keycode == XK_KP_Enter) {
      ctx->commit_history().push_back({"thru", std::string(1, keycode)});
    }
    return kNoop;
  }

  if (IsSpaceKey(keycode_)) {
    // LOG(INFO) << "上一个按键是空格键，跳过处理: " << keycode;
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
    if ((IsAlphabetKey(last_ascii_char) || IsPunctKey(last_ascii_char)) && !ascii_mode) {
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
