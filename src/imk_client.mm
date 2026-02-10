// IMK Client - Access IMKTextInput client from within a librime plugin
// via ObjC runtime method swizzling.
//
// When loaded inside Squirrel (macOS rime frontend), this hooks into
// SquirrelInputController's handleEvent:client: to capture the text input
// client. On each key event, it queries the client for the character before
// and after the cursor, and caches the results for use by the plugin.

#ifdef __APPLE__

#include "imk_client.h"

#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <mutex>
#include <string>

namespace rime {
namespace {

static IMP s_originalHandleEvent = nullptr;
static std::mutex s_cacheMutex;
static std::optional<SurroundingText> s_cachedContext;

// Query surrounding text from the IMK client and cache it
void CacheSurroundingText(id controller, id sender) {
  // Clear cache first - will only be re-set on success
  {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cachedContext.reset();
  }

  // Get the client: [controller client]
  SEL clientSel = @selector(client);
  if (![controller respondsToSelector:clientSel]) {
    return;
  }
  id client = ((id(*)(id, SEL))objc_msgSend)(controller, clientSel);
  if (!client) {
    return;
  }

  // Get selectedRange
  SEL selRangeSel = @selector(selectedRange);
  if (![client respondsToSelector:selRangeSel]) {
    return;
  }

  typedef NSRange (*SelRangeFn)(id, SEL);
  NSRange selRange = ((SelRangeFn)objc_msgSend)(client, selRangeSel);

  if (selRange.location == NSNotFound) {
    return;
  }

  // Get attributedSubstringFromRange:
  SEL attrSubSel = @selector(attributedSubstringFromRange:);
  if (![client respondsToSelector:attrSubSel]) {
    return;
  }

  typedef NSAttributedString* (*AttrSubFn)(id, SEL, NSRange);
  AttrSubFn attrSub = (AttrSubFn)objc_msgSend;

  std::string charBefore;
  std::string charAfter;
  std::string clientKey = "imk:unknown";
  {
    if (sender) {
      const char* senderClass = object_getClassName(sender);
      NSString* senderAddr = [NSString stringWithFormat:@"%p", sender];
      NSString* key =
          [NSString stringWithFormat:@"imk_sender:%s:%@",
                                     senderClass ?: "unknown", senderAddr];
      clientKey = [key UTF8String] ?: "imk:unknown";
    } else {
      NSString* clientAddr = [NSString stringWithFormat:@"%p", client];
      clientKey = [clientAddr UTF8String] ?: "imk:unknown";
    }
  }

  if (selRange.location > 0) {
    NSRange beforeRange = NSMakeRange(selRange.location - 1, 1);
    NSAttributedString* before = attrSub(client, attrSubSel, beforeRange);
    if (before) {
      charBefore = [[before string] UTF8String] ?: "";
    }
  }

  {
    NSRange afterRange = NSMakeRange(selRange.location + selRange.length, 1);
    NSAttributedString* after = attrSub(client, attrSubSel, afterRange);
    if (after) {
      charAfter = [[after string] UTF8String] ?: "";
    }
  }

  // Cache the result
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  s_cachedContext = SurroundingText{charBefore, charAfter, clientKey};
}

// Swizzled handleEvent:client:
static BOOL swizzled_handleEvent(id self, SEL _cmd, void* event, id sender) {
  @autoreleasepool {
    CacheSurroundingText(self, sender);
  }

  // Call original implementation
  return ((BOOL(*)(id, SEL, void*, id))s_originalHandleEvent)(self, _cmd, event,
                                                              sender);
}

// Initialize the hook at load time
__attribute__((constructor)) static void InitIMKClientHook() {
  @autoreleasepool {
    // Find IMKInputController or its subclasses
    Class imkBase = NSClassFromString(@"IMKInputController");
    Class squirrelCls = NSClassFromString(@"SquirrelInputController");

    Class targetClass = nil;
    if (imkBase) {
      unsigned int classCount = 0;
      Class* classes = objc_copyClassList(&classCount);
      for (unsigned int i = 0; i < classCount; i++) {
        Class superclass = class_getSuperclass(classes[i]);
        while (superclass) {
          if (superclass == imkBase) {
            targetClass = classes[i];
            break;
          }
          superclass = class_getSuperclass(superclass);
        }
      }
      free(classes);
    }

    Class cls = squirrelCls ?: targetClass ?: imkBase;
    if (!cls) {
      return;
    }

    SEL sel = @selector(handleEvent:client:);
    Method method = class_getInstanceMethod(cls, sel);
    if (!method) {
      return;
    }

    s_originalHandleEvent = method_getImplementation(method);
    method_setImplementation(method, (IMP)swizzled_handleEvent);

    NSLog(@"[IMK] Hooked handleEvent:client: on %s", class_getName(cls));
  }
}

}  // anonymous namespace

// Public API to get cached surrounding text
std::optional<SurroundingText> GetIMKSurroundingText() {
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  return s_cachedContext;
}

}  // namespace rime

#endif  // __APPLE__
