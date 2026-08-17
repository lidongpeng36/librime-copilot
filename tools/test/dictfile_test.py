"""Rime dictionary parsing and writing.

The YAML header cases here are the regression guard for 7312800: a header
parsed as vocabulary produced `name: -> custom` entries that build_copilot
accepted silently.
"""
from __future__ import annotations

import io
import sys
import tempfile
import types
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot.dictfile import DEFAULT_WEIGHT, Entry, read_entries, write_dict

HEADER = '''# a comment
---
name: fixture
version: "1"
sort: by_weight
columns:
  - text
  - weight
...
'''


class ReadEntries(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, text: str) -> Path:
        path = self.dir / "d.dict.yaml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_skips_the_yaml_header(self):
        entries = read_entries(self.write(HEADER + "建议\tjian yi\t500\n"))
        self.assertEqual([Entry("建议", "jian yi", 500)], entries)

    def test_header_without_terminator_raises(self):
        # Silently swallowing the file as metadata would build an empty db.
        with self.assertRaises(ValueError) as caught:
            read_entries(self.write("---\nname: broken\n建议\tjian yi\t1\n"))
        self.assertIn("...", str(caught.exception))

    def test_file_with_no_header_still_parses(self):
        entries = read_entries(self.write("# c\n建议\tjian yi\t500\n"))
        self.assertEqual([Entry("建议", "jian yi", 500)], entries)

    def test_dashes_after_the_header_do_not_reopen_it(self):
        entries = read_entries(self.write(HEADER + "建议\tjian yi\t500\n"))
        self.assertEqual(1, len(entries))

    def test_two_columns_numeric_second_is_a_weight(self):
        # word+weight (tencent.dict.yaml's shape) has no pinyin column, so
        # this exercises _auto_pinyin -> pypinyin. Stub the real package
        # (per test_missing_pypinyin_raises_an_actionable_error below) so
        # this test does not require pypinyin to actually be installed --
        # the suite must pass on a stock interpreter, not just one CI
        # happens to `pip install pypinyin` into.
        calls = []

        def lazy_pinyin(word):
            calls.append(word)
            return ["FAKE", "PY"]

        fake_pypinyin = types.ModuleType("pypinyin")
        fake_pypinyin.lazy_pinyin = lazy_pinyin

        had_module = "pypinyin" in sys.modules
        original = sys.modules.get("pypinyin")

        def restore():
            if had_module:
                sys.modules["pypinyin"] = original
            else:
                sys.modules.pop("pypinyin", None)

        self.addCleanup(restore)
        sys.modules["pypinyin"] = fake_pypinyin

        entries = read_entries(self.write("建议\t500\n"))
        self.assertEqual("建议", entries[0].word)
        self.assertEqual(500, entries[0].weight)
        # The two-column-numeric branch must actually call pypinyin and use
        # its result, not merely tolerate it being importable.
        self.assertEqual(["建议"], calls)
        self.assertEqual("FAKE PY", entries[0].pinyin)

    def test_two_columns_non_numeric_second_is_a_pinyin(self):
        entries = read_entries(self.write("建议\tjian yi\n"))
        self.assertEqual(Entry("建议", "jian yi", DEFAULT_WEIGHT), entries[0])

    def test_non_integer_weight_is_dropped(self):
        self.assertEqual([], read_entries(self.write("建议\tjian yi\tlots\n")))

    def test_word_containing_whitespace_is_dropped_with_a_warning(self):
        # build_copilot splits on whitespace; such a word cannot round-trip.
        # Capture the warning rather than letting it dirty the suite's output.
        path = self.write("New York\tniu yue\t100\n建议\tjian yi\t1\n")
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            entries = read_entries(path)
        self.assertEqual([Entry("建议", "jian yi", 1)], entries)
        self.assertIn("New York", buffer.getvalue())

    def test_comments_and_blank_lines_are_skipped(self):
        entries = read_entries(self.write(HEADER + "\n# x\n建议\tjian yi\t1\n"))
        self.assertEqual(1, len(entries))

    def test_keep_selects_by_word(self):
        path = self.write(HEADER + "建议\tjian yi\t1\n瓴\tling\t2\n")
        entries = read_entries(path, keep=lambda w: w == "瓴")
        self.assertEqual([Entry("瓴", "ling", 2)], entries)

    def test_keep_rejects_before_pypinyin_is_ever_reached(self):
        # The point of the filter: on a dictionary with no reading column,
        # a rejected word must not cost a pypinyin call. Leave pypinyin
        # unimportable so a call would raise instead of quietly succeeding.
        had_module = "pypinyin" in sys.modules
        original = sys.modules.get("pypinyin")

        def restore():
            if had_module:
                sys.modules["pypinyin"] = original
            else:
                sys.modules.pop("pypinyin", None)

        self.addCleanup(restore)
        sys.modules["pypinyin"] = None  # import pypinyin -> ImportError

        entries = read_entries(self.write("建议\t500\n"), keep=lambda w: False)
        self.assertEqual([], entries)

    def test_missing_pypinyin_raises_an_actionable_error(self):
        # Make `import pypinyin` genuinely fail for the duration of this
        # test (not merely "never called") by planting the sys.modules None
        # sentinel Python itself uses to remember a prior failed import.
        had_module = "pypinyin" in sys.modules
        original = sys.modules.get("pypinyin")

        def restore():
            if had_module:
                sys.modules["pypinyin"] = original
            else:
                sys.modules.pop("pypinyin", None)

        self.addCleanup(restore)
        sys.modules["pypinyin"] = None

        # A dictionary that already carries a pinyin column must still parse:
        # this proves the import is lazy, not merely deferred by one call.
        entries = read_entries(self.write("建议\tjian yi\t500\n"))
        self.assertEqual([Entry("建议", "jian yi", 500)], entries)

        # tencent.dict.yaml-shaped input (word, weight) has no pinyin column,
        # so it must hit _auto_pinyin and fail actionably rather than with
        # the raw ModuleNotFoundError traceback.
        with self.assertRaises(ImportError) as caught:
            read_entries(self.write("建议\t500\n"))
        message = str(caught.exception)
        self.assertIn("pypinyin", message)
        self.assertIn("interpreter", message)


class WriteDict(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_round_trips(self):
        path = self.dir / "out.dict.yaml"
        original = [Entry("建议", "jian yi", 500), Entry("落盘", "luo pan", 145)]
        written = write_dict(path, name="out", version="1", entries=original,
                             comment_lines=["# from a test"])
        self.assertEqual(2, written)
        self.assertEqual(original, read_entries(path))

    def test_emits_a_terminated_header(self):
        path = self.dir / "out.dict.yaml"
        write_dict(path, name="out", version="1", entries=[Entry("建议", "jian yi", 1)])
        lines = path.read_text(encoding="utf-8").splitlines()
        self.assertIn("---", lines)
        self.assertIn("...", lines)
        self.assertLess(lines.index("---"), lines.index("..."))
        self.assertIn("name: out", lines)

    def test_comment_lines_and_preset_vocabulary_are_written(self):
        # Regression guard: cmd_fetch (tools/rime_copilot/cli.py) relies on
        # both `comment_lines` and `use_preset_vocabulary` to reproduce
        # sogou.dict.yaml's header. Neither was ever asserted before this:
        # test_round_trips passes comment_lines but never checks it landed in
        # the file, and use_preset_vocabulary was never exercised at all —
        # either could be silently dropped and every existing test would
        # still pass.
        path = self.dir / "out.dict.yaml"
        write_dict(path, name="out", version="1", entries=[Entry("建议", "jian yi", 1)],
                  comment_lines=["# Rime dictionary", "# Sogou Pinyin Dict"],
                  use_preset_vocabulary=True)
        lines = path.read_text(encoding="utf-8").splitlines()
        self.assertIn("# Rime dictionary", lines)
        self.assertIn("# Sogou Pinyin Dict", lines)
        self.assertIn("use_preset_vocabulary: true", lines)
        # The comments belong before the header, the vocabulary flag inside it.
        self.assertLess(lines.index("# Rime dictionary"), lines.index("---"))
        self.assertLess(lines.index("---"), lines.index("use_preset_vocabulary: true"))
        self.assertLess(lines.index("use_preset_vocabulary: true"), lines.index("..."))

    def test_no_comment_lines_and_no_preset_vocabulary_by_default(self):
        # The flip side: when the caller does not ask for either, neither
        # line should appear at all.
        path = self.dir / "out.dict.yaml"
        write_dict(path, name="out", version="1", entries=[Entry("建议", "jian yi", 1)])
        text = path.read_text(encoding="utf-8")
        self.assertNotIn("use_preset_vocabulary", text)
        self.assertFalse(text.startswith("#"))


if __name__ == "__main__":
    unittest.main()
