"""The rename repair: what it must refuse.

This tool rewrites a file that is a transcript of the user's own typing and
exists in exactly one good copy, so every case here is a way to lose or
duplicate that data rather than a way to format it.
"""
from __future__ import annotations

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

_SPEC = importlib.util.spec_from_file_location(
    "merge_renamed_telemetry",
    Path(__file__).resolve().parents[1] / "merge_renamed_telemetry.py")
merge = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(merge)


def line(ts, machine=None, **kw):
    d = {"v": 5, "ts": ts}
    if machine is not None:
        d["machine"] = machine
    d.update(kw)
    return json.dumps(d, ensure_ascii=False)


class Rewrite(unittest.TestCase):
    def test_the_stale_name_is_corrected_and_kept(self):
        out, n = merge.rewrite([line("t1", "Old")], "Old", "New")
        self.assertEqual(n, 1)
        d = json.loads(out[0])
        self.assertEqual(d["machine"], "New")
        self.assertEqual(d["machine_was"], "Old")

    def test_lines_already_carrying_the_current_name_are_untouched(self):
        """A stale file holds BOTH names: the flip happened mid-file, when the
        deployer was corrected but the cached Writer was not."""
        src = line("t1", "New")
        out, n = merge.rewrite([src], "Old", "New")
        self.assertEqual(n, 0)
        self.assertEqual(out[0], src)
        self.assertNotIn("machine_was", json.loads(out[0]))

    def test_a_line_with_no_machine_field_survives_verbatim(self):
        """Stats lines have none, and they are the majority of a long file."""
        src = line("t1", None, type="stats", segments=9)
        out, n = merge.rewrite([src], "Old", "New")
        self.assertEqual((out, n), ([src], 0))


class Plan(unittest.TestCase):
    def test_a_clean_split_merges_in_time_order(self):
        stale = [line("2026-08-20T10:00:00+0800", "Old"),
                 line("2026-08-23T10:00:00+0800", "New")]
        cur = [line("2026-08-27T10:00:00+0800", "New")]
        merged, corrected = merge.plan(stale, cur, "Old", "New")
        self.assertEqual(len(merged), 3)
        self.assertEqual(corrected, 1)
        ts = [json.loads(m)["ts"] for m in merged]
        self.assertEqual(ts, sorted(ts))

    def test_it_refuses_a_file_already_merged(self):
        """Idempotency by refusal, not by silently doing nothing: running it
        twice must not double the file, and must say why it did not."""
        cur = [json.dumps({"v": 5, "ts": "t", "machine": "New", "machine_was": "Old"})]
        with self.assertRaises(ValueError) as e:
            merge.plan([line("t0", "Old")], cur, "Old", "New")
        self.assertIn("already merged", str(e.exception))

    def test_it_refuses_when_a_line_appears_in_both(self):
        shared = line("2026-08-20T10:00:00+0800", "New")
        with self.assertRaises(ValueError) as e:
            merge.plan([shared], [shared], "Old", "New")
        self.assertIn("refusing to duplicate", str(e.exception))

    def test_it_refuses_overlapping_time_ranges(self):
        """These two files are one machine's log split by a restart, so the
        stale one ends where the current one begins. An overlap means the
        input is not what this tool is for."""
        stale = [line("2026-08-27T10:00:00+0800", "Old")]
        cur = [line("2026-08-20T10:00:00+0800", "New")]
        with self.assertRaises(ValueError) as e:
            merge.plan(stale, cur, "Old", "New")
        self.assertIn("overlap", str(e.exception))


class Main(unittest.TestCase):
    def _files(self, tmp, stale_lines, cur_lines):
        s = Path(tmp) / "MacBookPro-M1.jsonl"
        c = Path(tmp) / "MacBookAir-M4.jsonl"
        s.write_text("".join(x + "\n" for x in stale_lines), encoding="utf-8")
        c.write_text("".join(x + "\n" for x in cur_lines), encoding="utf-8")
        return s, c

    def test_it_refuses_the_shared_copy(self):
        """The shared file is a PROJECTION of the local one -- every sync
        overwrites it wholesale, so a repair there lasts until the next sync.
        This is the mistake that produced this tool."""
        with tempfile.TemporaryDirectory() as tmp:
            d = Path(tmp) / "sync" / "copilot_telemetry"
            d.mkdir(parents=True)
            s, c = self._files(d, [line("t0", "MacBookPro-M1")], [line("t1", "MacBookAir-M4")])
            rc = merge.main([str(s), str(c), "--allow-running-squirrel"])
            self.assertEqual(rc, 1)

    def test_the_repaired_file_is_private(self):
        """0600: the file is a chronological transcript of the user's own
        Chinese input, and a default umask would publish it to the group."""
        with tempfile.TemporaryDirectory() as tmp:
            s, c = self._files(tmp, [line("2026-08-20T10:00:00+0800", "MacBookPro-M1")],
                               [line("2026-08-27T10:00:00+0800", "MacBookAir-M4")])
            self.assertEqual(merge.main([str(s), str(c), "--allow-running-squirrel"]), 0)
            self.assertEqual(c.stat().st_mode & 0o777, 0o600)
            got = [json.loads(x) for x in c.read_text(encoding="utf-8").splitlines()]
            self.assertEqual([d["machine"] for d in got],
                             ["MacBookAir-M4", "MacBookAir-M4"])
            self.assertEqual(got[0]["machine_was"], "MacBookPro-M1")

    def test_the_stale_file_is_kept_not_deleted(self):
        with tempfile.TemporaryDirectory() as tmp:
            s, c = self._files(tmp, [line("2026-08-20T10:00:00+0800", "MacBookPro-M1")],
                               [line("2026-08-27T10:00:00+0800", "MacBookAir-M4")])
            merge.main([str(s), str(c), "--allow-running-squirrel"])
            self.assertFalse(s.exists())
            self.assertTrue(s.with_name(s.name + ".merged").is_file())

    def test_a_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as tmp:
            s, c = self._files(tmp, [line("2026-08-20T10:00:00+0800", "MacBookPro-M1")],
                               [line("2026-08-27T10:00:00+0800", "MacBookAir-M4")])
            before = c.read_text(encoding="utf-8")
            self.assertEqual(merge.main([str(s), str(c), "--dry-run"]), 0)
            self.assertEqual(c.read_text(encoding="utf-8"), before)
            self.assertTrue(s.is_file())


if __name__ == "__main__":
    unittest.main()


class StaleNameCannotBeGuessed(unittest.TestCase):
    """A file renamed out of the way no longer ends in `.jsonl`, so a name
    derived from the filename matches nothing and every line passes through
    still attributed to the machine that never typed them. That merge looks
    like success -- the line count is right -- which is the same failure this
    tool exists to repair, reintroduced by the repair."""

    def test_it_refuses_when_nothing_matched(self):
        stale = [line("2026-08-20T10:00:00+0800", "MacBookPro-M1")]
        cur = [line("2026-08-27T10:00:00+0800", "MacBookAir-M4")]
        with self.assertRaises(ValueError) as e:
            merge.plan(stale, cur, "MacBookPro-M1.jsonl", "MacBookAir-M4")
        self.assertIn("--stale-name", str(e.exception))
        self.assertIn("MacBookPro-M1", str(e.exception))

    def test_a_stale_file_written_entirely_after_the_flip_is_not_an_error(self):
        """Legitimately zero: the rename happened before any of these lines, so
        they already carry the current name and there is nothing to correct."""
        stale = [line("2026-08-20T10:00:00+0800", "MacBookAir-M4")]
        cur = [line("2026-08-27T10:00:00+0800", "MacBookAir-M4")]
        merged, corrected = merge.plan(stale, cur, "MacBookPro-M1", "MacBookAir-M4")
        self.assertEqual((len(merged), corrected), (2, 0))

    def test_the_flag_overrides_the_filename(self):
        with tempfile.TemporaryDirectory() as tmp:
            s = Path(tmp) / "MacBookPro-M1.jsonl.bak-20260828"
            c = Path(tmp) / "MacBookAir-M4.jsonl"
            s.write_text(line("2026-08-20T10:00:00+0800", "MacBookPro-M1") + "\n",
                         encoding="utf-8")
            c.write_text(line("2026-08-27T10:00:00+0800", "MacBookAir-M4") + "\n",
                         encoding="utf-8")
            self.assertEqual(merge.main([str(s), str(c), "--stale-name", "MacBookPro-M1",
                                         "--allow-running-squirrel"]), 0)
            got = [json.loads(x) for x in c.read_text(encoding="utf-8").splitlines()]
            self.assertEqual(got[0]["machine_was"], "MacBookPro-M1")
