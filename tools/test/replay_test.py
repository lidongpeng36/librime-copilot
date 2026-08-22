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

    def test_a_single_han_character_is_a_run(self):
        # Regression guard: a second module-level `_HAN_RUN` once shadowed the
        # one han_runs uses, narrowing it to `{2,}` and silently dropping every
        # single-character run from every replay request.
        self.assertEqual([("", "字")], replay.han_runs("字"))

    def test_extended_cjk_is_a_run(self):
        # corpus.HAN_CLASS covers Ext-A, compatibility ideographs and Ext-B/C/D.
        # The shadowing pattern was the basic block only, so this is the other
        # half of the same regression.
        self.assertEqual([("", "㐀")], replay.han_runs("㐀"))


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


class UserdbFingerprintTest(unittest.TestCase):
    def _userdb(self, root: Path, name: str, payload: bytes) -> None:
        directory = root / name
        directory.mkdir(parents=True, exist_ok=True)
        (directory / "000005.ldb").write_bytes(payload)

    def test_finds_a_committed_phrase_in_the_raw_bytes(self):
        # LevelDB stores keys as plain bytes, so a memorised phrase is
        # literally present in the file.
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self._userdb(root, "private.userdb", "ran hou\t然后\tc=70".encode("utf-8"))
            found = replay.userdb_fingerprint(root)
            self.assertIn("然后", found)

    def test_an_untouched_snapshot_yields_nothing(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            self._userdb(root, "private.userdb", b"\x00\x11\x07\x01/db_name\x01private")
            self.assertEqual(set(), replay.userdb_fingerprint(root))

    def test_a_missing_userdb_directory_yields_nothing(self):
        with tempfile.TemporaryDirectory() as raw:
            self.assertEqual(set(), replay.userdb_fingerprint(Path(raw)))


class RunWarmedArmTest(unittest.TestCase):
    """The warm-then-measure pass, with the harness's own moving parts faked.

    replay.run is patched: the real one shells out to replay_copilot, which
    needs a deployed Rime directory. What this class checks is the ORDER and
    the guard, which is where a warmed arm goes wrong.
    """

    def _pristine(self, root: Path) -> Path:
        pristine = root / "pristine"
        (pristine / "private.userdb").mkdir(parents=True)
        (pristine / "private.userdb" / "000005.ldb").write_bytes(b"empty")
        (pristine / "melt_eng.userdb").mkdir(parents=True)
        (pristine / "melt_eng.userdb" / "000005.ldb").write_bytes(b"empty")
        return pristine

    def test_warm_requests_run_before_the_measured_ones(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            pristine = self._pristine(root)
            rime_dir = root / "rime"
            rime_dir.mkdir()
            seen = []

            def fake_run(requests, *args, **kwargs):
                seen.append([r["id"] for r in requests])
                # Simulate learning: the warm pass writes a phrase.
                (rime_dir / "private.userdb" / "000041.log").write_bytes(
                    "然后".encode("utf-8"))
                return [{"id": r["id"]} for r in requests]

            with mock.patch.object(replay, "run", side_effect=fake_run):
                out = replay.run_warmed_arm(
                    [{"id": "warm-1"}], [{"id": "eval-1"}],
                    replayer="x", rime_dir=str(rime_dir), pristine_dir=str(pristine))

            self.assertEqual([["warm-1"], ["eval-1"]], seen)
            self.assertEqual([{"id": "eval-1"}], out)

    def test_the_userdb_is_restored_before_warming_and_after_measuring(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            pristine = self._pristine(root)
            rime_dir = root / "rime"
            rime_dir.mkdir()
            (rime_dir / "private.userdb").mkdir()
            (rime_dir / "private.userdb" / "stale.ldb").write_bytes(b"contamination")

            def fake_run(requests, *args, **kwargs):
                (rime_dir / "private.userdb" / "000041.log").write_bytes(
                    "然后".encode("utf-8"))
                return []

            with mock.patch.object(replay, "run", side_effect=fake_run):
                replay.run_warmed_arm(
                    [{"id": "w"}], [{"id": "e"}],
                    replayer="x", rime_dir=str(rime_dir), pristine_dir=str(pristine))

            self.assertFalse((rime_dir / "private.userdb" / "stale.ldb").exists())
            self.assertFalse((rime_dir / "private.userdb" / "000041.log").exists())

    def test_a_warm_phase_that_learned_nothing_is_refused(self):
        # The failure this guard exists for: the warm pass runs, writes
        # nothing, and the arm silently reports the UNWARMED number as though
        # it were warmed.
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            pristine = self._pristine(root)
            rime_dir = root / "rime"
            rime_dir.mkdir()

            with mock.patch.object(replay, "run", side_effect=lambda *a, **k: []):
                with self.assertRaises(RuntimeError) as caught:
                    replay.run_warmed_arm(
                        [{"id": "w"}], [{"id": "e"}],
                        replayer="x", rime_dir=str(rime_dir), pristine_dir=str(pristine))
            self.assertIn("learned nothing", str(caught.exception))

    def test_the_guard_can_be_turned_off_for_a_deliberately_cold_arm(self):
        with tempfile.TemporaryDirectory() as raw:
            root = Path(raw)
            pristine = self._pristine(root)
            rime_dir = root / "rime"
            rime_dir.mkdir()

            with mock.patch.object(replay, "run", side_effect=lambda *a, **k: [{"id": "e"}]):
                out = replay.run_warmed_arm(
                    [], [{"id": "e"}],
                    replayer="x", rime_dir=str(rime_dir), pristine_dir=str(pristine),
                    require_learning=False)
            self.assertEqual([{"id": "e"}], out)


if __name__ == "__main__":
    unittest.main()
