"""Pure-logic coverage for rime_corpus.replay.

Never touches ~/Library/Rime, RIME_CORPUS_DIR, or a real userdb --
`restore_pristine_userdb` is exercised against tempdirs the test itself
manufactures, and `run_arm`/`assert_deterministic` are exercised with
`replay.run_arm` mocked, never a real subprocess.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import replay, speller


class HanRunsTest(unittest.TestCase):
    def test_single_run_has_empty_context(self):
        self.assertEqual(replay.han_runs("故意的"), [("", "故意的")])

    def test_context_accumulates_across_runs(self):
        self.assertEqual(
            replay.han_runs("这个 filter 的顺序"),
            [("", "这个"), ("这个 filter ", "的顺序")],
        )

    def test_text_with_no_han_yields_nothing(self):
        self.assertEqual(replay.han_runs("just english"), [])

    def test_placeholder_is_a_run_boundary(self):
        """A redaction placeholder is non-Han, so it separates runs -- which is
        semantically right: a redacted span really is 'not Chinese here'."""
        self.assertEqual(
            replay.han_runs("发到 ⟦EMAIL⟧ 这个邮箱"),
            [("", "发到"), ("发到 ⟦EMAIL⟧ ", "这个邮箱")],
        )


class BuildRequestsTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sp = speller.Speller(speller.load_rules(speller.FLYPY_RULES))

    def test_keys_are_twice_the_character_count(self):
        records = [{"id": "abc", "text": "故意的"}]
        requests = replay.build_requests(records, self.sp)
        self.assertEqual(len(requests), 1)
        self.assertEqual(requests[0]["keys"], "guyide")
        self.assertEqual(len(requests[0]["keys"]), 2 * len(requests[0]["text"]))

    def test_request_id_carries_the_run_index(self):
        records = [{"id": "abc", "text": "这个 x 的顺序"}]
        ids = [r["id"] for r in replay.build_requests(records, self.sp)]
        self.assertEqual(ids, ["abc#0", "abc#1"])

    def test_run_with_an_unspellable_syllable_is_dropped(self):
        """呣/嗯 are `m`/`n` in pinyin -- no initial, no final, so no rule
        produces two keys. A short key string would break the
        2-keys-per-character invariant every alignment downstream depends
        on, so the whole run must be dropped rather than replayed short."""
        self.assertEqual(replay.build_requests([{"id": "abc", "text": "呣呣"}], self.sp), [])

    def test_a_run_is_dropped_whole_not_truncated(self):
        """One unspellable character costs the entire run, not just itself --
        keeping the rest would silently shift every later character's
        alignment."""
        self.assertEqual(
            replay.build_requests([{"id": "abc", "text": "故意呣的"}], self.sp), []
        )


class StripTimingsTest(unittest.TestCase):
    def test_removes_us_from_every_segment(self):
        responses = [{"id": "a", "segments": [{"hit": 0, "us": {"keys": 1, "menu": 2}}]}]
        stripped = replay.strip_timings(responses)
        self.assertNotIn("us", stripped[0]["segments"][0])

    def test_does_not_mutate_its_argument(self):
        responses = [{"id": "a", "segments": [{"hit": 0, "us": {"keys": 1}}]}]
        replay.strip_timings(responses)
        self.assertIn("us", responses[0]["segments"][0])

    def test_two_responses_differing_only_in_us_become_equal(self):
        """The whole reason this function exists: every response carries a
        wall-clock `us` field that differs between any two runs by
        definition, so a byte-for-byte comparison could never pass without
        this -- not even on a genuinely deterministic replayer."""
        a = [{"id": "x", "segments": [{"hit": 0, "us": {"keys": 1}}]}]
        b = [{"id": "x", "segments": [{"hit": 0, "us": {"keys": 999}}]}]
        self.assertEqual(replay.strip_timings(a), replay.strip_timings(b))

    def test_a_real_difference_survives_stripping(self):
        a = [{"id": "x", "segments": [{"hit": 0, "us": {"keys": 1}}]}]
        b = [{"id": "x", "segments": [{"hit": 5, "us": {"keys": 1}}]}]
        self.assertNotEqual(replay.strip_timings(a), replay.strip_timings(b))


class RestorePristineUserdbTest(unittest.TestCase):
    """Both source and destination are tempdirs the test manufactures --
    never the real userdb, never RIME_CORPUS_DIR."""

    def test_copies_each_userdb_directory(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            pristine, rime_dir = tmp / "pristine", tmp / "rime-dir"
            for name in replay.USERDB_NAMES:
                (pristine / name).mkdir(parents=True)
                (pristine / name / "MANIFEST").write_text("pristine")
            rime_dir.mkdir()

            replay.restore_pristine_userdb(rime_dir, pristine)

            for name in replay.USERDB_NAMES:
                self.assertEqual((rime_dir / name / "MANIFEST").read_text(), "pristine")

    def test_overwrites_a_dirty_userdb(self):
        """This is the whole point: replay COMMITS candidates, and committing
        trains the dictionary. A restore that only creates missing files and
        leaves existing ones alone would not undo that training."""
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            pristine, rime_dir = tmp / "pristine", tmp / "rime-dir"
            for name in replay.USERDB_NAMES:
                (pristine / name).mkdir(parents=True)
                (pristine / name / "MANIFEST").write_text("pristine")
                (rime_dir / name).mkdir(parents=True)
                (rime_dir / name / "MANIFEST").write_text("trained-on-the-test-set")

            replay.restore_pristine_userdb(rime_dir, pristine)

            for name in replay.USERDB_NAMES:
                self.assertEqual((rime_dir / name / "MANIFEST").read_text(), "pristine")

    def test_missing_pristine_snapshot_raises(self):
        with tempfile.TemporaryDirectory() as tmp:
            tmp = Path(tmp)
            with self.assertRaises(FileNotFoundError):
                replay.restore_pristine_userdb(tmp / "rime-dir", tmp / "no-such-pristine")


class AssertDeterministicTest(unittest.TestCase):
    """Exercises the comparison/raising logic with `replay.run_arm` mocked --
    no subprocess, no real Rime directory, no userdb touched."""

    def test_returns_the_first_pass_when_identical_after_stripping(self):
        first = [{"id": "a", "status": "ok", "segments": [{"hit": 0, "us": {"keys": 1}}]}]
        second = [{"id": "a", "status": "ok", "segments": [{"hit": 0, "us": {"keys": 999}}]}]
        with mock.patch.object(replay, "run_arm", side_effect=[first, second]):
            result = replay.assert_deterministic([], "replayer", "rime-dir", "pristine")
        self.assertEqual(result, first)

    def test_raises_on_a_real_divergence(self):
        first = [{"id": "a", "status": "ok", "segments": [{"hit": 0}]}]
        second = [{"id": "a", "status": "ok", "segments": [{"hit": 5}]}]
        with mock.patch.object(replay, "run_arm", side_effect=[first, second]):
            with self.assertRaises(AssertionError):
                replay.assert_deterministic([], "replayer", "rime-dir", "pristine")

    def test_both_passes_go_through_run_arm_not_run(self):
        """Sentinel 4 must not skip the restore step -- each of the two
        passes has to be its own full restore-run-restore arm, so this must
        call run_arm (which restores), not run (which does not)."""
        first = second = [{"id": "a", "status": "ok", "segments": []}]
        with mock.patch.object(replay, "run_arm", side_effect=[first, second]) as run_arm:
            replay.assert_deterministic(["req"], "replayer", "rime-dir", "pristine", window=32)
        self.assertEqual(run_arm.call_count, 2)
        # 6-tuple, not 5: `wait_for_warm` (Task 6, replay.run_arm/run) is a
        # positional parameter assert_deterministic forwards unchanged, and
        # not passing it here means it carries its default, False. The point
        # of this assertion is still "each pass is a full run_arm call with
        # the caller's own arguments", not merely "run_arm was called twice"
        # -- weakening it to a call count would stop catching a pass that
        # silently called `run` (no restore) instead.
        for call in run_arm.call_args_list:
            self.assertEqual(call.args, (["req"], "replayer", "rime-dir", "pristine", 32, False))


if __name__ == "__main__":
    unittest.main()
