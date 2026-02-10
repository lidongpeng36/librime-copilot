// Accessibility API Proof of Concept
// Tests getting text and cursor position from focused text fields on macOS.
//
// Usage: run the program, then switch to a text editor and type.
// The program will print the text around the cursor every second.

#ifdef __APPLE__

#import <AppKit/AppKit.h>
#import <ApplicationServices/ApplicationServices.h>

#include <cstdio>
#include <string>

// Convert CFString to std::string (UTF-8)
std::string CFStringToString(CFStringRef cf_str) {
  if (!cf_str) return "";
  CFIndex length = CFStringGetLength(cf_str);
  CFIndex max_size = CFStringGetMaximumSizeForEncoding(length, kCFStringEncodingUTF8) + 1;
  std::string result(max_size, '\0');
  if (CFStringGetCString(cf_str, result.data(), max_size, kCFStringEncodingUTF8)) {
    result.resize(std::strlen(result.c_str()));
    return result;
  }
  return "";
}

// Get a single UTF-8 character ending at byte position `pos`
std::string GetCharBefore(const std::string& text, size_t byte_pos) {
  if (byte_pos == 0 || byte_pos > text.size()) return "";
  size_t start = byte_pos - 1;
  while (start > 0 && (static_cast<uint8_t>(text[start]) & 0xC0) == 0x80) {
    --start;
  }
  return text.substr(start, byte_pos - start);
}

// Get a single UTF-8 character starting at byte position `pos`
std::string GetCharAfter(const std::string& text, size_t byte_pos) {
  if (byte_pos >= text.size()) return "";
  size_t end = byte_pos + 1;
  while (end < text.size() && (static_cast<uint8_t>(text[end]) & 0xC0) == 0x80) {
    ++end;
  }
  return text.substr(byte_pos, end - byte_pos);
}

inline uint32_t Utf8ToCodepoint(const std::string& s) {
  uint32_t code = 0;
  const auto* bytes = reinterpret_cast<const uint8_t*>(s.data());
  size_t len = s.size();
  if (len == 1) code = bytes[0];
  else if (len == 2) code = ((bytes[0] & 0x1F) << 6) | (bytes[1] & 0x3F);
  else if (len == 3) code = ((bytes[0] & 0x0F) << 12) | ((bytes[1] & 0x3F) << 6) | (bytes[2] & 0x3F);
  else if (len == 4) code = ((bytes[0] & 0x07) << 18) | ((bytes[1] & 0x3F) << 12) | ((bytes[2] & 0x3F) << 6) | (bytes[3] & 0x3F);
  return code;
}

bool IsCJK(uint32_t cp) {
  return (cp >= 0x4E00 && cp <= 0x9FFF) ||   // CJK Unified Ideographs
         (cp >= 0x3400 && cp <= 0x4DBF) ||   // CJK Extension A
         (cp >= 0x20000 && cp <= 0x2A6DF) || // CJK Extension B
         (cp >= 0xF900 && cp <= 0xFAFF) ||   // CJK Compatibility Ideographs
         (cp >= 0x2F800 && cp <= 0x2FA1F);   // CJK Compatibility Supplement
}

bool IsAsciiAlnum(uint32_t cp) {
  return (cp >= '0' && cp <= '9') || (cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z');
}

const char* CharType(const std::string& ch) {
  if (ch.empty()) return "empty";
  uint32_t cp = Utf8ToCodepoint(ch);
  if (IsCJK(cp)) return "CJK";
  if (IsAsciiAlnum(cp)) return "ASCII_ALNUM";
  if (cp == ' ') return "SPACE";
  if (cp < 0x80) return "ASCII_OTHER";
  return "OTHER_UNICODE";
}

bool NeedSpace(const std::string& before, const std::string& after) {
  if (before.empty() || after.empty()) return false;
  uint32_t cp_before = Utf8ToCodepoint(before);
  uint32_t cp_after = Utf8ToCodepoint(after);
  if (IsCJK(cp_before) && IsAsciiAlnum(cp_after)) return true;
  if (IsAsciiAlnum(cp_before) && IsCJK(cp_after)) return true;
  return false;
}

// Convert CFIndex (UTF-16 code unit index) to byte offset in UTF-8 string.
size_t CharIndexToByteOffset(const std::string& utf8_text, CFIndex char_index) {
  CFIndex utf16_count = 0;
  size_t byte_offset = 0;

  while (byte_offset < utf8_text.size() && utf16_count < char_index) {
    uint8_t c = static_cast<uint8_t>(utf8_text[byte_offset]);
    size_t char_bytes;
    int utf16_units;

    if (c < 0x80) { char_bytes = 1; utf16_units = 1; }
    else if ((c & 0xE0) == 0xC0) { char_bytes = 2; utf16_units = 1; }
    else if ((c & 0xF0) == 0xE0) { char_bytes = 3; utf16_units = 1; }
    else if ((c & 0xF8) == 0xF0) { char_bytes = 4; utf16_units = 2; }
    else { char_bytes = 1; utf16_units = 1; }

    byte_offset += char_bytes;
    utf16_count += utf16_units;
  }

  return byte_offset;
}

struct AXTextInfo {
  bool ok = false;
  std::string app_name;
  std::string role;
  std::string text;
  std::string char_before;
  std::string char_after;
  CFIndex cursor_pos = -1;
  std::string error;
};

// Try to read text and cursor info from an AXUIElement.
// Returns true if the element has text value.
bool TryReadTextInfo(AXUIElementRef element, AXTextInfo& info) {
  // Get text value
  CFTypeRef value = nullptr;
  AXError err = AXUIElementCopyAttributeValue(
      element, kAXValueAttribute, &value);
  if (err != kAXErrorSuccess || !value) {
    return false;
  }

  if (CFGetTypeID(value) != CFStringGetTypeID()) {
    CFRelease(value);
    return false;
  }

  info.text = CFStringToString((CFStringRef)value);
  CFRelease(value);

  // Get role
  CFStringRef role = nullptr;
  AXUIElementCopyAttributeValue(element, kAXRoleAttribute, (CFTypeRef*)&role);
  if (role) {
    info.role = CFStringToString(role);
    CFRelease(role);
  }

  // Get selected text range (cursor position)
  CFTypeRef range_value = nullptr;
  err = AXUIElementCopyAttributeValue(
      element, kAXSelectedTextRangeAttribute, &range_value);
  if (err != kAXErrorSuccess || !range_value) {
    info.error = "has text but no cursor range";
    return false;
  }

  CFRange range;
  if (AXValueGetValue((AXValueRef)range_value,
                      (AXValueType)kAXValueCFRangeType, &range)) {
    info.cursor_pos = range.location;
    size_t byte_offset = CharIndexToByteOffset(info.text, range.location);
    info.char_before = GetCharBefore(info.text, byte_offset);
    info.char_after = GetCharAfter(info.text, byte_offset);
    info.ok = true;
  }
  CFRelease(range_value);

  return info.ok;
}

// Recursively search for a text element starting from `element`.
// Tries the element itself first, then its focused child, then all children.
bool FindTextElement(AXUIElementRef element, AXTextInfo& info, int depth = 0) {
  if (depth > 5) return false;

  // Try this element directly
  if (TryReadTextInfo(element, info)) {
    return true;
  }

  // Try focused child element
  AXUIElementRef focused_child = nullptr;
  AXError err = AXUIElementCopyAttributeValue(
      element, kAXFocusedUIElementAttribute, (CFTypeRef*)&focused_child);
  if (err == kAXErrorSuccess && focused_child) {
    bool found = FindTextElement(focused_child, info, depth + 1);
    CFRelease(focused_child);
    if (found) return true;
  }

  // Try children array
  CFTypeRef children = nullptr;
  err = AXUIElementCopyAttributeValue(
      element, kAXChildrenAttribute, &children);
  if (err == kAXErrorSuccess && children &&
      CFGetTypeID(children) == CFArrayGetTypeID()) {
    CFArrayRef arr = (CFArrayRef)children;
    CFIndex count = CFArrayGetCount(arr);
    // Limit search to avoid performance issues
    CFIndex limit = (count < 10) ? count : 10;
    for (CFIndex i = 0; i < limit; i++) {
      AXUIElementRef child =
          (AXUIElementRef)CFArrayGetValueAtIndex(arr, i);
      if (FindTextElement(child, info, depth + 1)) {
        CFRelease(children);
        return true;
      }
    }
    CFRelease(children);
  }

  return false;
}

// Get frontmost window's owner PID using CGWindowList (pure C, no event loop needed)
pid_t GetFrontmostPID() {
  CFArrayRef windowList = CGWindowListCopyWindowInfo(
      kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements,
      kCGNullWindowID);
  if (!windowList) return -1;

  pid_t pid = -1;
  CFIndex count = CFArrayGetCount(windowList);
  for (CFIndex i = 0; i < count; i++) {
    CFDictionaryRef info =
        (CFDictionaryRef)CFArrayGetValueAtIndex(windowList, i);

    // Only consider normal windows (layer 0)
    CFNumberRef layerRef = nullptr;
    if (!CFDictionaryGetValueIfPresent(
            info, kCGWindowLayer, (const void**)&layerRef))
      continue;
    int layer = -1;
    CFNumberGetValue(layerRef, kCFNumberIntType, &layer);
    if (layer != 0) continue;

    CFNumberRef pidRef = nullptr;
    if (CFDictionaryGetValueIfPresent(
            info, kCGWindowOwnerPID, (const void**)&pidRef)) {
      CFNumberGetValue(pidRef, kCFNumberIntType, &pid);
      break;  // first layer-0 window is frontmost
    }
  }
  CFRelease(windowList);
  return pid;
}

AXTextInfo GetFocusedTextInfo() {
  AXTextInfo info;

  pid_t pid = GetFrontmostPID();
  if (pid < 0) {
    info.error = "No frontmost window found";
    return info;
  }

  // Get app name from PID
  NSRunningApplication* app =
      [NSRunningApplication runningApplicationWithProcessIdentifier:pid];
  info.app_name = app ? [app.localizedName UTF8String] ?: "unknown"
                      : ("pid=" + std::to_string(pid));

  // Create AXUIElement for the frontmost application
  AXUIElementRef app_element = AXUIElementCreateApplication(pid);

  // Get focused UI element from the application
  AXUIElementRef focused_element = nullptr;
  AXError err = AXUIElementCopyAttributeValue(
      app_element, kAXFocusedUIElementAttribute, (CFTypeRef*)&focused_element);

  if (err != kAXErrorSuccess || !focused_element) {
    // Fallback: search from app element directly
    if (!FindTextElement(app_element, info)) {
      info.error = "App '" + info.app_name +
                   "': no focused element (AXError=" +
                   std::to_string(err) + ")";
    }
    CFRelease(app_element);
    return info;
  }
  CFRelease(app_element);

  // Try the focused element, then search deeper if needed
  if (!FindTextElement(focused_element, info)) {
    // Report what we found
    CFStringRef role = nullptr;
    AXUIElementCopyAttributeValue(
        focused_element, kAXRoleAttribute, (CFTypeRef*)&role);
    std::string role_str;
    if (role) {
      role_str = CFStringToString(role);
      CFRelease(role);
    }
    info.error = "App '" + info.app_name + "' [" + role_str +
                 "]: no text field found";
  }
  CFRelease(focused_element);

  return info;
}

int main() {
  @autoreleasepool {
    if (!AXIsProcessTrusted()) {
      printf("=== Accessibility permission NOT granted ===\n");
      printf("Please grant Accessibility permission to this program:\n");
      printf("  System Settings -> Privacy & Security -> Accessibility\n\n");

      const void* keys[] = {kAXTrustedCheckOptionPrompt};
      const void* values[] = {kCFBooleanTrue};
      CFDictionaryRef opts = CFDictionaryCreate(
          nullptr, keys, values, 1, &kCFTypeDictionaryKeyCallBacks,
          &kCFTypeDictionaryValueCallBacks);
      AXIsProcessTrustedWithOptions(opts);
      CFRelease(opts);

      printf("Waiting for permission... (re-run after granting)\n");
      return 1;
    }

    printf("=== Accessibility API POC ===\n");
    printf("Accessibility permission: GRANTED\n");
    printf("Switch to a text editor and move your cursor around.\n");
    printf("Press Ctrl+C to exit.\n\n");

    while (true) {
      @autoreleasepool {
        auto info = GetFocusedTextInfo();

        if (!info.ok) {
          printf("[!] %s\n", info.error.c_str());
        } else {
          std::string display_text = info.text;
          if (display_text.size() > 80) {
            display_text = display_text.substr(0, 77) + "...";
          }
          for (auto& c : display_text) {
            if (c == '\n') c = ' ';
          }

          printf("---\n");
          printf("  App:    %s\n", info.app_name.c_str());
          printf("  Role:   %s\n", info.role.c_str());
          printf("  Text:   \"%s\"\n", display_text.c_str());
          printf("  Cursor: %ld (UTF-16)\n", (long)info.cursor_pos);
          printf("  Before: \"%s\" (%s)\n", info.char_before.c_str(),
                 CharType(info.char_before));
          printf("  After:  \"%s\" (%s)\n", info.char_after.c_str(),
                 CharType(info.char_after));
          printf("  Space?  %s\n",
                 NeedSpace(info.char_before, info.char_after) ? "YES" : "NO");
        }
        fflush(stdout);
      }

      [[NSRunLoop currentRunLoop]
          runUntilDate:[NSDate dateWithTimeIntervalSinceNow:1.0]];
    }
  }

  return 0;
}

#else

#include <cstdio>
int main() {
  printf("This POC only works on macOS.\n");
  return 1;
}

#endif
