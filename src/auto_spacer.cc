#include "auto_spacer.h"

#include <rime/context.h>
#include <rime/engine.h>
#include <rime/key_event.h>

namespace rime {

namespace {

inline bool IsNumKey(int keycode) { return (keycode >= XK_0 && keycode <= XK_9); }

inline bool IsAlphabetKey(int keycode) {
  return (IsNumKey(keycode) || (keycode >= XK_a && keycode <= XK_z) ||
          (keycode >= XK_A && keycode <= XK_Z));
}

inline bool IsSpaceKey(int keycode) {
  return (keycode == XK_space || keycode == XK_Return || keycode == XK_KP_Enter);
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

}  // namespace

ProcessResult AutoSpacer::Process(Context* ctx, const KeyEvent& key_event) {
  const auto keycode = key_event.keycode();

  const auto& latest_text = ctx->commit_history().latest_text();

  const bool ascii_mode = ctx->get_option("ascii_mode");
  // LOG(INFO) << "[AutoSpacer] " << std::showbase << std::hex << "  keycode=" << keycode
  //           << ", input='" << ctx->input() << "'"
  //           << ", prev_ascii_mode=" << ascii_mode_ << ", ascii_mode=" << ascii_mode
  //           << ", latest_text='" << latest_text << "', modifier=" << key_event.modifier();

  if (latest_text.empty()) {
    return kNoop;
  }

  if (IsNumKey(keycode)) {
    return kNoop;
  }

  if (IsSpaceKey(keycode)) {
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
    if (IsAlphabetKey(last_ascii_char) && !ascii_mode) {
      // LOG(INFO) << "为**中文**添加空格: " << keycode;
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
  return ret;
}

}  // namespace rime
