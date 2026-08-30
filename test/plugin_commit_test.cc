// NOTE: <gtest/gtest.h> is included before "plugin_commit.h" (deviating from
// the brief's literal include order) because plugin_commit.h transitively
// includes <rime/processor.h> -> rime_api.h, which #defines a bare `Bool`
// macro; if that macro is active when gtest-param-test.h is parsed, it
// collides with gtest's `Bool()` function template. See
// test/ime_bridge_state_test.cc and test/commit_text_test.cc for the same
// ordering used to avoid this pre-existing macro collision.
#include <gtest/gtest.h>

#include "plugin_commit.h"

#include <string>
#include <vector>

namespace {

// Records the order of operations, and models the one thing that makes this
// invariant necessary: NotifyForLearning reaches librime's OnCommit, which
// appends its own per-segment records and so BURIES whatever was at .back().
struct Recorder : rime::CommitSteps {
  std::vector<std::string> ops;
  std::string back;  // what commit_history().back() would return

  void CommitText(const std::string& text) override {
    ops.push_back("commit:" + text);
    back = text;  // librime pushes {"raw", text} itself (engine.cc:244-246)
  }
  void NotifyForLearning() override {
    ops.push_back("notify");
    back = "PER-SEGMENT";  // OnCommit's Push(composition, input)
  }
  void PushRecord(const std::string& text) override {
    ops.push_back("push:" + text);
    back = text;
  }
  void Clear() override {
    ops.push_back("clear");
    // Context::Clear() fires update_notifier_ SYNCHRONOUSLY, and the reader on
    // the other end takes commit_history().back().text. Capture what it saw.
    seen_by_reader = back;
  }
  std::string seen_by_reader;
};

}  // namespace

// The invariant: by the time Clear() wakes its reader, .back() must be the
// text this commit actually produced -- not the per-segment records that
// NotifyForLearning's OnCommit appended in between.
TEST(PluginCommit, TheReaderWokenByClearSeesThisCommitsText) {
  Recorder r;
  rime::RunCommitSequence(r, "你好 ", /*learn=*/true);
  EXPECT_EQ(r.seen_by_reader, "你好 ");
}

TEST(PluginCommit, OrderIsCommitNotifyReassertClear) {
  Recorder r;
  rime::RunCommitSequence(r, "你好 ", /*learn=*/true);
  const std::vector<std::string> expected{"commit:你好 ", "notify", "push:你好 ", "clear"};
  EXPECT_EQ(r.ops, expected);
}

// Without learning there is no OnCommit in between, so nothing buries .back()
// and the re-assert would be a duplicate record. One commit, one record.
TEST(PluginCommit, WithoutLearningThereIsNoReassert) {
  Recorder r;
  rime::RunCommitSequence(r, "abc", /*learn=*/false);
  const std::vector<std::string> expected{"commit:abc", "clear"};
  EXPECT_EQ(r.ops, expected);
  EXPECT_EQ(r.seen_by_reader, "abc");
}

TEST(PluginCommit, EmptyTextCommitsNothingAtAll) {
  Recorder r;
  rime::RunCommitSequence(r, "", /*learn=*/true);
  EXPECT_TRUE(r.ops.empty());
}
