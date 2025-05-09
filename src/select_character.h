#pragma once

#include <string>

#include "copilot_plugin.h"

namespace rime {

class SelectCharacter : public CopilotPlugin<SelectCharacter> {
 public:
  SelectCharacter(const Ticket& ticket, CopilotCallback on_accept = nullptr,
                  CopilotCallback on_noop = nullptr)
      : CopilotPlugin<SelectCharacter>(ticket, on_accept, on_noop) {
    Init(ticket);
  }

  ProcessResult Process(const KeyEvent& key_event, std::string* text);

 private:
  void Init(const Ticket& ticket);

  enum struct Selection;
  std::unordered_map<std::string, Selection> selection_map_;
};

}  // namespace rime
