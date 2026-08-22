"""Pure-logic coverage for rime_corpus.compare_warmed.

Never touches ~/Library/Rime, RIME_CORPUS_DIR, or a real userdb --
`replay.run_arm` and `replay.run_warmed_arm` are mocked throughout; nothing
here shells out to `replay_copilot`. Corpus fixtures are real files, but
always under a tempdir this test manufactures itself.
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import compare_warmed as cw
from rime_corpus import replay


def _write_jsonl(path: Path, records: list[dict]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(path, "w", encoding="utf-8") as handle:
        for r in records:
            handle.write(json.dumps(r, ensure_ascii=False) + "\n")


class McNemarTest(unittest.TestCase):
    """Hand-computed cases against the continuity-corrected statistic
    docs/superpowers/specs/2026-08-22-lexicon-phase2-results.md reports:
    chi2 = (|b-c|-1)^2 / (b+c), p from the chi-square_1 upper tail."""

    def test_hand_computed_case_matches_the_results_document(self):
        # b=449 (GAIN), c=549 (LOSS) from that document's Step 3 table:
        # chi2 = (|449-549|-1)^2/(449+549) = 99^2/998 = 9.819...
        chi2, p = cw.mcnemar(449, 549)
        self.assertAlmostEqual(chi2, 9.82, places=2)
        # The document reports p = 0.0017.
        self.assertAlmostEqual(p, 0.0017, places=3)

    def test_no_discordant_pairs_does_not_divide_by_zero(self):
        chi2, p = cw.mcnemar(0, 0)
        self.assertEqual(chi2, 0.0)
        self.assertEqual(p, 1.0)

    def test_large_asymmetry_gives_a_small_p_value(self):
        chi2, p = cw.mcnemar(0, 100)
        self.assertLess(p, 0.001)

    def test_small_symmetric_counts_give_a_large_p_value(self):
        chi2, p = cw.mcnemar(5, 4)
        self.assertGreater(p, 0.5)

    def test_is_symmetric_in_its_two_arguments(self):
        self.assertEqual(cw.mcnemar(30, 12), cw.mcnemar(12, 30))


class MainSharedRequestListTest(unittest.TestCase):
    """The methodology this module exists to reproduce: both arms measured on
    the SAME request list, generated once from the eval corpus; only the warm
    arm additionally sees a warm list, built from a separate corpus dir."""

    def _corpora(self, tmp: Path):
        eval_dir = tmp / "eval"
        warm_dir = tmp / "warm"
        _write_jsonl(
            eval_dir / "a.jsonl",
            [
                {"id": "e1", "text": "故意的"},
                {"id": "e2", "text": "这个顺序"},
            ],
        )
        _write_jsonl(warm_dir / "a.jsonl", [{"id": "w1", "text": "然后呢"}])
        return eval_dir, warm_dir

    def _run(self, tmp: Path, fake_run_arm, fake_run_warmed_arm):
        eval_dir, warm_dir = self._corpora(tmp)
        with mock.patch.object(replay, "run_arm", side_effect=fake_run_arm), mock.patch.object(
            replay, "run_warmed_arm", side_effect=fake_run_warmed_arm
        ), redirect_stdout(io.StringIO()):
            rc = cw.main(
                [
                    "--eval-corpus-dir",
                    str(eval_dir),
                    "--warm-corpus-dir",
                    str(warm_dir),
                    "--rime-dir-cold",
                    str(tmp / "cold"),
                    "--rime-dir-warm",
                    str(tmp / "warm-rime"),
                    "--pristine",
                    str(tmp / "pristine"),
                ]
            )
        return rc

    def test_both_arms_receive_the_same_measured_request_list(self):
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            calls: dict = {}

            def fake_run_arm(requests, *a, **k):
                calls["cold_requests"] = requests
                return []

            def fake_run_warmed_arm(warm_requests, requests, *a, **k):
                calls["warm_measured_requests"] = requests
                return []

            rc = self._run(tmp, fake_run_arm, fake_run_warmed_arm)

            self.assertEqual(rc, 0)
            self.assertEqual(calls["cold_requests"], calls["warm_measured_requests"])
            self.assertEqual([r["id"] for r in calls["cold_requests"]], ["e1#0", "e2#0"])

    def test_only_the_warm_arm_receives_the_warm_list(self):
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            calls: dict = {}

            def fake_run_arm(requests, *a, **k):
                # The cold arm's only positional argument is the measured
                # request list -- there is no separate warm-list parameter
                # for it to receive one through.
                return []

            def fake_run_warmed_arm(warm_requests, requests, *a, **k):
                calls["warm_ids"] = [r["id"] for r in warm_requests]
                return []

            self._run(tmp, fake_run_arm, fake_run_warmed_arm)

            self.assertEqual(calls["warm_ids"], ["w1#0"])

    def test_no_eval_corpus_is_reported_and_returns_nonzero(self):
        with tempfile.TemporaryDirectory() as raw:
            tmp = Path(raw)
            (tmp / "warm").mkdir()
            _write_jsonl(tmp / "warm" / "a.jsonl", [{"id": "w1", "text": "然后"}])
            with mock.patch.object(replay, "run_arm") as run_arm, mock.patch.object(
                replay, "run_warmed_arm"
            ) as run_warmed_arm, redirect_stdout(io.StringIO()):
                rc = cw.main(
                    [
                        "--eval-corpus-dir",
                        str(tmp / "no-such-eval-dir"),
                        "--warm-corpus-dir",
                        str(tmp / "warm"),
                        "--rime-dir-cold",
                        str(tmp / "cold"),
                        "--rime-dir-warm",
                        str(tmp / "warm-rime"),
                        "--pristine",
                        str(tmp / "pristine"),
                    ]
                )
            self.assertNotEqual(rc, 0)
            run_arm.assert_not_called()
            run_warmed_arm.assert_not_called()


if __name__ == "__main__":
    unittest.main()
