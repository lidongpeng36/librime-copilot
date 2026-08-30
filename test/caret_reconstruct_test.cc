#include "caret_reconstruct.h"

#include <gtest/gtest.h>

using rime::caret::ReconstructFromHistory;
using rime::caret::ReconstructInput;

TEST(CaretReconstruct, BeforeIsTheLastCommittedText) {
  auto r = ReconstructFromHistory(ReconstructInput{"你好"});
  EXPECT_TRUE(r.usable);
  EXPECT_EQ(r.before, "你好");
}

// The commit history knows only what was committed, i.e. what is BEHIND the
// caret. Claiming to know what follows it would be a lie, and a lie that
// NeedSpaceAfter would act on.
TEST(CaretReconstruct, AfterIsAlwaysEmpty) {
  auto r = ReconstructFromHistory(ReconstructInput{"你好"});
  EXPECT_EQ(r.after, "");
}

// An empty history is not "before is empty" -- it is "I do not know". The
// difference matters: an empty `before` is a positive claim that the caret is
// at the start of the text, which would suppress a space that belongs there.
TEST(CaretReconstruct, EmptyHistoryIsNotUsable) {
  auto r = ReconstructFromHistory(ReconstructInput{""});
  EXPECT_FALSE(r.usable);
}

TEST(CaretReconstruct, AsciiTextSurvivesVerbatim) {
  auto r = ReconstructFromHistory(ReconstructInput{" hello"});
  EXPECT_TRUE(r.usable);
  EXPECT_EQ(r.before, " hello");
}
