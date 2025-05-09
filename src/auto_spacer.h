#pragma once

#include "copilot_plugin.h"

namespace rime {

class AutoSpacer : public CopilotPlugin<AutoSpacer> {
 public:
  using CopilotPlugin<AutoSpacer>::CopilotPlugin;

  ProcessResult Process(const KeyEvent& key_event);

 private:
  bool ascii_mode_ = false;
  bool has_space_ = false;

  int count_ = 0;
};

}  // namespace rime
