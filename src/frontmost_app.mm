#ifdef __APPLE__

#include "frontmost_app.h"

#import <AppKit/AppKit.h>

namespace rime {

std::string FrontmostBundleId() {
  @autoreleasepool {
    NSRunningApplication* app = [[NSWorkspace sharedWorkspace] frontmostApplication];
    NSString* bundle_id = app.bundleIdentifier;
    if (!bundle_id) {
      return "";
    }
    const char* utf8 = [bundle_id UTF8String];
    return utf8 ? std::string(utf8) : std::string();
  }
}

}  // namespace rime

#endif  // __APPLE__
