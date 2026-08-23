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
// Some real clients answer a 1-character request but return nil for a longer
// range; set this to emulate them (see ImkClientQuery.FallsBackToOneChar).
@property(nonatomic) NSUInteger maxAnswerableLength;
@end

@implementation MockTextClient
- (NSRange)selectedRange {
  return _selectedRangeValue;
}
- (NSRange)markedRange {
  return _markedRangeValue;
}
- (instancetype)init {
  if ((self = [super init])) {
    _maxAnswerableLength = NSUIntegerMax;
  }
  return self;
}
- (NSAttributedString*)attributedSubstringFromRange:(NSRange)range {
  if (range.location == NSNotFound ||
      range.location + range.length > _text.length ||
      range.length > _maxAnswerableLength) {
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
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/1);
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
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/1);
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
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/1);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("a", s->before);
    EXPECT_EQ("", s->after);
  }
}

TEST(ImkClientQuery, ExtractsMultiCharPrefix) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"今天天气很好";
    c.selectedRangeValue = NSMakeRange(5, 0);  // caret before 好
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/3);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("天气很", s->before);
    EXPECT_EQ("好", s->after);  // `after` stays a single character
  }
}

TEST(ImkClientQuery, PrefixIsClampedAtDocumentStart) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"今天很好";
    c.selectedRangeValue = NSMakeRange(2, 0);  // only two characters precede
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/8);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("今天", s->before);
  }
}

TEST(ImkClientQuery, FallsBackToOneCharWhenLongRangeIsRefused) {
  // R1: a client that refuses the longer range must not cost us the boundary
  // character AutoSpacer needs — retry with a single character instead of
  // reporting "no context at all".
  //
  // The document holds 4 characters before the caret and 8 were requested;
  // the app answered only 1, so this is a kByApp case, not kFull -- the
  // document did not run out, the app declined to answer the full request.
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"今天天气";
    c.selectedRangeValue = NSMakeRange(4, 0);
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    c.maxAnswerableLength = 1;
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/8);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ("气", s->before);
    EXPECT_EQ(1, s->before_depth);
    EXPECT_EQ(rime::Truncation::kByApp, s->truncation);
  }
}

// The tie: exactly as many characters precede the caret as were requested.
// The region ended on its own -- nothing was cut -- so this must be kFull,
// not kByConfig. This is AutoSpacer's own default request shape
// (prefix_chars=1), so it is also the most common real-world case.
TEST(ImkClientQuery, TruncationFullOnExactTie) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"a中";
    c.selectedRangeValue = NSMakeRange(1, 0);  // one character precedes
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/1);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(1, s->before_depth);
    EXPECT_EQ(rime::Truncation::kFull, s->truncation);
  }
}

TEST(ImkClientQuery, NilClientReturnsNullopt) {
  EXPECT_FALSE(QuerySurroundingFromClient(nil, /*prefix_chars=*/1).has_value());
}

// Asked for more than the document holds: the region genuinely ended.
TEST(ImkClientQuery, TruncationFullWhenDocumentIsShorterThanRequested) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"今天";
    c.selectedRangeValue = NSMakeRange(2, 0);  // only two characters precede
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/8);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(2, s->before_depth);
    EXPECT_EQ(rime::Truncation::kFull, s->truncation);
  }
}

// Asked for 2 of a 4-character document: the budget is what cut it.
TEST(ImkClientQuery, TruncationByConfigWhenDocumentHasMore) {
  @autoreleasepool {
    MockTextClient* c = [MockTextClient new];
    c.text = @"今天天气";
    c.selectedRangeValue = NSMakeRange(4, 0);  // caret after 天天, before 气
    c.markedRangeValue = NSMakeRange(NSNotFound, 0);
    auto s = QuerySurroundingFromClient(c, /*prefix_chars=*/2);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(2, s->before_depth);
    EXPECT_EQ(rime::Truncation::kByConfig, s->truncation);
  }
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
