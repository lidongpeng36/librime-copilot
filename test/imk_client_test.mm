// macOS-only ObjC++ test: drives the IMK client query / compose-detection logic
// against a mock NSTextInputClient, with no live Squirrel controller and no
// system input source. Compiled into copilot_test on Apple only.

#include "imk_client_internal.h"

#include <gtest/gtest.h>

#import <Foundation/Foundation.h>

using rime::ClientIsComposing;
using rime::QuerySurroundingFromClient;

// Minimal stand-in for the IMK text client: responds to the same selectors
// (selectedRange / markedRange / attributedSubstringFromRange:) the real query
// path uses, backed by a plain NSString document.
@interface MockTextClient : NSObject
@property(nonatomic) NSRange selectedRangeValue;
@property(nonatomic) NSRange markedRangeValue;
@property(nonatomic, strong) NSString* text;
@end

@implementation MockTextClient
- (NSRange)selectedRange {
  return _selectedRangeValue;
}
- (NSRange)markedRange {
  return _markedRangeValue;
}
- (NSAttributedString*)attributedSubstringFromRange:(NSRange)range {
  if (range.location == NSNotFound ||
      range.location + range.length > _text.length) {
    return nil;
  }
  return [[NSAttributedString alloc]
      initWithString:[_text substringWithRange:range]];
}
@end

TEST(ImkClientQuery, ExtractsBeforeAndAfter) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"中a";
    c.selectedRangeValue = NSMakeRange(1, 0);  // cursor between 中 and a
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("中", s->before);
    EXPECT_EQ("a", s->after);
  }
}

TEST(ImkClientQuery, NoBeforeAtDocumentStart) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"a中";
    c.selectedRangeValue = NSMakeRange(0, 0);  // cursor at the very start
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("", s->before);
    EXPECT_EQ("a", s->after);
  }
}

TEST(ImkClientQuery, NoAfterAtDocumentEnd) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"中a";
    c.selectedRangeValue = NSMakeRange(2, 0);  // cursor at the very end
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("a", s->before);
    EXPECT_EQ("", s->after);
  }
}

TEST(ImkClientQuery, NilClientReturnsNullopt) {
  EXPECT_FALSE(QuerySurroundingFromClient(nil).has_value());
}

TEST(ImkClientComposing, TrueWhenMarkedTextPresent) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.markedRangeValue = NSMakeRange(0, 2);
    EXPECT_TRUE(ClientIsComposing(c));
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    EXPECT_FALSE(ClientIsComposing(c));
    c.markedRangeValue = NSMakeRange(3, 0);  // valid location but zero length
    EXPECT_FALSE(ClientIsComposing(c));
  }
}

TEST(ImkClientComposing, NilClientNotComposing) {
  EXPECT_FALSE(ClientIsComposing(nil));
}
