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
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import install as I
from rime_copilot import vault as V
from rime_copilot.cli import SOGOU_DICT_NAME, main
from rime_copilot.freshness import STAMP_NAME

# cmd_install always sources from this real repo's tools/ (it derives
# source_root from cli.py's own location, not from --rime-dir), so these
# tests point --dest at a fixture directory and pass an explicit --builder
# fixture rather than relying on paths.find_builder's discovery. They read
# this checkout's real tools/rime_copilot/*.py (harmless) and write only
# inside the fixture tempdir -- never ~/Library/Rime.

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

    def fake_builder(self) -> Path:
        # cmd_install's source_root is always this real repo's tools/, so
        # find_builder's own discovery is bypassed with an explicit path
        # rather than relying on (and being at the mercy of) a real build
        # tree existing on whatever machine runs this test.
        builder = Path(self.tmp.name) / "fake_build_copilot"
        builder.write_text("#!/bin/sh\necho fake\n", encoding="utf-8")
        builder.chmod(0o755)
        return builder

    def fake_builder_that_writes(self) -> Path:
        # Unlike fake_builder above, this one actually produces the output
        # file it is asked for (consuming stdin), for tests that need a
        # real, non-dry-run `build` to succeed end to end.
        builder = Path(self.tmp.name) / "fake_build_copilot_write"
        builder.write_text('#!/bin/sh\ncat > "$1"\n', encoding="utf-8")
        builder.chmod(0o755)
        return builder

    def fake_squirrel(self) -> Path:
        squirrel = Path(self.tmp.name) / "fake_squirrel"
        squirrel.write_text("#!/bin/sh\nexit 0\n", encoding="utf-8")
        squirrel.chmod(0o755)
        return squirrel


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

    def test_missing_in_vault_alone_does_not_block(self):
        # plan_backup silently skips a file the user doesn't have locally
        # (kind "missing-locally"), so the vault legitimately never gets it.
        # The resulting missing-in-vault on restore is not a failure -- it
        # is the round trip of a legitimately partial backup. Distinguishes
        # the fix: the pre-fix code lumps missing-in-vault in with conflict
        # and returns 1, so this assertion is false against the old code.
        self.run_cli("backup")
        vault_files = self.sync / "copilot_vault" / "files"
        (vault_files / "private" / "custom.dict.yaml").unlink()
        code, out = self.run_cli("restore")
        self.assertEqual(0, code)
        self.assertIn("missing-in-vault", out)
        self.assertIn("private/custom.dict.yaml", out)
        # The summary must name the likely causes, since a bare "not
        # restored" gives no hint whether to wait (iCloud) or shrug (never
        # backed up from this machine).
        self.assertTrue("never backed up" in out or "iCloud" in out, out)

    def test_missing_in_vault_alone_does_not_block_a_dry_run_either(self):
        self.run_cli("backup")
        vault_files = self.sync / "copilot_vault" / "files"
        (vault_files / "private" / "custom.dict.yaml").unlink()
        code, out = self.run_cli("--dry-run", "restore")
        self.assertEqual(0, code)

    def test_conflict_still_blocks_even_alongside_a_missing_in_vault_file(self):
        # A real conflict must still exit non-zero even when it is mixed in
        # with benign missing-in-vault entries -- the fix must not have
        # loosened the conflict rule while fixing missing-in-vault.
        self.run_cli("backup")
        vault_files = self.sync / "copilot_vault" / "files"
        (vault_files / "private" / "custom.dict.yaml").unlink()
        (self.rime / "squirrel.custom.yaml").write_text("mine\n", encoding="utf-8")
        code, out = self.run_cli("restore")
        self.assertNotEqual(0, code)
        self.assertIn("conflict", out)


class VaultUnavailable(unittest.TestCase):
    """installation.yaml exists but carries no `sync_dir` -- the state a
    genuinely new Mac is in, since Squirrel never writes that key (see
    ~/repo/librime/src/rime/lever/deployment_tasks.cc:105-163: it reads
    sync_dir if present and writes back installation_id/install_time/etc,
    never sync_dir itself). backup/restore must fail with an actionable
    message naming the missing key, not a raw LookupError traceback."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rime = Path(self.tmp.name) / "Rime"
        (self.rime / "private").mkdir(parents=True)
        (self.rime / "installation.yaml").write_text(
            'installation_id: "TestMac"\n', encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def run_cli(self, *args) -> "tuple[int, str]":
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = main(["--rime-dir", str(self.rime), *args])
        return code, buffer.getvalue()

    def test_restore_fails_actionably_not_with_a_traceback(self):
        code, out = self.run_cli("restore")
        self.assertNotEqual(0, code)
        self.assertIn("sync_dir", out)
        self.assertIn("installation.yaml", out)

    def test_backup_fails_actionably_not_with_a_traceback(self):
        code, out = self.run_cli("backup")
        self.assertNotEqual(0, code)
        self.assertIn("sync_dir", out)
        self.assertIn("installation.yaml", out)

    def test_missing_installation_yaml_entirely_is_also_actionable(self):
        (self.rime / "installation.yaml").unlink()
        code, out = self.run_cli("restore")
        self.assertNotEqual(0, code)
        self.assertIn("installation.yaml", out)


class Fetch(CliBase):
    def test_download_failure_is_reported_not_raised(self):
        # scel.download_all raises RuntimeError on any failed download (an
        # unreachable Sogou, or a scraper-selector drift). cmd_fetch must
        # catch it, not let it blow past cmd_fetch as an uncaught exception.
        with mock.patch("rime_copilot.cli.scel.download_all",
                        side_effect=RuntimeError("2/4 downloads failed; refusing")):
            code, out = self.run_cli("fetch")
        self.assertNotEqual(0, code)
        self.assertIn("fetch failed", out)
        self.assertFalse((self.rime / "private" / SOGOU_DICT_NAME).exists())


class Update(CliBase):
    def test_fetch_failure_is_fatal_with_no_existing_dictionary_to_fall_back_on(self):
        with mock.patch("rime_copilot.cli.scel.download_all",
                        side_effect=RuntimeError("network unreachable")):
            code, out = self.run_cli("update",
                                     "--builder", str(self.fake_builder_that_writes()),
                                     "--squirrel", str(self.fake_squirrel()))
        self.assertNotEqual(0, code)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())

    def test_fetch_failure_is_non_fatal_when_a_dictionary_already_exists(self):
        (self.rime / "private" / SOGOU_DICT_NAME).write_text(
            "existing\txian you\t1\n", encoding="utf-8")
        with mock.patch("rime_copilot.cli.scel.download_all",
                        side_effect=RuntimeError("network unreachable")):
            code, out = self.run_cli("update",
                                     "--builder", str(self.fake_builder_that_writes()),
                                     "--squirrel", str(self.fake_squirrel()))
        # Distinguishes the fix from the pre-fix behaviour: before the fix
        # `update` dies on the RuntimeError regardless of whether a good
        # sogou.dict.yaml is sitting right there, so this assertion is false
        # against the old code.
        self.assertEqual(0, code)
        self.assertIn("fetch failed", out)
        self.assertTrue((self.rime / "private" / "private.predict.db").exists())


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

    def test_config_override_reads_from_the_given_path_not_the_default(self):
        # Without --config, build reads <rime-dir>/private/dict.json -- this
        # is exactly the hardcoding a general reader of the README cannot
        # override. Point it somewhere else entirely and it must be honored.
        other_dir = Path(self.tmp.name) / "elsewhere"
        other_dir.mkdir()
        other_dict = other_dir / "other.dict.yaml"
        other_dict.write_text("独立\tdu li\t9\n", encoding="utf-8")
        other_config = other_dir / "my_dict.json"
        other_config.write_text(json.dumps([{"dict": str(other_dict)}]), encoding="utf-8")

        code, out = self.run_cli("build", "--force-build",
                                 "--builder", str(self.fake_builder_that_writes()),
                                 "--config", str(other_config))
        self.assertEqual(0, code)
        # The default dict.json (only "建议") was not what got built from.
        self.assertIn("1 words", out)

    def test_output_override_writes_to_the_given_path_not_the_default(self):
        custom_output = Path(self.tmp.name) / "custom.predict.db"
        code, out = self.run_cli("build", "--force-build",
                                 "--builder", str(self.fake_builder_that_writes()),
                                 "--output", str(custom_output))
        self.assertEqual(0, code)
        self.assertTrue(custom_output.is_file())
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())

    def test_a_custom_output_build_does_not_disturb_the_default_stamp(self):
        # Regression test: a build targeting a non-default --output/--config
        # must not overwrite the stamp that describes the default output.
        # Before the fix, both builds share one stamp file per Rime
        # directory (STAMP_NAME under private/), so the second build here
        # clobbers the first build's bookkeeping and a later `status` --
        # which always inspects the default config/output -- wrongly
        # reports the untouched default db as needing a rebuild.
        #
        # The two builds must produce genuinely different output content,
        # or a shared stamp happens to still match by coincidence (both
        # fake builders just echo their stdin, and identical entries ->
        # identical bytes) -- hence a distinct --config here, not just a
        # distinct --output, mirroring the reviewer's actual repro.
        code, _ = self.run_cli("build", "--builder", str(self.fake_builder_that_writes()))
        self.assertEqual(0, code)
        self.assertTrue((self.rime / "private" / "private.predict.db").is_file())

        other_dir = Path(self.tmp.name) / "elsewhere"
        other_dir.mkdir()
        other_dict = other_dir / "other.dict.yaml"
        other_dict.write_text("独立\tdu li\t9\n", encoding="utf-8")
        other_config = other_dir / "my_dict.json"
        other_config.write_text(json.dumps([{"dict": str(other_dict)}]), encoding="utf-8")
        custom_output = Path(self.tmp.name) / "custom.predict.db"
        code, _ = self.run_cli("build", "--force-build",
                               "--builder", str(self.fake_builder_that_writes()),
                               "--config", str(other_config),
                               "--output", str(custom_output))
        self.assertEqual(0, code)

        _, out = self.run_cli("status")
        self.assertIn("up to date", out)
        self.assertNotIn("output changed since it was built", out)

    def test_a_custom_output_build_writes_its_own_stamp_and_is_reused(self):
        custom_output = Path(self.tmp.name) / "custom.predict.db"
        code, out = self.run_cli("build", "--builder", str(self.fake_builder_that_writes()),
                                 "--output", str(custom_output))
        self.assertEqual(0, code)
        self.assertTrue(custom_output.is_file())

        code, out = self.run_cli("build", "--builder", str(self.fake_builder_that_writes()),
                                 "--output", str(custom_output))
        self.assertEqual(0, code)
        self.assertIn("up to date", out)


class StatusSurvivesABrokenConfig(CliBase):
    def test_missing_dictionary_is_reported_not_raised(self):
        # `status` is the command you run when something is wrong. It must
        # never be the thing that crashes.
        (self.rime / "cn_dicts" / "base.dict.yaml").unlink()
        code, out = self.run_cli("status")
        self.assertEqual(0, code)
        self.assertIn("base.dict.yaml", out)


class Install(CliBase):
    def test_status_reports_not_installed_on_a_fresh_fixture(self):
        _, out = self.run_cli("status")
        self.assertIn("installed: not installed", out)

    def test_dry_run_writes_nothing(self):
        code, out = self.run_cli("--dry-run", "install", "--builder", str(self.fake_builder()))
        self.assertEqual(0, code)
        self.assertFalse((self.rime / "private" / "bin").exists())
        self.assertIsNone(I.read_install_manifest(self.rime / "private" / "bin"))

    def test_status_reports_in_sync_after_install(self):
        code, _ = self.run_cli("install", "--builder", str(self.fake_builder()))
        self.assertEqual(0, code)
        _, out = self.run_cli("status")
        self.assertIn("in sync", out)
        self.assertNotIn("not installed", out)

    def test_status_names_a_drifted_file_after_one_is_edited(self):
        self.run_cli("install", "--builder", str(self.fake_builder()))
        installed_paths_py = self.rime / "private" / "bin" / "rime_copilot" / "paths.py"
        self.assertTrue(installed_paths_py.is_file())
        installed_paths_py.write_text("tampered\n", encoding="utf-8")
        _, out = self.run_cli("status")
        self.assertIn("paths.py", out)
        self.assertIn("differ", out)


if __name__ == "__main__":
    unittest.main()
