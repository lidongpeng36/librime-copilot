#include "utils.h"

#ifdef _WIN32
#include <windows.h>
#elif __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/ps/IOPSKeys.h>
#include <IOKit/ps/IOPowerSources.h>
#elif __linux__
#include <fstream>
#endif

namespace copilot {
// Returns true if connected to AC power, false if on battery
bool IsACPowerConnected() {
#ifdef _WIN32
  SYSTEM_POWER_STATUS status;
  if (GetSystemPowerStatus(&status)) {
    return status.ACLineStatus == 1;
  }
  return false;

#elif __APPLE__
  CFTypeRef power_info = IOPSCopyPowerSourcesInfo();
  CFArrayRef power_sources = IOPSCopyPowerSourcesList(power_info);

  bool is_ac_power = false;
  if (CFArrayGetCount(power_sources) > 0) {
    CFDictionaryRef power_source =
        IOPSGetPowerSourceDescription(power_info, CFArrayGetValueAtIndex(power_sources, 0));
    if (power_source) {
      CFStringRef power_state =
          (CFStringRef)CFDictionaryGetValue(power_source, CFSTR(kIOPSPowerSourceStateKey));
      is_ac_power =
          (CFStringCompare(power_state, CFSTR(kIOPSACPowerValue), 0) == kCFCompareEqualTo);
    }
  }

  CFRelease(power_sources);
  CFRelease(power_info);
  return is_ac_power;

#elif __linux__
  std::ifstream file("/sys/class/power_supply/AC/online");
  if (!file.is_open()) {
    return false;  // Assume battery if unable to read
  }
  int status = 0;
  file >> status;
  return status == 1;

#else
  // Unsupported platform
  return false;
#endif
}

}  // namespace copilot
