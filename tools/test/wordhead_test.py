"""The word-head table generator, and its half of the golden pin."""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import wordhead  # noqa: E402

GOLDEN = Path(__file__).resolve().parents[2] / "test" / "data" / "wordhead_golden.txt"
# Hand-written, never mined: the remote is public (CLAUDE.md, goldens.py).
FIXTURE_TEXTS = ["现在 先 走", "发现 先", "现场"]


def split(text: str) -> "list[str]":
    return text.split()


class Counting(unittest.TestCase):
    def test_single_characters_are_solo_and_longer_tokens_credit_their_head(self):
        head, solo = wordhead.count(FIXTURE_TEXTS, split)
        self.assertEqual(dict(head), {"现": 2, "发": 1})
        self.assertEqual(dict(solo), {"先": 2, "走": 1})

    def test_non_han_tokens_are_ignored(self):
        head, solo = wordhead.count(["abc 12 ， 先"], split)
        self.assertEqual(dict(head), {})
        self.assertEqual(dict(solo), {"先": 1})

    def test_mass_is_smoothed_by_two_on_each_side(self):
        m = wordhead.masses({"现": 2}, {"先": 2})
        self.assertAlmostEqual(m["现"], 4 / 6)
        self.assertAlmostEqual(m["先"], 2 / 6)


class GoldenPin(unittest.TestCase):
    """The Python half: test/wordhead_table_test.cc parses the same file."""

    def test_render_of_the_fixture_is_the_golden_byte_for_byte(self):
        head, solo = wordhead.count(FIXTURE_TEXTS, split)
        self.assertEqual(wordhead.render(wordhead.masses(head, solo)),
                         GOLDEN.read_text(encoding="utf-8"))

    def test_generate_writes_it_atomically_and_reports_the_count(self):
        with tempfile.TemporaryDirectory() as tmp:
            corpus = Path(tmp) / "corpus"
            corpus.mkdir()
            (corpus / "c.jsonl").write_text(
                "".join(json.dumps({"text": t}) + "\n" for t in FIXTURE_TEXTS), encoding="utf-8")
            out = Path(tmp) / "wordhead.txt"
            self.assertEqual(wordhead.generate(corpus=corpus, output=out, segment=split), 4)
            self.assertEqual(out.read_text(encoding="utf-8"), GOLDEN.read_text(encoding="utf-8"))
            self.assertFalse((Path(tmp) / "wordhead.txt.new").exists())


if __name__ == "__main__":
    unittest.main()
