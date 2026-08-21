"""Which evaluation set `isolate` compares against, said out loud.

cmd_isolate called corpus.corpus_dir() with no argument, so it always
fingerprinted the default directory. Under a train/eval split that is BOTH
halves, and the check would report the training half as an overlap and mean
nothing. The only existing lever was the RIME_CORPUS_DIR environment variable --
an implicit env var deciding what a correctness check compares against is the
same silent-divergence shape as dict.json's `boost` key.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from io import StringIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_train import cli


def write_corpus(directory: Path, texts: list[str]) -> None:
    directory.mkdir(parents=True, exist_ok=True)
    with open(directory / "dingtalk.jsonl", "w", encoding="utf-8") as handle:
        for i, text in enumerate(texts):
            handle.write(json.dumps(
                {"v": 1, "id": str(i), "src": "dingtalk",
                 "ts": "2026-01-01T00:00:00+08:00", "text": text, "redacted": []},
                ensure_ascii=False) + "\n")


class IsolateEvalCorpusDirTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.addCleanup(self._tmp.cleanup)
        self.training = self.root / "training.txt"
        self.training.write_text("我们明天讨论这个方案的细节\n", encoding="utf-8")

    def _run(self, eval_dir: Path) -> str:
        out = StringIO()
        with redirect_stdout(out):
            cli.main(["isolate", "--corpus", str(self.training),
                      "--eval-corpus-dir", str(eval_dir)])
        return out.getvalue()

    def test_names_the_directory_it_compared_against(self):
        ev = self.root / "eval"
        write_corpus(ev, ["完全无关的一句话"])
        self.assertIn(str(ev), self._run(ev))

    def test_an_overlapping_eval_half_is_reported(self):
        ev = self.root / "eval"
        write_corpus(ev, ["我们明天讨论这个方案的细节"])
        text = self._run(ev)
        self.assertNotIn("overlapping training sentences: 0", text)

    def test_a_disjoint_eval_half_reports_no_overlap(self):
        ev = self.root / "eval"
        write_corpus(ev, ["今天的天气真好适合出去走走"])
        self.assertIn("overlapping training sentences: 0", self._run(ev))

    def test_pointing_at_the_other_half_changes_the_answer(self):
        """The whole reason the flag exists: aimed at the training half, the
        same training corpus overlaps itself completely."""
        same = self.root / "train"
        write_corpus(same, ["我们明天讨论这个方案的细节"])
        other = self.root / "eval"
        write_corpus(other, ["今天的天气真好适合出去走走"])
        self.assertNotIn("overlapping training sentences: 0", self._run(same))
        self.assertIn("overlapping training sentences: 0", self._run(other))


if __name__ == "__main__":
    unittest.main()
