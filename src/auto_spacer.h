#pragma once

#include "copilot_plugin.h"

namespace rime {

class Context;
class AutoSpacer : public CopilotPlugin<AutoSpacer> {
 public:
  using CopilotPlugin<AutoSpacer>::CopilotPlugin;

  ProcessResult Process(const KeyEvent& key_event);

 private:
  ProcessResult Process(Context* ctx, const KeyEvent& key_event);

  ProcessResult HandleNumberKey(Context* ctx, const int keycode) const;
  bool ascii_mode_ = false;
  int keycode_ = 0;
  std::string input_;
};

}  // namespace rime
