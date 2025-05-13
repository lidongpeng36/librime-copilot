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

ProcessResult AutoSpacer::Process(const KeyEvent& key_event) {
  if (!engine_ || key_event.release()) {
    return kNoop;
  }
  auto* ctx = engine_->context();
  if (!ctx) {
    return kNoop;
  }
  const auto keycode = key_event.keycode();

  const bool ascii_mode = ctx->get_option("ascii_mode");
  const bool is_alphabet = IsAlphabetKey(keycode);
  const bool has_input = !ctx->input().empty();

  // LOG(INFO) << "[AutoSpacer] has_space_=" << has_space_ << std::showbase << std::hex
  //           << ", keycode=" << keycode << ", input='" << ctx->input() << "'"
  //           << ", prev_ascii_mode=" << ascii_mode_ << ", ascii_mode=" << ascii_mode;

  bool has_space = has_space_;
  if (IsSpaceKey(keycode)) {
    // DLOG(INFO) << "HasMenu: " << ctx->HasMenu() << ", IsComposing: " << ctx->IsComposing()
    //            << ", Input: '" << ctx->input() << "'";
    has_space = !has_input;
  } else if (is_alphabet || keycode == XK_BackSpace || keycode == XK_Delete) {
    has_space = false;
  }

  if (ascii_mode_ == ascii_mode || has_space_ || !is_alphabet || count_ == 0) {
    // LOG(INFO) << "Skip [HasSpace/NoAlpha]: (has_space_ || !is_alphabet || count_==0) = "
    //           << has_space_ << " || " << !is_alphabet << " || " << count_ << " == 0]";
    if (ascii_mode_ == ascii_mode) {
      count_ += (is_alphabet && !has_input) || (IsSelectionKey(keycode) && has_input);
    } else {
      count_ = 0;
    }
    ascii_mode_ = ascii_mode;
    has_space_ = has_space;
    return kNoop;
  }

  // LOG(INFO) << "MODE CHANGE: ascii_mode=" << ascii_mode << ", count_=" << count_;
  count_ = 0;
  bool change_to_ascii_mode = (!ascii_mode_ && ascii_mode);
  bool change_from_ascii_mode = (ascii_mode_ && !ascii_mode);
  ascii_mode_ = ascii_mode;
  has_space_ = has_space;
  if (change_to_ascii_mode) {
    if (!ctx->commit_history().empty() && IsLastCharAlnum(ctx->commit_history().back().text)) {
      // LOG(INFO) << "[AutoSpacerProcessor]: last is ascii, skip";
      return kNoop;
    }
    engine_->CommitText(AddSpace(keycode));
  } else if (change_from_ascii_mode) {
    ctx->set_input(AddSpace(keycode));
  }
  return kAccepted;
  // return kNoop;
}

}  // namespace rime
