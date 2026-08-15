"""The rebuild decision.

The mtime comparisons this replaces missed the case that actually happened: on
2026-08-14 the YAML-header fix changed what the build produces without changing
any input's mtime, so neither shell script would ever have rebuilt.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import freshness as F


class Stamp(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rime = Path(self.tmp.name)
        (self.rime / "cn_dicts").mkdir()
        self.base = self.rime / "cn_dicts" / "base.dict.yaml"
        self.base.write_text("建议\tjian yi\t500\n", encoding="utf-8")
        self.custom = self.rime / "custom.dict.yaml"
        self.custom.write_text("落盘\tluo pan\t145\n", encoding="utf-8")
        self.config = self.rime / "dict.json"
        self.config.write_text(json.dumps([
            {"dict": str(self.custom), "top": True},
            {"dict": str(self.base)},
        ]), encoding="utf-8")
        self.output = self.rime / "out.db"
        self.output.write_bytes(b"database")

    def tearDown(self):
        self.tmp.cleanup()

    def current(self):
        return F.compute_stamp(self.rime, self.config, self.output)

    def test_inputs_are_derived_from_the_config_and_include_it(self):
        keys = set(self.current()["inputs"])
        self.assertIn("dict.json", keys)
        self.assertIn("custom.dict.yaml", keys)
        self.assertIn("cn_dicts/base.dict.yaml", keys)

    def test_unchanged_needs_no_rebuild(self):
        stamp = self.current()
        self.assertIsNone(F.rebuild_reason(self.current(), stamp))

    def test_changed_input_content_triggers_a_rebuild(self):
        stamp = self.current()
        self.base.write_text("建议\tjian yi\t501\n", encoding="utf-8")
        reason = F.rebuild_reason(self.current(), stamp)
        self.assertIsNotNone(reason)
        self.assertIn("cn_dicts/base.dict.yaml", reason)

    def test_touching_a_file_without_changing_it_does_not_rebuild(self):
        # Files restored from iCloud carry fresh mtimes and identical content.
        stamp = self.current()
        self.base.touch()
        self.assertIsNone(F.rebuild_reason(self.current(), stamp))

    def test_added_dictionary_triggers_a_rebuild(self):
        stamp = self.current()
        extra = self.rime / "extra.dict.yaml"
        extra.write_text("新词\txin ci\t1\n", encoding="utf-8")
        self.config.write_text(json.dumps([
            {"dict": str(self.custom), "top": True},
            {"dict": str(self.base)},
            {"dict": str(extra)},
        ]), encoding="utf-8")
        reason = F.rebuild_reason(self.current(), stamp)
        self.assertIsNotNone(reason)
        self.assertIn("extra.dict.yaml", reason)

    def test_removed_dictionary_triggers_a_rebuild(self):
        stamp = self.current()
        self.config.write_text(json.dumps([{"dict": str(self.base)}]), encoding="utf-8")
        self.assertIsNotNone(F.rebuild_reason(self.current(), stamp))

    def test_recipe_version_bump_triggers_a_rebuild(self):
        stamp = self.current()
        stamp["recipe_version"] = F.RECIPE_VERSION - 1
        reason = F.rebuild_reason(self.current(), stamp)
        self.assertIn("recipe", reason)

    def test_output_altered_behind_our_back_triggers_a_rebuild(self):
        stamp = self.current()
        self.output.write_bytes(b"tampered")
        reason = F.rebuild_reason(self.current(), stamp)
        self.assertIn("output", reason)

    def test_output_deleted_triggers_a_rebuild(self):
        stamp = self.current()
        self.output.unlink()
        self.assertIsNotNone(F.rebuild_reason(self.current(), stamp))

    def test_no_stamp_at_all_triggers_a_rebuild(self):
        self.assertIsNotNone(F.rebuild_reason(self.current(), None))

    def test_stamp_round_trips(self):
        path = self.rime / F.STAMP_NAME
        stamp = self.current()
        F.write_stamp(path, stamp)
        self.assertEqual(stamp, F.read_stamp(path))

    def test_unreadable_stamp_is_treated_as_absent(self):
        path = self.rime / F.STAMP_NAME
        path.write_text("{not json", encoding="utf-8")
        self.assertIsNone(F.read_stamp(path))

    def test_missing_input_file_is_reported_not_crashed_on(self):
        self.base.unlink()
        with self.assertRaises(FileNotFoundError) as caught:
            self.current()
        self.assertIn("base.dict.yaml", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
