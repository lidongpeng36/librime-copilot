// Internal, Objective-C++-only seam for the IMK client query logic.
//
// These take an Objective-C `id` (an NSTextInputClient-like object), so this
// header must only be included from `.mm` translation units — never from the
// C++-visible imk_client.h. Extracted so the query / compose-detection logic
// can be unit-tested against a mock client (see test/imk_client_test.mm),
// without a live Squirrel controller.

#pragma once

#ifdef __APPLE__

#include <objc/objc.h>  // id

#include <optional>

#include "imk_client.h"  // SurroundingText

namespace rime {

// True if `client` currently has marked (preedit) text — i.e. a composition is
// in progress. Used to skip the (expensive) surrounding-text query while
// composing. Returns false for nil / non-responding clients.
bool ClientIsComposing(id client);

// Query the character immediately before and after the cursor from `client`
// via selectedRange + attributedSubstringFromRange:. Returns nullopt if the
// client is nil, does not respond, or has no valid selection.
std::optional<SurroundingText> QuerySurroundingFromClient(id client);

}  // namespace rime

#endif  // __APPLE__
