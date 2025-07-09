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

inline bool IsLastCharAlnum(const std::string& str) {
  if (str.empty()) return false;

  // 从最后一个字节开始向前查找 UTF-8 字符的起始字节
  int i = static_cast<int>(str.size()) - 1;
  // 找到首字节标志 (最高位不为 10 的字节)
  while (i >= 0 && (static_cast<unsigned char>(str[i]) & 0xC0) == 0x80) {
    --i;
  }
  if (i < 0) return false;  // 非法 UTF-8 序列

  unsigned char c = static_cast<unsigned char>(str[i]);
  if ((c & 0x80) != 0) return false;  // 非 ASCII 字符

  // 判断是否为字母或数字
  return (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || ('0' <= c && c <= '9'));
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
    bool is_last_alnum = IsLastCharAlnum(latest_text);
    if (is_last_alnum && !ascii_mode) {
      // LOG(INFO) << "为**中文**添加空格";
      ctx->set_input(AddSpace(keycode));
      return kAccepted;
    }

    if (!is_last_alnum && ascii_mode) {
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
  const bool ascii_mode = ctx->get_option("ascii_mode");
  auto ret = Process(ctx, key_event);
  ascii_mode_ = ascii_mode;
  return ret;
}

}  // namespace rime
