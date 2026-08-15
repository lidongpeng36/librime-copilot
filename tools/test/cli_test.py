"""CLI behaviour that can be checked without a network or an input method.

Only `--dry-run` paths and `status` are covered here; `fetch` and `deploy` need
the outside world and are exercised by hand in Task 8.
"""
from __future__ import annotations

import io
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import vault as V
from rime_copilot.cli import main
from rime_copilot.freshness import STAMP_NAME

INSTALLATION = 'installation_id: "TestMac"\nsync_dir: "{sync}"\n'


class CliBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.rime = root / "Rime"
        self.sync = root / "sync"
        (self.rime / "private").mkdir(parents=True)
        (self.rime / "cn_dicts").mkdir(parents=True)
        (self.rime / "installation.yaml").write_text(
            INSTALLATION.format(sync=self.sync), encoding="utf-8")
        for rel in V.VAULTED_FILES:
            target = self.rime / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"content of {rel}\n", encoding="utf-8")
        base = self.rime / "cn_dicts" / "base.dict.yaml"
        base.write_text("建议\tjian yi\t500\n", encoding="utf-8")
        (self.rime / "private" / "dict.json").write_text(
            json.dumps([{"dict": str(base)}]), encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args) -> "tuple[int, str]":
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(["--rime-dir", str(self.rime), *args])
        return code, buffer.getvalue()


class Status(CliBase):
    def test_reports_that_a_build_is_needed_and_changes_nothing(self):
        code, out = self.run_cli("status")
        self.assertEqual(0, code)
        self.assertIn("no build stamp", out)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())

    def test_reports_vault_state(self):
        code, out = self.run_cli("status")
        self.assertIn("custom.dict.yaml", out)


class Backup(CliBase):
    def test_dry_run_writes_nothing(self):
        code, out = self.run_cli("--dry-run", "backup")
        self.assertEqual(0, code)
        self.assertIn("backup", out)
        self.assertFalse(self.sync.exists())

    def test_backup_then_status_shows_nothing_to_do(self):
        self.assertEqual(0, self.run_cli("backup")[0])
        _, out = self.run_cli("status")
        self.assertNotIn("conflict", out)


class Restore(CliBase):
    def test_conflict_exits_non_zero_and_names_the_file(self):
        self.run_cli("backup")
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        code, out = self.run_cli("restore")
        self.assertNotEqual(0, code)
        self.assertIn("private/custom.dict.yaml", out)
        self.assertIn("conflict", out)
        # A refusal must not have touched the file.
        self.assertEqual("mine\n",
                         (self.rime / "private/custom.dict.yaml").read_text(encoding="utf-8"))

    def test_force_succeeds(self):
        self.run_cli("backup")
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        code, _ = self.run_cli("restore", "--force")
        self.assertEqual(0, code)

    def test_restore_of_a_missing_file_succeeds(self):
        self.run_cli("backup")
        (self.rime / "private/custom.dict.yaml").unlink()
        code, _ = self.run_cli("restore")
        self.assertEqual(0, code)
        self.assertTrue((self.rime / "private/custom.dict.yaml").is_file())


class Build(CliBase):
    def test_dry_run_reports_the_reason_and_builds_nothing(self):
        code, out = self.run_cli("--dry-run", "build")
        self.assertEqual(0, code)
        self.assertIn("no build stamp", out)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())
        self.assertFalse((self.rime / "private" / STAMP_NAME).exists())

    def test_missing_config_is_reported_not_raised(self):
        # `status` guards this (StatusSurvivesABrokenConfig); `build` must
        # too -- a missing dict.json must not surface as an uncaught
        # traceback out of freshness.compute_stamp. Found by hand while
        # running `--dry-run build` against a real ~/Library/Rime whose
        # dict.json lives elsewhere.
        (self.rime / "private" / "dict.json").unlink()
        code, out = self.run_cli("--dry-run", "build")
        self.assertNotEqual(0, code)
        self.assertIn("dict.json", out)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())

    def test_dictionary_named_in_config_but_missing_is_reported_not_raised(self):
        # dict.json itself is present -- unlike the test above -- but names a
        # dictionary that does not exist. This is the state a fresh machine
        # passes through: `restore` brings back dict.json before the
        # dictionaries it names exist. compute_stamp raises the same
        # FileNotFoundError as the missing-dict.json case; cmd_build must
        # guard this call too, the same way cmd_status already does.
        (self.rime / "cn_dicts" / "base.dict.yaml").unlink()
        code, out = self.run_cli("--dry-run", "build")
        self.assertNotEqual(0, code)
        self.assertIn("base.dict.yaml", out)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())


class StatusSurvivesABrokenConfig(CliBase):
    def test_missing_dictionary_is_reported_not_raised(self):
        # `status` is the command you run when something is wrong. It must
        # never be the thing that crashes.
        (self.rime / "cn_dicts" / "base.dict.yaml").unlink()
        code, out = self.run_cli("status")
        self.assertEqual(0, code)
        self.assertIn("base.dict.yaml", out)


if __name__ == "__main__":
    unittest.main()
