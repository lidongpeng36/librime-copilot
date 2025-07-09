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
  bool ascii_mode_ = false;
};

}  // namespace rime
