#pragma once

#include <cstdint>
#include <functional>

namespace copilot {

bool IsACPowerConnected();

// Handle identifying a registered power-change callback. 0 = not registered.
using PowerChangeToken = uint64_t;

// The monitor is a process-wide singleton that outlives its subscribers, so a
// callback capturing `this` MUST be removed in the owner's destructor —
// otherwise the next plug/unplug calls into freed memory.
PowerChangeToken RegisterPowerChange(std::function<void(bool /* is_ac_power */)> callback);
void UnregisterPowerChange(PowerChangeToken token);

}  // namespace copilot
