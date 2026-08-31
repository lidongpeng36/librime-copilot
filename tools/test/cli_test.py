"""CLI behaviour that can be checked without a network or an input method.

Only `--dry-run` paths and `status` are covered here; `fetch` and `deploy` need
the outside world and are exercised by hand in Task 8.
"""
from __future__ import annotations

import argparse
import contextlib
import io
import json
import os
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import cli
from rime_copilot import install as I
from rime_copilot import vault as V
from rime_copilot.cli import MIN_MODEL_SIZE, SOGOU_DICT_NAME, main
from rime_copilot.freshness import STAMP_NAME

# cmd_install always sources from this real repo's tools/ (it derives
# source_root from cli.py's own location, not from --rime-dir), so these
# tests point --dest at a fixture directory and pass an explicit --builder
# fixture rather than relying on paths.find_builder's discovery. They read
# this checkout's real tools/rime_copilot/*.py (harmless) and write only
# inside the fixture tempdir -- never ~/Library/Rime.

INSTALLATION = 'installation_id: "TestMac"\nsync_dir: "{sync}"\n'


@contextlib.contextmanager
def stub_personal_lexicon():
    """Avoid the real, jieba- and pypinyin-backed oracles `personal.generate()`
    reaches for whenever a caller (`cmd_personal`, hence `cmd_update` too)
    does not inject its own.

    `generate()` builds a `clean.Lexicon` via `clean.load_lexicon()`
    unconditionally when not given one -- not only when a mined word actually
    needs the segmenter -- so ANY test that reaches `cmd_personal` needs jieba
    unless this is patched, even one whose corpus mines nothing at all. Same
    fix as `CleanFixture.run_clean` applies to `clean.load_lexicon` for
    `clean`, below. `dictfile.auto_pinyin` is patched alongside it for the
    same reason: `generate()` never lets the CLI inject a `reading` function,
    so any mined word that survives to become an entry is read through the
    real pypinyin unless this is stubbed too.
    """
    from rime_copilot.clean import Lexicon

    def fake_reading(word: str) -> str:
        return " ".join(f"py{index}" for index, _ in enumerate(word))

    fake_lexicon = Lexicon(known={}, chart=None, segment=lambda word: [word],
                           tags=lambda word: [])
    with mock.patch("rime_copilot.clean.load_lexicon", return_value=fake_lexicon), \
         mock.patch("rime_copilot.dictfile.auto_pinyin", side_effect=fake_reading):
        yield


class CliBase(unittest.TestCase):
    # A real, tab-separated dictionary row -- not the generic
    # "content of <rel>" placeholder every other vaulted file gets -- so
    # dictfile.read_entries (which cmd_personal calls) never has to fall back
    # to auto_pinyin (pypinyin) just to parse the fixture. See setUp below.
    CUSTOM_DICT_NAME = "custom.dict.yaml"
    CUSTOM_DICT_FIXTURE_BODY = "词条\tci tiao\t7\n"

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
        # custom.dict.yaml is not just any vaulted file: cmd_personal parses it
        # with dictfile.read_entries, and a one-column placeholder line forces
        # every reader through auto_pinyin (pypinyin) before being dropped for
        # containing whitespace -- pypinyin then becomes a hard dependency of
        # every CliBase test, not just the ones that mean to exercise it.
        # Real tab-separated content sidesteps that entirely.
        (self.rime / "private" / self.CUSTOM_DICT_NAME).write_text(
            self.CUSTOM_DICT_FIXTURE_BODY, encoding="utf-8")
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

    def hand_the_vault_to_another_machine(self, rel):
        """Make the vault's copy of `rel` look like another machine wrote it."""
        store = self.sync / "copilot_vault"
        records = V.read_manifest(store)
        records[rel] = V.Record(sha256=records[rel].sha256, size=records[rel].size,
                                backed_up_at="2026-08-20T06:35:31Z", machine="Mac-Mini")
        V.write_manifest(store, records)

    def test_backing_up_over_another_machines_copy_exits_non_zero(self):
        self.run_cli("backup")
        self.hand_the_vault_to_another_machine("private/custom.dict.yaml")
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        code, out = self.run_cli("backup")
        self.assertNotEqual(0, code)
        self.assertIn("conflict", out)
        self.assertIn("private/custom.dict.yaml", out)

    def test_a_refused_backup_names_the_machine_it_would_have_overwritten(self):
        self.run_cli("backup")
        self.hand_the_vault_to_another_machine("private/custom.dict.yaml")
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        _, out = self.run_cli("backup")
        self.assertIn("Mac-Mini", out)

    def test_a_refused_backup_leaves_the_vault_untouched(self):
        self.run_cli("backup")
        self.hand_the_vault_to_another_machine("private/custom.dict.yaml")
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        self.run_cli("backup")
        stored = self.sync / "copilot_vault" / "files" / "private/custom.dict.yaml"
        self.assertEqual(self.CUSTOM_DICT_FIXTURE_BODY,
                         stored.read_text(encoding="utf-8"))

    def test_force_backs_up_over_another_machines_copy(self):
        self.run_cli("backup")
        self.hand_the_vault_to_another_machine("private/custom.dict.yaml")
        (self.rime / "private/custom.dict.yaml").write_text("newer\n", encoding="utf-8")
        code, _ = self.run_cli("backup", "--force")
        self.assertEqual(0, code)
        stored = self.sync / "copilot_vault" / "files" / "private/custom.dict.yaml"
        self.assertEqual("newer\n", stored.read_text(encoding="utf-8"))

    def test_editing_a_file_you_backed_up_yourself_still_just_works(self):
        self.run_cli("backup")
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        code, _ = self.run_cli("backup")
        self.assertEqual(0, code)
        stored = self.sync / "copilot_vault" / "files" / "private/custom.dict.yaml"
        self.assertEqual("mine\n", stored.read_text(encoding="utf-8"))


class LexiconStatus(CliBase):
    """`status` must check the stamp against the file, not just read it.

    The clean stamp is vaulted, so it reaches every machine -- including one
    that never applied the cleaning it describes. Mac-Mini reported
    `cleaned 2026-08-17T09:13:29Z, 8231 entries` while holding the 1MB
    pristine export, because the line was printed from the stamp alone.
    """

    def lexicon(self) -> Path:
        return self.rime / "private" / "custom.dict.yaml"

    def write_stamp(self, *, result: str, raw: str) -> None:
        (self.rime / "private" / ".copilot_clean_stamp.json").write_text(
            json.dumps({"cleaned_at": "2026-08-17T09:13:29Z",
                        "raw_sha256": raw, "result_sha256": result,
                        "counts": {"surviving": 8231}}), encoding="utf-8")

    def report(self) -> str:
        """Just the `lexicon:` line and its continuations.

        Asserting against the whole status output is how a test passes for
        the wrong reason -- "missing" matches the `missing-in-vault` lines
        above, and `restore` appears in half a dozen hints.
        """
        _, out = self.run_cli("status")
        lines = out.splitlines()
        start = next(i for i, line in enumerate(lines)
                     if line.startswith("lexicon:"))
        block = [lines[start]]
        for line in lines[start + 1:]:
            if line.startswith(" "):
                block.append(line)
            else:
                break
        return "\n".join(block)

    def test_a_lexicon_matching_the_stamp_is_reported_cleaned(self):
        self.write_stamp(result=V.sha256_file(self.lexicon()), raw="0" * 64)
        report = self.report()
        self.assertIn("cleaned 2026-08-17T09:13:29Z", report)
        self.assertIn("8231", report)

    def test_a_matching_lexicon_draws_no_warning(self):
        self.write_stamp(result=V.sha256_file(self.lexicon()), raw="0" * 64)
        self.assertEqual(1, len(self.report().splitlines()))

    def test_holding_the_pristine_export_is_not_reported_as_cleaned(self):
        self.write_stamp(result="0" * 64, raw=V.sha256_file(self.lexicon()))
        self.assertIn("pristine export", self.report())

    def test_holding_the_pristine_export_names_the_command_that_fixes_it(self):
        self.write_stamp(result="0" * 64, raw=V.sha256_file(self.lexicon()))
        self.assertIn("restore", self.report())

    def test_a_lexicon_matching_neither_hash_is_reported_as_a_mismatch(self):
        self.write_stamp(result="0" * 64, raw="1" * 64)
        self.assertIn("matches neither", self.report())

    def test_a_stamp_with_no_lexicon_at_all_is_reported(self):
        self.write_stamp(result="0" * 64, raw="1" * 64)
        self.lexicon().unlink()
        self.assertIn("missing", self.report())

    def test_an_old_stamp_without_hashes_still_reports_cleaned(self):
        # Stamps written before the hashes existed must not start reporting
        # a mismatch they have no way to disprove.
        (self.rime / "private" / ".copilot_clean_stamp.json").write_text(
            json.dumps({"cleaned_at": "2026-08-17T09:13:29Z",
                        "counts": {"surviving": 8231}}), encoding="utf-8")
        report = self.report()
        self.assertIn("cleaned 2026-08-17T09:13:29Z", report)
        self.assertEqual(1, len(report.splitlines()))


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


class FetchModel(CliBase):
    """`_fetch_model_write` is the sole network boundary (mirrors how Fetch
    above patches around `scel.download_all` rather than mocking `requests`
    internals) -- every test here replaces it, so none ever reaches the
    network. `MIN_MODEL_SIZE` is patched down to a few bytes so a fake
    "download" does not actually write hundreds of MB to a tempdir on every
    test run; the real constant is exercised by the too-small test below,
    which stays below whatever the patched floor is.
    """

    FAKE_FLOOR = 64

    def setUp(self):
        super().setUp()
        self.enterContext(mock.patch.object(cli, "MIN_MODEL_SIZE", self.FAKE_FLOOR))

    def fake_write(self, size: int = FAKE_FLOOR + 1):
        def write(url, dest):
            dest.write_bytes(b"\0" * size)
            return size
        return write

    def test_downloads_to_private_and_prints_the_config_line(self):
        with mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=self.fake_write()) as fake:
            code, out = self.run_cli("fetch-model")
        self.assertEqual(0, code)
        fake.assert_called_once()
        (url, dest_arg), _ = fake.call_args
        self.assertEqual(cli.MODEL_URL, url)

        target = self.rime / "private" / cli.MODEL_NAME
        self.assertTrue(target.is_file())
        # Written through a .part path and renamed, not written in place --
        # a stray .part left behind would mean the atomic-rename step ran
        # against the wrong path.
        self.assertFalse(target.with_name(target.name + ".part").exists())
        self.assertIn(str(target), out)
        self.assertIn(f"model: private/{cli.MODEL_NAME}", out)

    def test_already_present_is_skipped_without_touching_the_network(self):
        target = self.rime / "private" / cli.MODEL_NAME
        target.write_bytes(b"\0" * (self.FAKE_FLOOR + 1))
        with mock.patch("rime_copilot.cli._fetch_model_write") as fake:
            code, out = self.run_cli("fetch-model")
        self.assertEqual(0, code)
        fake.assert_not_called()
        self.assertIn("already present, skipping", out)
        self.assertIn(str(target), out)
        # Skipping still tells the user the config line to paste -- that is
        # the whole point of running the command on a machine that already
        # has the file.
        self.assertIn(f"model: private/{cli.MODEL_NAME}", out)

    def test_dry_run_writes_nothing(self):
        with mock.patch("rime_copilot.cli._fetch_model_write") as fake:
            code, out = self.run_cli("--dry-run", "fetch-model")
        self.assertEqual(0, code)
        fake.assert_not_called()
        self.assertFalse((self.rime / "private" / cli.MODEL_NAME).exists())

    def test_force_redownloads_an_already_present_file(self):
        target = self.rime / "private" / cli.MODEL_NAME
        target.write_bytes(b"\0" * (self.FAKE_FLOOR + 1))
        with mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=self.fake_write()) as fake:
            code, _ = self.run_cli("fetch-model", "--force")
        self.assertEqual(0, code)
        fake.assert_called_once()

    def test_a_download_far_short_of_a_real_model_is_refused(self):
        # Sanity floor against an HTML error page or a truncated connection
        # silently becoming the "model" copilot/rerank/llm/model points at.
        with mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=self.fake_write(size=1)):
            code, out = self.run_cli("fetch-model")
        self.assertNotEqual(0, code)
        self.assertIn("refusing to install", out)
        self.assertFalse((self.rime / "private" / cli.MODEL_NAME).exists())

    def test_download_failure_is_reported_not_raised(self):
        with mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=RuntimeError("network unreachable")):
            code, out = self.run_cli("fetch-model")
        self.assertNotEqual(0, code)
        self.assertIn("fetch-model failed", out)
        self.assertFalse((self.rime / "private" / cli.MODEL_NAME).exists())
        # No half-written file left behind for a later run to trip over.
        self.assertFalse(
            (self.rime / "private" / (cli.MODEL_NAME + ".part")).exists())

    def test_name_and_url_overrides_are_honoured(self):
        with mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=self.fake_write()) as fake:
            code, out = self.run_cli("fetch-model", "--name", "other.gguf",
                                     "--url", "https://example.invalid/other.gguf")
        self.assertEqual(0, code)
        (url, _), _ = fake.call_args
        self.assertEqual("https://example.invalid/other.gguf", url)
        self.assertTrue((self.rime / "private" / "other.gguf").is_file())
        self.assertIn("model: private/other.gguf", out)

    def test_the_real_min_model_size_rejects_a_far_undersized_download(self):
        # The one test in this class that exercises the actual production
        # constant rather than the patched-down FAKE_FLOOR -- without it,
        # every other test in here could pass against a MIN_MODEL_SIZE of 0
        # and nothing would notice.
        with mock.patch.object(cli, "MIN_MODEL_SIZE", MIN_MODEL_SIZE), \
             mock.patch("rime_copilot.cli._fetch_model_write",
                        side_effect=self.fake_write(size=1024)):
            code, out = self.run_cli("fetch-model")
        self.assertNotEqual(0, code)
        self.assertIn("refusing to install", out)


class Update(CliBase):
    def test_fetch_failure_is_fatal_with_no_existing_dictionary_to_fall_back_on(self):
        with mock.patch("rime_copilot.cli.scel.download_all",
                        side_effect=RuntimeError("network unreachable")):
            code, out = self.run_cli("update",
                                     "--builder", str(self.fake_builder_that_writes()),
                                     "--squirrel", str(self.fake_squirrel()),
                                     "--corpus-dir", str(self.rime.parent / "corpus"))
        self.assertNotEqual(0, code)
        self.assertFalse((self.rime / "private" / "private.predict.db").exists())

    def test_fetch_failure_is_non_fatal_when_a_dictionary_already_exists(self):
        (self.rime / "private" / SOGOU_DICT_NAME).write_text(
            "existing\txian you\t1\n", encoding="utf-8")
        with mock.patch("rime_copilot.cli.scel.download_all",
                        side_effect=RuntimeError("network unreachable")), \
             stub_personal_lexicon():
            code, out = self.run_cli("update",
                                     "--builder", str(self.fake_builder_that_writes()),
                                     "--squirrel", str(self.fake_squirrel()),
                                     "--corpus-dir", str(self.rime.parent / "corpus"))
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


class InstallInterpreter(CliBase):
    """`install` defaults to `sys.executable`, which under pyenv is whatever
    a parent directory's `.python-version` names -- resolved from the
    *caller's* cwd, not the checkout's. `--python` is how you say which
    interpreter you actually meant, and both the plan and `status` name the
    one in force so a wrong pick is visible before it costs a subcommand.
    """

    def other_interpreter(self) -> Path:
        # A real, runnable interpreter at a path distinguishable from
        # sys.executable, so "the flag was honoured" is an observable fact
        # and not just "the default happened to be the same binary".
        #
        # A symlink specifically, which also pins down that the given path is
        # *not* resolved through to its target: a virtualenv's `bin/python3`
        # is exactly this shape, and resolving it would pin the base
        # interpreter -- the one without the environment's packages, i.e.
        # the very failure --python exists to fix.
        link = Path(self.tmp.name) / "other-python3"
        link.symlink_to(sys.executable)
        return link

    def shebang(self) -> str:
        entry_point = self.rime / "private" / "bin" / "rime-copilot"
        return entry_point.read_text(encoding="utf-8").splitlines()[0]

    def test_python_flag_pins_the_named_interpreter(self):
        other = self.other_interpreter()
        code, _ = self.run_cli("install", "--builder", str(self.fake_builder()),
                               "--python", str(other))
        self.assertEqual(0, code)
        self.assertEqual(f"#!{other}", self.shebang())

    def test_with_nothing_declared_the_running_interpreter_is_pinned(self):
        # The fixture Rime dir has no .python-version above it, so this is
        # the last-resort branch, not the declared one.
        self.run_cli("install", "--builder", str(self.fake_builder()))
        self.assertEqual(f"#!{sys.executable}", self.shebang())

    def test_the_plan_names_the_interpreter_it_would_pin(self):
        other = self.other_interpreter()
        code, out = self.run_cli("--dry-run", "install", "--builder",
                                 str(self.fake_builder()), "--python", str(other))
        self.assertEqual(0, code)
        self.assertIn(str(other), out)

    def test_a_nonexistent_interpreter_is_refused_and_installs_nothing(self):
        missing = Path(self.tmp.name) / "no-such-python3"
        code, out = self.run_cli("install", "--builder", str(self.fake_builder()),
                                 "--python", str(missing))
        self.assertEqual(1, code)
        self.assertIn(str(missing), out)
        self.assertIsNone(I.read_install_manifest(self.rime / "private" / "bin"))

    def test_status_names_the_pinned_interpreter(self):
        other = self.other_interpreter()
        self.run_cli("install", "--builder", str(self.fake_builder()),
                     "--python", str(other))
        _, out = self.run_cli("status")
        self.assertIn(str(other), out)


class BuildWithoutConfig(CliBase):
    """`dict.json` is vaulted, so on a machine that has just run `install`
    the usual reason `build` cannot find it is that `restore` has not run
    yet -- the file is sitting in the vault, one command away. The bare
    "no <path>" left that to be remembered rather than read.
    """

    def remove_config(self) -> None:
        (self.rime / "private" / "dict.json").unlink()

    def test_points_at_the_vault_when_the_config_is_waiting_there(self):
        self.run_cli("backup")
        self.remove_config()
        code, out = self.run_cli("build", "--builder", str(self.fake_builder()))
        self.assertEqual(1, code)
        self.assertIn("dict.json", out)
        self.assertIn("restore", out)

    def test_says_nothing_about_restore_when_the_vault_has_no_config(self):
        # Pointing at a `restore` that would restore nothing is worse than
        # saying only what is true.
        self.remove_config()
        code, out = self.run_cli("build", "--builder", str(self.fake_builder()))
        self.assertEqual(1, code)
        self.assertIn("dict.json", out)
        self.assertNotIn("restore", out)

    def test_still_reports_plainly_when_the_vault_cannot_be_found(self):
        # No sync_dir in installation.yaml: the first-run state. Looking for
        # a hint must not turn a clear error into a traceback.
        (self.rime / "installation.yaml").write_text(
            'installation_id: "TestMac"\n', encoding="utf-8")
        self.remove_config()
        code, out = self.run_cli("build", "--builder", str(self.fake_builder()))
        self.assertEqual(1, code)
        self.assertIn("dict.json", out)

    def test_update_surfaces_the_same_hint(self):
        # `update` chains fetch -> build -> deploy, and this is the error it
        # actually stops on for a freshly installed machine.
        self.run_cli("backup")
        self.remove_config()
        with mock.patch("rime_copilot.cli.cmd_fetch", return_value=0), \
             stub_personal_lexicon():
            code, out = self.run_cli("update", "--builder", str(self.fake_builder()),
                                     "--corpus-dir", str(self.rime.parent / "corpus"))
        self.assertEqual(1, code)
        self.assertIn("restore", out)


class InstallDeclaredInterpreter(CliBase):
    """`~/Library/Rime/private/.python-version` already named the right
    environment while `install` was pinning whatever the caller's shell
    resolved. The destination's declaration is about *this* installation and
    travels with it, so it beats the interpreter that happens to be running.
    """

    def declare(self, version: str = "rime") -> Path:
        version_file = self.rime / "private" / ".python-version"
        version_file.write_text(f"{version}\n", encoding="utf-8")
        return version_file

    def fake_pyenv(self, answer: "Path | None" = None, fails: bool = False) -> Path:
        """A `pyenv` on PATH answering `which python3` with `answer`.

        A stub, so these tests say the same thing on a machine with no pyenv
        installed and can stage the "version is not installed" failure.
        """
        bin_dir = Path(self.tmp.name) / "fakebin"
        bin_dir.mkdir(exist_ok=True)
        script = bin_dir / "pyenv"
        if fails:
            script.write_text("#!/bin/sh\necho \"pyenv: version \\`rime' is not "
                              "installed\" >&2\nexit 1\n", encoding="utf-8")
        else:
            script.write_text(f'#!/bin/sh\n[ "$1" = which ] || exit 1\necho "{answer}"\n',
                              encoding="utf-8")
        script.chmod(0o755)
        self.enterContext(mock.patch.dict(
            os.environ, {"PATH": f"{bin_dir}:{os.environ['PATH']}"}))
        return script

    def declared_python(self) -> Path:
        link = Path(self.tmp.name) / "declared-python3"
        link.symlink_to(sys.executable)
        return link

    def shebang(self) -> str:
        entry_point = self.rime / "private" / "bin" / "rime-copilot"
        return entry_point.read_text(encoding="utf-8").splitlines()[0]

    def test_the_declaration_is_used_without_being_asked(self):
        self.declare()
        declared = self.declared_python()
        self.fake_pyenv(answer=declared)
        code, _ = self.run_cli("install", "--builder", str(self.fake_builder()))
        self.assertEqual(0, code)
        self.assertEqual(f"#!{declared}", self.shebang())

    def test_the_plan_names_the_file_the_choice_came_from(self):
        version_file = self.declare()
        self.fake_pyenv(answer=self.declared_python())
        _, out = self.run_cli("--dry-run", "install", "--builder", str(self.fake_builder()))
        self.assertIn(str(version_file), out)
        self.assertIn("rime", out)

    def test_an_explicit_python_flag_still_wins(self):
        self.declare()
        self.fake_pyenv(answer=self.declared_python())
        override = Path(self.tmp.name) / "override-python3"
        override.symlink_to(sys.executable)
        self.run_cli("install", "--builder", str(self.fake_builder()),
                     "--python", str(override))
        self.assertEqual(f"#!{override}", self.shebang())

    def test_an_unresolvable_declaration_warns_and_still_installs(self):
        version_file = self.declare()
        self.fake_pyenv(fails=True)
        code, out = self.run_cli("install", "--builder", str(self.fake_builder()))
        self.assertEqual(0, code, "a broken declaration must not strand the machine")
        self.assertIn(str(version_file), out)
        self.assertIn("not installed", out)
        self.assertEqual(f"#!{sys.executable}", self.shebang())

    def test_status_reports_a_declaration_changed_after_install(self):
        # Editing .python-version looks like it should take effect; the
        # shebang was frozen at install time and nothing re-reads it.
        self.run_cli("install", "--builder", str(self.fake_builder()))
        self.declare()
        declared = self.declared_python()
        self.fake_pyenv(answer=declared)
        _, out = self.run_cli("status")
        self.assertIn(str(declared), out)
        self.assertIn("re-run install", out)

    def test_status_is_quiet_when_the_declaration_matches(self):
        self.declare()
        self.fake_pyenv(answer=self.declared_python())
        self.run_cli("install", "--builder", str(self.fake_builder()))
        _, out = self.run_cli("status")
        self.assertNotIn("re-run install", out)


class InstallDependencies(CliBase):
    """A dependency the pinned interpreter lacks must be reported at install
    time and at every `status` -- all of them, not just `pypinyin`. Checking
    one of three is how an interpreter with `pypinyin` and no `bs4` installed
    clean and then failed in `fetch`.
    """

    ABSENT = I.Requirement("no_such_module_pqxz", "no-such-pkg", "`fetch` (the download step)")

    def install_missing_one(self, *args) -> "tuple[int, str]":
        with mock.patch.object(I, "RUNTIME_REQUIREMENTS", (self.ABSENT,)):
            return self.run_cli("install", "--builder", str(self.fake_builder()), *args)

    def test_install_names_the_missing_module_and_what_it_breaks(self):
        code, out = self.install_missing_one()
        self.assertEqual(0, code, "a missing dependency warns, it does not refuse")
        self.assertIn("no_such_module_pqxz", out)
        self.assertIn("fetch", out)

    def test_install_prints_a_pip_command_naming_the_pinned_interpreter(self):
        _, out = self.install_missing_one()
        self.assertIn("no-such-pkg", out)
        self.assertIn(f"{sys.executable} -m pip install", out)

    def test_the_dry_run_plan_warns_before_anything_is_installed(self):
        with mock.patch.object(I, "RUNTIME_REQUIREMENTS", (self.ABSENT,)):
            code, out = self.run_cli("--dry-run", "install", "--builder",
                                     str(self.fake_builder()))
        self.assertEqual(0, code)
        self.assertIn("no_such_module_pqxz", out)

    def test_status_reports_it_again_on_an_installed_copy(self):
        self.run_cli("install", "--builder", str(self.fake_builder()))
        with mock.patch.object(I, "RUNTIME_REQUIREMENTS", (self.ABSENT,)):
            _, out = self.run_cli("status")
        self.assertIn("no_such_module_pqxz", out)

    def test_a_satisfied_interpreter_gets_no_warning(self):
        present = I.Requirement("json", "json", "nothing")
        with mock.patch.object(I, "RUNTIME_REQUIREMENTS", (present,)):
            _, out = self.run_cli("install", "--builder", str(self.fake_builder()))
        self.assertNotIn("warning", out)


class CleanFixture(unittest.TestCase):
    """Shared fixture/helpers for the `clean` subcommand.

    Not itself a test class in spirit -- kept separate from `CleanCommand` so
    the guard-specific classes below (`CleanStampGuard`, `CleanThresholdGuard`,
    `CleanForceGuard`) can reuse `setUp`/`run_clean` without also inheriting
    (and re-running) every one of `CleanCommand`'s own test methods.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rime = Path(self.tmp.name)
        (self.rime / "private").mkdir(parents=True)
        (self.rime / "cn_dicts").mkdir(parents=True)
        (self.rime / "cn_dicts" / "8105.dict.yaml").write_text(
            "---\nname: chart\n...\n\n" + "".join(f"{c}\tx\t1\n" for c in "上线的问题识六一份"),
            encoding="utf-8")
        (self.rime / "private" / "custom.dict.yaml").write_text(
            "---\nname: custom\nversion: \"1\"\nsort: by_weight\n...\n\n"
            "上线\tshang xian\t2877\n"
            "的问题\tde wen ti\t45\n"
            "识六\tshi liu\t475\n"
            "一份\tyi fen\t2\n",
            encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def fake_lexicon(self):
        from rime_copilot.clean import Lexicon
        return Lexicon(known={"上线": 4210}, chart=set("上线的问题识六一份"),
                       segment=lambda w: [w], tags=lambda w: ["n"])

    def run_clean(self, *argv, dry_run=False):
        from rime_copilot import cli, clean
        real = clean.load_lexicon
        clean.load_lexicon = lambda chart_path=None: self.fake_lexicon()
        prefix = ["--rime-dir", str(self.rime)]
        if dry_run:
            prefix.append("--dry-run")
        try:
            return cli.main([*prefix, "clean", *argv])
        finally:
            clean.load_lexicon = real


class CleanCommand(CleanFixture):
    def test_dry_run_writes_nothing(self):
        self.assertEqual(0, self.run_clean(dry_run=True))
        self.assertFalse((self.rime / "private" / "clean_out").exists())

    def test_writes_review_and_drop(self):
        self.assertEqual(0, self.run_clean())
        out = self.rime / "private" / "clean_out"
        self.assertTrue((out / "review.tsv").is_file())
        self.assertTrue((out / "drop.tsv").is_file())
        self.assertIn("识六", (out / "review.tsv").read_text(encoding="utf-8"))
        self.assertIn("的问题", (out / "drop.tsv").read_text(encoding="utf-8"))

    def test_apply_rewrites_the_dictionary_and_preserves_the_original(self):
        self.run_clean()
        self.assertEqual(0, self.run_clean("--apply"))
        raw = self.rime / "private" / "custom.dict.yaml.raw"
        self.assertIn("的问题", raw.read_text(encoding="utf-8"))
        cleaned = (self.rime / "private" / "custom.dict.yaml").read_text(encoding="utf-8")
        self.assertNotIn("的问题", cleaned)
        self.assertIn("上线", cleaned)
        self.assertIn("识六", cleaned)

    def test_a_second_apply_does_not_clobber_the_pristine_original(self):
        self.run_clean()
        self.run_clean("--apply")
        raw = self.rime / "private" / "custom.dict.yaml.raw"
        first = raw.read_text(encoding="utf-8")
        self.run_clean()
        self.run_clean("--apply")
        self.assertEqual(first, raw.read_text(encoding="utf-8"))

    def test_apply_without_a_review_file_refuses(self):
        self.assertEqual(1, self.run_clean("--apply"))

    def test_apply_writes_a_stamp_that_status_reports(self):
        from rime_copilot import cli
        self.run_clean()
        self.run_clean("--apply")
        stamp = self.rime / "private" / ".copilot_clean_stamp.json"
        self.assertTrue(stamp.is_file())
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            cli.main(["--rime-dir", str(self.rime), "status"])
        self.assertIn("lexicon:", buffer.getvalue())

    def test_apply_leaves_no_staged_temporary_file_behind(self):
        self.run_clean()
        self.run_clean("--apply")
        leftover = list((self.rime / "private").glob("*.new"))
        self.assertEqual([], leftover, f"staged files left behind: {leftover}")

    def test_a_second_apply_does_not_clobber_a_truncated_raw(self):
        """Guards `not raw.exists()`: any `raw` already on disk, truncated or
        not, is treated as already preserved and left alone.

        The staged-then-renamed write this branch uses for `raw` (see the
        comment above it in `cmd_clean`) makes a truncated `raw` unreachable
        from this code path -- an interrupted `--apply` now leaves either no
        `raw` or a complete one, never a fragment. The state is still
        reachable from an older copy of this tool, or from something outside
        it (an external editor, a half-finished manual copy) writing directly
        into `raw`, which is what this test stands in for.
        """
        raw = self.rime / "private" / "custom.dict.yaml.raw"
        raw.write_text("truncated", encoding="utf-8")
        self.run_clean()
        self.run_clean("--apply")
        self.assertEqual("truncated", raw.read_text(encoding="utf-8"))


class CleanStampGuard(CleanFixture):
    """C1: a clean stamp with no matching `.raw` must never let --apply proceed.

    Reproduces the reviewer's chain: `restore` on a second machine can
    materialise `custom.dict.yaml` before `custom.dict.yaml.raw` (normal on a
    new Mac -- iCloud has not synced everything yet). Without this guard,
    `clean --apply` there would preserve the already-cleaned live file as the
    "pristine" original, and a later `backup` would push that fake original
    over the real one in the vault, destroying the only copy of the Sogou
    export everywhere.
    """

    def stamp_path(self) -> Path:
        return self.rime / "private" / ".copilot_clean_stamp.json"

    def raw_path(self) -> Path:
        return self.rime / "private" / "custom.dict.yaml.raw"

    def write_stamp(self, raw_sha256: str) -> None:
        self.stamp_path().write_text(json.dumps({
            "cleaned_at": "2026-08-01T00:00:00Z",
            "raw_sha256": raw_sha256,
            "result_sha256": "irrelevant-to-this-guard",
            "counts": {"keep": 0, "review": 0, "drop": 0, "surviving": 0},
            "thresholds": {"high": 100, "low": 3, "compound": 50},
        }), encoding="utf-8")

    def test_stamp_with_no_raw_refuses(self):
        self.run_clean()
        self.write_stamp("deadbeef" * 8)
        self.assertEqual(1, self.run_clean("--apply"))
        self.assertFalse(self.raw_path().exists())
        # The live file must be untouched by the refused apply.
        self.assertIn("的问题",
                      (self.rime / "private" / "custom.dict.yaml").read_text(encoding="utf-8"))

    def test_stamp_with_a_mismatched_raw_refuses(self):
        self.run_clean()
        self.raw_path().write_text("not the sogou export", encoding="utf-8")
        self.write_stamp("deadbeef" * 8)  # does not match the real hash of raw_path()
        self.assertEqual(1, self.run_clean("--apply"))
        self.assertEqual("not the sogou export", self.raw_path().read_text(encoding="utf-8"))

    def test_ordinary_first_run_still_succeeds(self):
        # No stamp, no .raw: the normal first-ever clean on a machine.
        self.run_clean()
        self.assertEqual(0, self.run_clean("--apply"))
        self.assertTrue(self.raw_path().is_file())

    def test_a_matching_raw_alongside_the_stamp_still_succeeds(self):
        # The stamp is present and .raw agrees with it -- e.g. a rerun after
        # `restore` has finished pulling everything down.
        self.run_clean()
        self.assertEqual(0, self.run_clean("--apply"))
        from rime_copilot.paths import sha256_file
        self.write_stamp(sha256_file(self.raw_path()))
        self.run_clean("--force")
        self.assertEqual(0, self.run_clean("--apply"))


class CleanThresholdGuard(CleanFixture):
    """I3: --apply must use the thresholds review.tsv was generated with."""

    def test_apply_with_different_thresholds_refuses(self):
        self.run_clean("--threshold-high", "2000")
        self.assertEqual(1, self.run_clean("--apply"))  # default threshold-high=100
        # Refused before anything was rewritten.
        cleaned = (self.rime / "private" / "custom.dict.yaml").read_text(encoding="utf-8")
        self.assertIn("的问题", cleaned)
        self.assertFalse((self.rime / "private" / "custom.dict.yaml.raw").exists())

    def test_apply_with_matching_thresholds_succeeds(self):
        self.run_clean("--threshold-high", "2000")
        self.assertEqual(0, self.run_clean("--apply", "--threshold-high", "2000"))

    def test_a_review_file_without_a_header_is_still_accepted(self):
        # Backward compatible with a review.tsv that predates this header.
        self.run_clean()
        review_path = self.rime / "private" / "clean_out" / "review.tsv"
        stripped = "\n".join(line for line in review_path.read_text(encoding="utf-8").splitlines()
                             if not line.startswith("# thresholds:"))
        review_path.write_text(stripped + "\n", encoding="utf-8")
        self.assertEqual(0, self.run_clean("--apply"))


class CleanMalformedReviewGuard(CleanFixture):
    """A hand-edited review.tsv must refuse as a message, not a traceback.

    parse_review already refuses a malformed row and a word decided two
    conflicting ways; this covers cmd_clean surfacing that the way every other
    refusal in it surfaces. Reached by hand-editing a 1,500-row TSV, which is
    when a stack trace is least useful.
    """

    def malformed_review(self, body):
        self.run_clean()
        review_path = self.rime / "private" / "clean_out" / "review.tsv"
        review_path.write_text(body, encoding="utf-8")
        return review_path

    def test_a_conflicting_duplicate_refuses_without_a_traceback(self):
        self.malformed_review("keep\t475\t识六\tR9\twhy\ndrop\t475\t识六\tR9\twhy\n")
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = self.run_clean("--apply")
        self.assertEqual(1, code)
        self.assertIn("refusing", buffer.getvalue())
        self.assertIn("识六", buffer.getvalue())

    def test_an_unknown_action_refuses_without_a_traceback(self):
        self.malformed_review("maybe\t475\t识六\tR9\twhy\n")
        buffer = io.StringIO()
        with contextlib.redirect_stdout(buffer):
            code = self.run_clean("--apply")
        self.assertEqual(1, code)
        self.assertIn("refusing", buffer.getvalue())

    def test_the_refusal_happens_before_anything_is_rewritten(self):
        self.malformed_review("maybe\t475\t识六\tR9\twhy\n")
        self.run_clean("--apply")
        cleaned = (self.rime / "private" / "custom.dict.yaml").read_text(encoding="utf-8")
        self.assertIn("的问题", cleaned)
        self.assertFalse((self.rime / "private" / "custom.dict.yaml.raw").exists())


class CleanForceGuard(CleanFixture):
    """I4: a bare re-run of `clean` must not silently destroy an annotated
    review.tsv -- 1,533 hand-annotated rows is real work, and the natural
    reason to re-run is exactly retuning the thresholds (I3).
    """

    def review_path(self) -> Path:
        return self.rime / "private" / "clean_out" / "review.tsv"

    def test_rerunning_without_force_refuses_and_preserves_the_annotation(self):
        self.run_clean()
        annotated = self.review_path().read_text(encoding="utf-8").replace(
            "keep\t475\t识六", "drop\t475\t识六")
        self.assertNotEqual(annotated, self.review_path().read_text(encoding="utf-8"),
                            "fixture assumption broken: expected a 识六 review row")
        self.review_path().write_text(annotated, encoding="utf-8")
        self.assertEqual(1, self.run_clean())
        self.assertEqual(annotated, self.review_path().read_text(encoding="utf-8"))

    def test_dry_run_is_exempt_from_the_force_guard(self):
        # A dry run writes nothing, so refusing it would protect annotations
        # against a write that never happens -- and `clean --dry-run` is the
        # documented first step of the sequence, which would then fail on
        # every run after the first.
        self.run_clean()
        before = self.review_path().read_text(encoding="utf-8")
        self.assertEqual(0, self.run_clean(dry_run=True))
        self.assertEqual(before, self.review_path().read_text(encoding="utf-8"))

    def test_force_regenerates_and_the_new_default_wins_on_apply(self):
        self.run_clean()
        self.review_path().write_text(
            self.review_path().read_text(encoding="utf-8").replace(
                "keep\t475\t识六", "drop\t475\t识六"),
            encoding="utf-8")
        self.assertEqual(0, self.run_clean("--force"))
        # Regenerated from scratch: back to the chain's default (keep), the
        # hand annotation is gone.
        self.assertIn("keep\t475\t识六", self.review_path().read_text(encoding="utf-8"))
        self.assertEqual(0, self.run_clean("--apply"))
        cleaned = (self.rime / "private" / "custom.dict.yaml").read_text(encoding="utf-8")
        self.assertIn("识六", cleaned)


class CleanVocabularyMismatchGuard(unittest.TestCase):
    """M5: `--apply` must refuse when `read_entries` silently dropped a row
    `count_vocabulary_lines` still counts -- that used to feed a rebuildable
    database harmlessly; here it would erase the row for good.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rime = Path(self.tmp.name)
        (self.rime / "private").mkdir(parents=True)
        (self.rime / "cn_dicts").mkdir(parents=True)
        (self.rime / "cn_dicts" / "8105.dict.yaml").write_text(
            "---\nname: chart\n...\n\n" + "".join(f"{c}\tx\t1\n" for c in "上线一份"),
            encoding="utf-8")
        (self.rime / "private" / "custom.dict.yaml").write_text(
            "---\nname: custom\nversion: \"1\"\nsort: by_weight\n...\n\n"
            "上线\tshang xian\t2877\n"
            "识六\tshi liu\tnot-a-number\n"  # unparseable weight: read_entries skips it silently
            "一份\tyi fen\t2\n",
            encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_refuses_and_writes_nothing(self):
        from rime_copilot import cli
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = cli.main(["--rime-dir", str(self.rime), "clean"])
        self.assertEqual(1, code)
        self.assertIn("3 vocabulary line", buffer.getvalue())
        self.assertFalse((self.rime / "private" / "clean_out").exists())


class VaultedFiles(unittest.TestCase):
    def test_the_pristine_export_is_vaulted(self):
        from rime_copilot.vault import VAULTED_FILES
        self.assertIn("private/custom.dict.yaml.raw", VAULTED_FILES)

    def test_the_clean_stamp_is_vaulted(self):
        # C1: without this, a second machine can never see that the lexicon
        # was already cleaned elsewhere -- see CleanStampGuard.
        from rime_copilot.vault import VAULTED_FILES
        self.assertIn("private/.copilot_clean_stamp.json", VAULTED_FILES)


if __name__ == "__main__":
    unittest.main()


class GrammarState(unittest.TestCase):
    """A schema that names a grammar whose file is absent decodes exactly as
    though no grammar were configured -- Octagram keeps a null db and Query
    returns a constant (octagram.cc:110). librime logs nothing a user reads,
    so `status` is the only place this can surface."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "build").mkdir()
        self.addCleanup(self.tmp.cleanup)

    def write_custom(self, body: str) -> None:
        (self.dir / "double_pinyin_flypy.custom.yaml").write_text(body, encoding="utf-8")

    def write_built(self, body: str) -> None:
        (self.dir / "build" / "double_pinyin_flypy.schema.yaml").write_text(body, encoding="utf-8")

    def test_no_grammar_anywhere_is_unconfigured(self):
        self.write_custom("patch:\n  melt_eng/enable_user_dict: true\n")
        state, detail = cli.grammar_state(self.dir)
        self.assertEqual(state, "unconfigured")
        self.assertIn("no language model", detail)

    def test_configured_with_the_file_present_is_ok(self):
        self.write_custom("patch:\n  grammar/language: private/zh-hans-t-essay-bgw\n")
        (self.dir / "private").mkdir()
        (self.dir / "private" / "zh-hans-t-essay-bgw.gram").write_bytes(b"x" * 7)
        state, detail = cli.grammar_state(self.dir)
        self.assertEqual(state, "ok")
        self.assertIn("7 bytes", detail)

    def test_configured_with_the_file_absent_is_missing(self):
        self.write_custom("patch:\n  grammar/language: private/zh-hans-t-essay-bgw\n")
        state, detail = cli.grammar_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("MISSING", detail)
        # naming the file that asked for it is the actionable half
        self.assertIn("double_pinyin_flypy.custom.yaml", detail)

    def test_the_suffix_is_appended_not_expected_in_the_name(self):
        # octagram declares ResourceType{"gram_db", "", ".gram"} and appends
        # it, so `language` carries no suffix. Looking for the name verbatim
        # would report every correct configuration as missing.
        self.write_custom("patch:\n  grammar/language: private/zh-hans-t-essay-bgw\n")
        (self.dir / "private").mkdir()
        (self.dir / "private" / "zh-hans-t-essay-bgw.gram").write_bytes(b"x")
        self.assertEqual(cli.grammar_state(self.dir)[0], "ok")

    def test_the_deployed_schema_form_is_read_too(self):
        # The built schema writes a nested block, not the patch's flat key.
        self.write_built('grammar:\n  language: "private/zh-hans-t-essay-bgw"\n')
        self.assertEqual(cli.grammar_state(self.dir)[0], "missing")

    def test_a_language_key_outside_a_grammar_block_is_ignored(self):
        # `language:` is a common key; only the one under `grammar:` names a
        # gram db, and mistaking another would report a phantom missing file.
        self.write_built("translator:\n  language: pinyin\n")
        self.assertEqual(cli.grammar_state(self.dir)[0], "unconfigured")

    def test_quotes_and_trailing_comments_are_stripped(self):
        self.write_custom('patch:\n  "grammar/language": private/zh  # why\n')
        state, detail = cli.grammar_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("private/zh MISSING", detail)


class ModelState(unittest.TestCase):
    """librime does not fail when a schema names a model that is not there:
    LlmScorer logs one load failure and never retries, the fallback chain
    reads that as kNoModel, and re-ranking quietly uses the db -- which from
    outside is indistinguishable from a working install."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "build").mkdir()
        self.addCleanup(self.tmp.cleanup)

    def write(self, body: str) -> None:
        (self.dir / "double_pinyin_flypy.custom.yaml").write_text(body, encoding="utf-8")

    def test_no_model_anywhere_is_unconfigured(self):
        self.write("patch:\n  melt_eng/enable_user_dict: true\n")
        self.assertEqual(cli.model_state(self.dir)[0], "unconfigured")

    def test_configured_and_present_is_ok(self):
        self.write('patch:\n  "copilot/rerank/llm/model": private/rime40m-q8.gguf\n')
        (self.dir / "private").mkdir()
        (self.dir / "private" / "rime40m-q8.gguf").write_bytes(b"GGUF" + b"x" * 16)
        state, detail = cli.model_state(self.dir)
        self.assertEqual(state, "ok")
        self.assertIn("rime40m-q8.gguf", detail)

    def test_configured_and_absent_is_missing_and_names_the_file(self):
        self.write('patch:\n  "copilot/rerank/llm/model": private/rime40m-q8.gguf\n')
        state, detail = cli.model_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("MISSING", detail)
        self.assertIn("double_pinyin_flypy.custom.yaml", detail)

    def test_a_bare_model_key_is_ignored(self):
        # `model:` appears under other blocks; only the qualified patch key
        # unambiguously names the re-ranking model.
        self.write("patch:\n  translator:\n    model: something\n")
        self.assertEqual(cli.model_state(self.dir)[0], "unconfigured")

    def test_a_commented_out_model_is_ignored(self):
        self.write('patch:\n  # "copilot/rerank/llm/model": private/old.gguf\n')
        self.assertEqual(cli.model_state(self.dir)[0], "unconfigured")

    def write_built(self, body: str) -> None:
        (self.dir / "build" / "double_pinyin_flypy.schema.yaml").write_text(body, encoding="utf-8")

    def test_the_nested_patch_form_is_read_too(self):
        # A patch may write the copilot subtree hierarchically instead of as
        # flat "a/b/c" keys -- the whole node is replaced either way, so both
        # are correct, and a check that sees only one reports a configured
        # model as `unconfigured`.
        self.write(
            "patch:\n"
            "  copilot:\n"
            "    rerank:\n"
            "      llm:\n"
            "        model: private/rime40m-q8.gguf\n"
        )
        state, detail = cli.model_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("private/rime40m-q8.gguf", detail)

    def test_the_deployed_flow_style_map_is_read_too(self):
        # Rime's deployer emits small maps in flow style, so the deployed
        # schema puts the model on the same line as its block's key. A
        # line-anchored `model:` match never sees it.
        self.write_built(
            "copilot:\n"
            "  rerank:\n"
            '    llm: {context_chars: 32, enable: true, '
            'model: "private/rime40m-q8.gguf", top_n: 4}\n'
        )
        state, detail = cli.model_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("private/rime40m-q8.gguf", detail)

    def test_the_prediction_model_is_not_mistaken_for_the_rerank_model(self):
        # copilot/llm/model names the next-word prediction model, a different
        # feature. Counting it here would report `ok` for a re-ranking model
        # that is not configured at all.
        self.write(
            "patch:\n"
            "  copilot:\n"
            "    llm:\n"
            "      model: private/Qwen3-0.6B-q4_K_M.gguf\n"
        )
        self.assertEqual(cli.model_state(self.dir)[0], "unconfigured")

    def test_the_install_model_hint_uses_the_nested_form(self):
        buf = io.StringIO()
        with redirect_stdout(buf):
            cli._print_install_model_config(self.dir / "private" / "rime40m-q8.gguf")
        self.assertIn("rerank:", buf.getvalue())

    def test_the_install_model_hint_is_a_patch_status_can_read(self):
        # The hint and the check must not drift apart: whatever install-model
        # tells the user to paste has to be what `status` then reports as ok.
        (self.dir / "private").mkdir()
        (self.dir / "private" / "rime40m-q8.gguf").write_bytes(b"GGUF")
        buf = io.StringIO()
        with redirect_stdout(buf):
            cli._print_install_model_config(self.dir / "private" / "rime40m-q8.gguf")
        pasted = [l for l in buf.getvalue().splitlines() if l.startswith("  ")]
        self.write("patch:\n" + "\n".join(pasted) + "\n")
        self.assertEqual(cli.model_state(self.dir)[0], "ok")


class InstallModel(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)
        self.src = self.dir / "m.gguf"
        self.src.write_bytes(b"GGUF" + b"\0" * 32)
        self.rime = self.dir / "rime"
        (self.rime / "private").mkdir(parents=True)

    def args(self, **kw):
        return argparse.Namespace(source=str(self.src), rime_dir=self.rime, name=None,
                                  force=False, dry_run=False, **kw)

    def test_copies_into_private_and_prints_the_patch(self):
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            self.assertEqual(0, cli.cmd_install_model(self.args()))
        self.assertTrue((self.rime / "private" / "m.gguf").is_file())
        # Asserted through the same reader `status` uses, not as a substring:
        # the hint is only useful if pasting it makes `status` say `ok`, and
        # that is a property of the paths it resolves to, not of its wording.
        pasted = "\n".join(l for l in buffer.getvalue().splitlines() if l.startswith("  "))
        self.assertIn((cli.RERANK_MODEL_KEY, "private/m.gguf"),
                      list(cli._config_leaves("patch:\n" + pasted)))

    def test_refuses_a_file_that_is_not_gguf(self):
        # Installing the wrong file surfaces only as re-ranking silently
        # using the db, which looks exactly like a working install.
        self.src.write_bytes(b"not a model")
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            self.assertEqual(1, cli.cmd_install_model(self.args()))
        self.assertFalse((self.rime / "private" / "m.gguf").exists())

    def test_refuses_a_missing_source(self):
        args = self.args()
        args.source = str(self.dir / "nope.gguf")
        self.assertEqual(1, cli.cmd_install_model(args))

    def test_existing_file_is_left_alone_without_force(self):
        (self.rime / "private" / "m.gguf").write_bytes(b"GGUF" + b"old")
        with redirect_stdout(io.StringIO()):
            cli.cmd_install_model(self.args())
        self.assertEqual(b"GGUF" + b"old", (self.rime / "private" / "m.gguf").read_bytes())

    def test_force_overwrites(self):
        (self.rime / "private" / "m.gguf").write_bytes(b"GGUF" + b"old")
        args = self.args()
        args.force = True
        with redirect_stdout(io.StringIO()):
            cli.cmd_install_model(args)
        self.assertEqual(self.src.read_bytes(), (self.rime / "private" / "m.gguf").read_bytes())

    def test_leaves_no_part_file_behind(self):
        with redirect_stdout(io.StringIO()):
            cli.cmd_install_model(self.args())
        self.assertEqual([], list((self.rime / "private").glob("*.part")))


class VaultConflictExplanation(unittest.TestCase):
    """`conflict` is two very different situations wearing one word, and the
    consequence of the common one is silent: another machine's `restore`
    quietly brings down the older content and reports success. That is what
    happened to double_pinyin_flypy.custom.yaml after a day of config edits,
    and `status` said only "local content is not in the vault"."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        root = Path(self.tmp.name)
        self.rime = root / "rime"
        self.store = root / "vault"
        (self.store / "files").mkdir(parents=True)
        self.rime.mkdir()
        (self.rime / "squirrel.custom.yaml").write_text("local\n", encoding="utf-8")
        (self.store / "files" / "squirrel.custom.yaml").write_text("vaulted\n", encoding="utf-8")

    def record(self, machine: str):
        from rime_copilot import vault as v
        v.write_manifest(self.store, {"squirrel.custom.yaml": v.Record(
            sha256="x", size=8, backed_up_at="2026-08-20T07:26:24Z", machine=machine)})

    def status(self, machine_id: str) -> str:
        args = argparse.Namespace(rime_dir=self.rime, dry_run=False)
        buffer = io.StringIO()
        with contextlib.ExitStack() as stack:
            stack.enter_context(mock.patch.object(cli.paths, "vault_dir",
                                                  return_value=self.store))
            stack.enter_context(mock.patch.object(cli.paths, "machine_id",
                                                  return_value=machine_id))
            stack.enter_context(redirect_stdout(buffer))
            cli.cmd_status(args)
        return buffer.getvalue()

    def test_our_own_stale_backup_names_the_command_that_fixes_it(self):
        self.record("this-machine")
        out = self.status("this-machine")
        self.assertIn("edited here since your own backup", out)
        self.assertIn("rime-copilot backup", out)
        # and says what it costs, which is the part that was silent
        self.assertIn("OLDER copy", out)

    def test_another_machines_copy_is_not_something_backup_fixes(self):
        self.record("other-machine")
        out = self.status("this-machine")
        self.assertIn("other-machine", out)
        self.assertIn("reconcile by hand", out)
        self.assertNotIn("edited here since your own backup", out)


class EnabledSchemas(unittest.TestCase):
    """Which schemas the running IME actually loads.

    `build/` accumulates: a schema built once stays there forever, so a Rime
    dir that has been through a few schema_list edits holds build outputs for
    schemas nothing loads any more. Scanning all of them makes `status` report
    missing files for configurations that are not running -- noise on every
    run, in the one command whose value is that every line is worth reading.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "build").mkdir()
        self.addCleanup(self.tmp.cleanup)

    def write_built(self, name: str, body: str) -> None:
        (self.dir / "build" / f"{name}.schema.yaml").write_text(body, encoding="utf-8")

    def test_reads_the_deployed_schema_list(self):
        (self.dir / "build" / "default.yaml").write_text(
            "schema_list:\n  - schema: double_pinyin_flypy\n  - schema: stroke\nswitcher:\n"
            "  caption: x\n", encoding="utf-8")
        self.assertEqual(cli.enabled_schemas(self.dir), {"double_pinyin_flypy", "stroke"})

    def test_falls_back_to_the_patch_when_nothing_is_deployed(self):
        # A machine that has never deployed still has an answer, and it is the
        # one that will take effect.
        (self.dir / "default.custom.yaml").write_text(
            'patch:\n  schema_list:\n    - schema: double_pinyin_flypy  # comment\n'
            '  "key_binder/bindings/+":\n    - { accept: Control+p }\n', encoding="utf-8")
        self.assertEqual(cli.enabled_schemas(self.dir), {"double_pinyin_flypy"})

    def test_no_list_anywhere_is_None_not_an_empty_set(self):
        # None means "cannot tell" and the callers must fall back to scanning
        # everything. An empty set would silently narrow to nothing and turn
        # every real missing file into an ok.
        self.assertIsNone(cli.enabled_schemas(self.dir))

    def test_a_dependency_of_an_enabled_schema_counts(self):
        # `schema/dependencies` is loaded by the running IME without ever
        # appearing in schema_list -- double_pinyin_flypy pulls in melt_eng
        # this way. Filtering on schema_list alone would drop it.
        (self.dir / "build" / "default.yaml").write_text(
            "schema_list:\n  - schema: double_pinyin_flypy\n", encoding="utf-8")
        self.write_built("double_pinyin_flypy",
                         "schema:\n  author:\n    - Dvel\n  dependencies:\n"
                         "    - melt_eng\n    - radical_pinyin\n")
        self.assertEqual(cli.enabled_schemas(self.dir),
                         {"double_pinyin_flypy", "melt_eng", "radical_pinyin"})

    def test_dependencies_are_followed_transitively(self):
        (self.dir / "build" / "default.yaml").write_text(
            "schema_list:\n  - schema: a\n", encoding="utf-8")
        self.write_built("a", "schema:\n  dependencies:\n    - b\n")
        self.write_built("b", "schema:\n  dependencies:\n    - c\n")
        self.write_built("c", "schema:\n  name: c\n")
        self.assertEqual(cli.enabled_schemas(self.dir), {"a", "b", "c"})

    def test_a_dependency_cycle_terminates(self):
        (self.dir / "build" / "default.yaml").write_text(
            "schema_list:\n  - schema: a\n", encoding="utf-8")
        self.write_built("a", "schema:\n  dependencies:\n    - b\n")
        self.write_built("b", "schema:\n  dependencies:\n    - a\n")
        self.assertEqual(cli.enabled_schemas(self.dir), {"a", "b"})


class DisabledSchemasAreNotScanned(unittest.TestCase):
    """The grammar and model checks read only what the IME loads.

    Both had the same bug: they globbed every `build/*.schema.yaml`, so the
    stock luna_pinyin/bopomofo/terra schemas -- built once at install and
    disabled ever since -- reported `zh-hant-t-essay-bgw MISSING` forever on a
    machine whose only enabled schema was correctly configured.
    """

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        (self.dir / "build").mkdir()
        (self.dir / "build" / "default.yaml").write_text(
            "schema_list:\n  - schema: double_pinyin_flypy\n", encoding="utf-8")
        self.addCleanup(self.tmp.cleanup)

    def built(self, name: str, body: str) -> None:
        (self.dir / "build" / f"{name}.schema.yaml").write_text(body, encoding="utf-8")

    def test_a_disabled_schemas_grammar_is_ignored(self):
        self.built("luna_pinyin", "grammar:\n  language: zh-hant-t-essay-bgw\n")
        self.built("double_pinyin_flypy", "schema:\n  name: flypy\n")
        self.assertEqual(cli.grammar_state(self.dir)[0], "unconfigured")

    def test_an_enabled_schemas_grammar_is_still_reported(self):
        self.built("double_pinyin_flypy", "grammar:\n  language: private/zh-hans\n")
        state, detail = cli.grammar_state(self.dir)
        self.assertEqual(state, "missing")
        self.assertIn("private/zh-hans", detail)

    def test_a_dependency_schemas_grammar_is_reported(self):
        self.built("double_pinyin_flypy", "schema:\n  dependencies:\n    - melt_eng\n")
        self.built("melt_eng", "grammar:\n  language: private/zh-hans\n")
        self.assertEqual(cli.grammar_state(self.dir)[0], "missing")

    def test_a_disabled_schemas_patch_is_ignored_too(self):
        # The patch form is where a human writes it, so it needs the same gate.
        self.built("luna_pinyin", "schema:\n  name: luna\n")
        (self.dir / "luna_pinyin.custom.yaml").write_text(
            "patch:\n  grammar/language: zh-hant-t-essay-bgw\n", encoding="utf-8")
        self.assertEqual(cli.grammar_state(self.dir)[0], "unconfigured")

    def test_a_disabled_schemas_model_is_ignored(self):
        self.built("luna_pinyin",
                   'patch:\n  "copilot/rerank/llm/model": private/gone.gguf\n')
        self.assertEqual(cli.model_state(self.dir)[0], "unconfigured")

    def test_default_custom_yaml_is_not_mistaken_for_a_disabled_schema(self):
        # `default` and `squirrel` are not schemas; dropping them because they
        # are not in schema_list would be filtering on a name collision.
        (self.dir / "default.custom.yaml").write_text(
            "patch:\n  grammar/language: private/zh-hans\n", encoding="utf-8")
        self.assertEqual(cli.grammar_state(self.dir)[0], "missing")


class PersonalCommandTest(unittest.TestCase):
    def test_writes_the_dictionary_into_private(self):
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "custom.dict.yaml").write_text(
                "---\nname: custom\n...\n\n词库\tche duan\t40\n", encoding="utf-8")
            corpus = rime_dir / "corpus"
            corpus.mkdir()
            with stub_personal_lexicon():
                code = cli.main(["--rime-dir", str(rime_dir), "personal",
                                 "--corpus-dir", str(corpus)])
            self.assertEqual(code, 0)
            written = rime_dir / "private" / "personal.dict.yaml"
            self.assertTrue(written.is_file())
            self.assertIn("name: personal", written.read_text(encoding="utf-8"))

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "custom.dict.yaml").write_text(
                "---\nname: custom\n...\n\n词库\tche duan\t40\n", encoding="utf-8")
            code = cli.main(["--rime-dir", str(rime_dir), "--dry-run", "personal",
                             "--corpus-dir", str(rime_dir / "corpus")])
            self.assertEqual(code, 0)
            self.assertFalse((rime_dir / "private" / "personal.dict.yaml").exists())

    def test_refuses_when_there_is_neither_a_lexicon_nor_a_corpus(self):
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            code = cli.main(["--rime-dir", str(rime_dir), "personal",
                             "--corpus-dir", str(rime_dir / "corpus")])
            self.assertEqual(code, 1)

    def test_creates_private_when_only_the_corpus_exists(self):
        # A machine can have a corpus but no private/ yet (custom.dict.yaml
        # absent) -- cmd_personal must create the directory write_dict needs
        # rather than raise a bare FileNotFoundError.
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            corpus = rime_dir / "corpus"
            corpus.mkdir()
            (corpus / "a.jsonl").write_text(
                json.dumps({"id": "1", "text": "词库联调"}, ensure_ascii=False) + "\n"
                + json.dumps({"id": "2", "text": "词库联调"}, ensure_ascii=False) + "\n",
                encoding="utf-8")
            self.assertFalse((rime_dir / "private").exists())
            with stub_personal_lexicon():
                code = cli.main(["--rime-dir", str(rime_dir), "personal",
                                 "--corpus-dir", str(corpus)])
            self.assertEqual(code, 0)
            self.assertTrue((rime_dir / "private" / "personal.dict.yaml").is_file())

    def test_no_corpus_and_an_existing_dictionary_is_left_alone(self):
        # Finding 2: a machine with no corpus that already holds a
        # corpus-mined personal.dict.yaml (brought down by `restore`) must
        # not have `update` silently overwrite it with a strictly worse,
        # custom-only rebuild.
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "custom.dict.yaml").write_text(
                "---\nname: custom\n...\n\n词库\tche duan\t40\n", encoding="utf-8")
            output = rime_dir / "private" / "personal.dict.yaml"
            good_content = "---\nname: personal\n...\n\n笔记\tbi ji\t123456\n"
            output.write_text(good_content, encoding="utf-8")

            code, out = self.run_cli(rime_dir, "personal",
                                     "--corpus-dir", str(rime_dir / "corpus"))
            self.assertEqual(code, 0)
            self.assertEqual(good_content, output.read_text(encoding="utf-8"))
            self.assertIn("leaving", out)
            self.assertIn(str(output), out)

    def test_no_corpus_and_no_existing_dictionary_still_builds_from_custom(self):
        # The other half of Finding 2: a genuinely first run (no corpus, no
        # personal.dict.yaml yet) is not the hazard, and must still work.
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "custom.dict.yaml").write_text(
                "---\nname: custom\n...\n\n词库\tche duan\t40\n", encoding="utf-8")
            with stub_personal_lexicon():
                code, out = self.run_cli(rime_dir, "personal",
                                         "--corpus-dir", str(rime_dir / "corpus"))
            self.assertEqual(code, 0)
            self.assertTrue((rime_dir / "private" / "personal.dict.yaml").is_file())
            self.assertIn("note:", out)

    def test_the_version_is_stable_across_identical_regenerations(self):
        # Finding 1: a date-derived version churned on the first `update` of
        # every calendar day even though the vocabulary never changed. The
        # generated file (including its version line) must be byte-identical
        # across repeated runs against unchanged inputs.
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "custom.dict.yaml").write_text(
                "---\nname: custom\n...\n\n词库\tche duan\t40\n", encoding="utf-8")
            output = rime_dir / "private" / "personal.dict.yaml"

            with stub_personal_lexicon():
                self.run_cli(rime_dir, "personal",
                            "--corpus-dir", str(rime_dir / "corpus"))
            first = output.read_bytes()
            output.unlink()
            with stub_personal_lexicon():
                self.run_cli(rime_dir, "personal",
                            "--corpus-dir", str(rime_dir / "corpus"))
            second = output.read_bytes()
            self.assertEqual(first, second)

    def run_cli(self, rime_dir: Path, *args) -> "tuple[int, str]":
        buffer = io.StringIO()
        with redirect_stdout(buffer):
            code = cli.main(["--rime-dir", str(rime_dir), *args])
        return code, buffer.getvalue()


class StatusImportTableTest(unittest.TestCase):
    def test_status_names_an_import_table_that_is_not_on_disk(self):
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private.dict.yaml").write_text(
                "---\nname: private\nimport_tables:\n"
                "  - private/personal\n...\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                cli.main(["--rime-dir", str(rime_dir), "status"])
            printed = output.getvalue()
            self.assertIn("private/personal", printed)
            self.assertIn("dictionary build will fail", printed)

    def test_status_is_quiet_when_every_import_table_exists(self):
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "personal.dict.yaml").write_text(
                "---\nname: personal\n...\n", encoding="utf-8")
            (rime_dir / "private.dict.yaml").write_text(
                "---\nname: private\nimport_tables:\n"
                "  - private/personal\n...\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                cli.main(["--rime-dir", str(rime_dir), "status"])
            self.assertNotIn("dictionary build will fail", output.getvalue())

    def test_import_table_entries_with_trailing_comments_are_parsed_bare(self):
        # The real private.dict.yaml writes every entry with a trailing
        # `# comment` (e.g. `- cn_dicts/8105     # 字表`). A parser that
        # slices off the leading `- ` and keeps the rest (`stripped[2:]`)
        # returns the comment text along with the name, so every table
        # compares unequal to the file on disk and status raises a false
        # alarm on a perfectly healthy machine.
        with tempfile.TemporaryDirectory() as raw:
            rime_dir = Path(raw)
            (rime_dir / "private").mkdir()
            (rime_dir / "private" / "personal.dict.yaml").write_text(
                "---\nname: personal\n...\n", encoding="utf-8")
            (rime_dir / "private.dict.yaml").write_text(
                "---\nname: private\nimport_tables:\n"
                "  - private/personal     # 个人词库\n...\n", encoding="utf-8")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                cli.main(["--rime-dir", str(rime_dir), "status"])
            self.assertNotIn("dictionary build will fail", output.getvalue())


class ContextMemoryStatusTest(unittest.TestCase):
    def _line(self, body: "str | None", enabled: bool = True) -> "str | None":
        with tempfile.TemporaryDirectory() as d:
            conf = Path(d) / "tmux.conf"
            if body is not None:
                conf.write_text(body)
            return cli._context_memory_status(conf, enabled)

    def _hooks(self, script: str, flag: str = "-ga", args: bool = True) -> str:
        tail = ' \\"#{pane_id}\\" \\"#{pane_current_command}\\" \\"#{socket_path}\\"' if args else ""
        return "".join(
            f"set-hook {flag} {hook} 'run-shell -b \"{script}{tail}\"'\n"
            for hook in cli._CTX_HOOKS)

    def _script(self, d: Path, *, executable: bool = True) -> Path:
        path = d / "rime_ctx_report.sh"
        path.write_text("#!/bin/sh\n")
        if executable:
            path.chmod(0o755)
        return path

    def test_reports_missing_tmux_hook(self):
        line = self._line("set -g mouse on\n")
        self.assertIn("polling", line)
        self.assertIn("after-select-pane", line)

    def test_reports_present_tmux_hook(self):
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d))
            line = self._line(self._hooks(str(script)))
        self.assertIn("hook installed", line)
        self.assertNotIn("polling", line)

    def test_reports_a_hook_that_does_not_pass_the_pane_in(self):
        """The pre-2026-08-31 hook form, which asks tmux instead of being told.

        `display-message -p` with no `-t` resolves against the INVOKING
        client, so a scripted pane switch makes the reporter name a pane in
        whatever session issued the command. Measured on tmux 3.7c: a switch
        to `copilot:3` (%5) reported `librime:1 %3 claude`. Interactive
        switches are correct by accident -- there the invoking client is the
        one being switched -- so this never shows up in ordinary use and
        never shows up in a log. A status line is the only way it surfaces.
        """
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d))
            line = self._line(self._hooks(str(script), args=False))
        self.assertIn("#{pane_id}", line)
        self.assertNotIn("hook installed (", line)

    def test_hook_passing_the_pane_in_is_reported_as_installed(self):
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d))
            line = self._line(self._hooks(str(script), args=True))
        self.assertIn("hook installed", line)
        self.assertNotIn("#{pane_id}", line)

    def test_reports_half_installed_hook(self):
        line = self._line(
            "set-hook -ga after-select-pane 'run-shell -b \"rime_ctx_report.sh\"'\n"
        )
        self.assertIn("after-select-window", line)
        self.assertIn("polling", line)

    def test_reports_absent_tmux_conf(self):
        line = self._line(None)
        self.assertIn("polling", line)

    # `set-hook -g` REPLACES an existing hook of that name; `-ga` appends, and
    # is what the README now recommends. Matching the literal `set-hook -g `
    # reported `missing` on a machine configured the right way.
    def test_accepts_the_flag_variants_a_correct_config_uses(self):
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d))
            for flag in ("-g", "-ga", "-g -a", "-ga  "):
                with self.subTest(flag=flag):
                    self.assertIn("hook installed", self._line(self._hooks(str(script), flag)))

    # The hook names a path, and matching the set-hook lines alone reported
    # `hook installed` for a hook pointing into a checkout that had since been
    # moved -- at which point it fires and does nothing, forever.
    def test_flags_a_hook_naming_a_script_that_is_not_there(self):
        line = self._line(self._hooks("/nonexistent/rime_ctx_report.sh"))
        self.assertIn("does not exist", line)

    def test_flags_a_hook_naming_a_script_that_is_not_executable(self):
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d), executable=False)
            line = self._line(self._hooks(str(script)))
        self.assertIn("not executable", line)

    # An off-by-default feature must not print a line on every run on every
    # machine; that is how "read every line of status" stops being a
    # discipline. Silence, unless the user did half the setup.
    def test_silent_when_the_feature_is_off_and_nothing_was_configured(self):
        self.assertIsNone(self._line("set -g mouse on\n", enabled=False))
        self.assertIsNone(self._line(None, enabled=False))

    def test_speaks_up_when_the_hooks_are_installed_but_the_feature_is_off(self):
        with tempfile.TemporaryDirectory() as d:
            script = self._script(Path(d))
            line = self._line(self._hooks(str(script)), enabled=False)
        self.assertIsNotNone(line)
        self.assertIn("off", line)
        self.assertIn(cli.CTX_MEMORY_ENABLE_KEY, line)

    def _enabled(self, body: str) -> bool:
        with tempfile.TemporaryDirectory() as d:
            built = Path(d) / "x.schema.yaml"
            built.write_text(body)
            return cli._context_memory_enabled([built])

    # The same three shapes `_config_leaves` exists for: a flat patch key, a
    # nested block, and the flow map Rime's deployer writes.
    def test_enable_is_read_in_every_shape_the_key_reaches_a_schema_in(self):
        self.assertTrue(self._enabled('"copilot/context_memory/enable": true\n'))
        self.assertTrue(self._enabled("copilot:\n  context_memory:\n    enable: true\n"))
        self.assertTrue(self._enabled("copilot:\n  context_memory: {enable: true, debug: false}\n"))
        self.assertFalse(self._enabled("copilot:\n  context_memory:\n    enable: false\n"))
        self.assertFalse(self._enabled("copilot:\n  rerank:\n    enable: true\n"))

    def _order(self, processors: "list[str]") -> str:
        with tempfile.TemporaryDirectory() as d:
            built = Path(d) / "x.schema.yaml"
            body = "engine:\n  processors:\n" + "".join(f"    - {p}\n" for p in processors)
            built.write_text(body)
            return cli._processor_order_status([built])

    def test_order_ok_when_copilot_precedes_ascii_composer(self):
        line = self._order(["copilot", "ascii_composer", "key_binder"])
        self.assertIn("ok", line)

    def test_order_flagged_when_copilot_follows_ascii_composer(self):
        line = self._order(["ascii_composer", "copilot", "key_binder"])
        self.assertIn("ascii_composer", line)
        self.assertIn("x.schema.yaml", line)

    def test_order_silent_when_copilot_absent(self):
        # A schema that does not use this plugin is not a misconfiguration.
        line = self._order(["ascii_composer", "key_binder"])
        self.assertIn("n/a", line)
