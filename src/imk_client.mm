// IMK Client - Access IMKTextInput client from within a librime plugin
// via ObjC runtime method swizzling.
//
// When loaded inside Squirrel (macOS rime frontend), this hooks into
// SquirrelInputController's handleEvent:client: to capture the text input
// client. On each key event, it queries the client for the character before
// and after the cursor, and caches the results for use by the plugin.

#ifdef __APPLE__

#include "history.h"
#include "imk_client.h"
#include "imk_client_internal.h"

#import <Foundation/Foundation.h>
#import <objc/message.h>
#import <objc/runtime.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>

namespace rime {
namespace {

static IMP s_originalHandleEvent = nullptr;
static std::mutex s_cacheMutex;
static std::optional<SurroundingText> s_cachedContext;
// 1 = today's behavior (just the boundary character AutoSpacer needs).
static std::atomic<int> s_prefixChars{1};

// Derive a stable per-client key for per-app state isolation.
std::string ClientKeyFromSender(id sender, id client) {
  if (sender) {
    const char* senderClass = object_getClassName(sender);
    NSString* senderAddr = [NSString stringWithFormat:@"%p", sender];
    NSString* key = [NSString stringWithFormat:@"imk_sender:%s:%@",
                                               senderClass ?: "unknown", senderAddr];
    return [key UTF8String] ?: "imk:unknown";
  }
  NSString* clientAddr = [NSString stringWithFormat:@"%p", client];
  return [clientAddr UTF8String] ?: "imk:unknown";
}

// Query surrounding text from the IMK client and cache it.
void CacheSurroundingText(id controller, id sender) {
  SEL clientSel = @selector(client);
  id client = [controller respondsToSelector:clientSel]
                  ? ((id (*)(id, SEL))objc_msgSend)(controller, clientSel)
                  : nil;

  // Skip the query while composing: the AutoSpacer only uses the boundary
  // snapshot taken at composition start, so mid-composition queries are wasted.
  // markedRange reflects the app's live composition state at hook time, so this
  // stays correct across commits — after a commit there is no marked text, so
  // the next word's first key refreshes the boundary.
  if (client && ClientIsComposing(client)) {
    return;  // keep the composition-start snapshot
  }

  {
    std::lock_guard<std::mutex> lock(s_cacheMutex);
    s_cachedContext.reset();
  }
  if (!client) {
    return;
  }

  auto surrounding = QuerySurroundingFromClient(client, s_prefixChars.load());
  if (!surrounding) {
    return;
  }
  surrounding->client_key = ClientKeyFromSender(sender, client);

  std::lock_guard<std::mutex> lock(s_cacheMutex);
  s_cachedContext = surrounding;
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

bool ClientIsComposing(id client) {
  if (!client) {
    return false;
  }
  SEL markedSel = @selector(markedRange);
  if (![client respondsToSelector:markedSel]) {
    return false;
  }
  typedef NSRange (*MarkedRangeFn)(id, SEL);
  NSRange marked = ((MarkedRangeFn)objc_msgSend)(client, markedSel);
  return marked.location != NSNotFound && marked.length > 0;
}

std::optional<SurroundingText> QuerySurroundingFromClient(id client, int prefix_chars) {
  if (!client) {
    return std::nullopt;
  }
  SEL selRangeSel = @selector(selectedRange);
  if (![client respondsToSelector:selRangeSel]) {
    return std::nullopt;
  }
  typedef NSRange (*SelRangeFn)(id, SEL);
  NSRange selRange = ((SelRangeFn)objc_msgSend)(client, selRangeSel);
  if (selRange.location == NSNotFound) {
    return std::nullopt;
  }

  SEL attrSubSel = @selector(attributedSubstringFromRange:);
  if (![client respondsToSelector:attrSubSel]) {
    return std::nullopt;
  }
  typedef NSAttributedString* (*AttrSubFn)(id, SEL, NSRange);
  AttrSubFn attrSub = (AttrSubFn)objc_msgSend;

  const int requested = std::max(prefix_chars, 1);
  // What we asked for, clamped to what the document could possibly hold
  // before the caret -- captured before the fallback retry below, so the
  // classifier can tell "the document ended" apart from "the app cut it".
  // Mirrors tmux_source_util.h's `available`, captured before its tail is
  // taken (src/tmux_source_util.h:602-611).
  NSUInteger wanted = 0;

  std::string charBefore;
  std::string charAfter;
  if (selRange.location > 0) {
    wanted = std::min(static_cast<NSUInteger>(requested), selRange.location);
    NSAttributedString* before =
        attrSub(client, attrSubSel, NSMakeRange(selRange.location - wanted, wanted));
    // Some clients answer a single character but refuse a longer range. Falling
    // through with "no context" would also cost AutoSpacer its boundary
    // character, so retry with one character before giving up.
    if (!before && wanted > 1) {
      before = attrSub(client, attrSubSel, NSMakeRange(selRange.location - 1, 1));
    }
    if (before) {
      NSString* text = [before string];
      // The range is in UTF-16 units, so its first unit may be the trailing
      // half of a surrogate pair; drop it rather than emit a broken character.
      if (text.length > 0) {
        unichar first = [text characterAtIndex:0];
        if (first >= 0xDC00 && first <= 0xDFFF) {
          text = [text substringFromIndex:1];
        }
      }
      charBefore = [text UTF8String] ?: "";
    }
  }
  {
    NSAttributedString* after =
        attrSub(client, attrSubSel, NSMakeRange(selRange.location + selRange.length, 1));
    if (after) {
      charAfter = [[after string] UTF8String] ?: "";
    }
  }
  const int got = ::copilot::CharCount(charBefore);

  SurroundingText out;
  out.before = charBefore;
  out.after = charAfter;
  out.client_key = "imk:query";
  out.before_depth = got;
  out.after_depth = ::copilot::CharCount(charAfter);
  // `wanted` is what we asked for after clamping to the document; `got` is
  // what came back. Comparing against `wanted` (not `requested`) is what
  // makes the exact-tie case read as kFull rather than kByConfig: when the
  // document holds fewer characters than were requested, `wanted` already
  // equals the document's own length, so "got everything we asked for"
  // and "the region ended on its own" are the same event.
  if (got < static_cast<int>(wanted)) {
    // The app answered less than even the clamped ask -- e.g. it refuses long
    // ranges and we fell back to one character. The document had more
    // (`wanted` already accounts for its length); the app declined to give it.
    //
    // NOTE selRange.location is in UTF-16 units while `got`/`wanted` are code
    // points, so on astral text this comparison can fire when the true cause
    // was kFull or kByConfig, never the reverse -- it only ever reports
    // "less than requested", which is what happened. It does not change
    // which of kFull/kByConfig fires below, and does not make either of
    // those two branches approximate.
    out.truncation = Truncation::kByApp;
  } else if (selRange.location <= static_cast<NSUInteger>(requested)) {
    // The document does not hold `requested` characters before the caret --
    // including the exact tie, where it holds precisely `requested`: the
    // region ended on its own, nothing was cut.
    out.truncation = Truncation::kFull;
  } else {
    // The document had more than `requested` and we got all of `wanted`
    // (which is capped at `requested`): the budget is what cut it.
    out.truncation = Truncation::kByConfig;
  }
  return out;
}

void SetIMKSurroundingPrefixChars(int n) { s_prefixChars.store(std::clamp(n, 1, 64)); }

// Public API to get cached surrounding text
std::optional<SurroundingText> GetIMKSurroundingText() {
  std::lock_guard<std::mutex> lock(s_cacheMutex);
  return s_cachedContext;
}

}  // namespace rime

#endif  // __APPLE__
