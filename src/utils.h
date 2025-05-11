#pragma once

#include <functional>

namespace copilot {

bool IsACPowerConnected();

void RegisterPowerChange(std::function<void(bool /* is_ac_power */)> callback);

}  // namespace copilot
