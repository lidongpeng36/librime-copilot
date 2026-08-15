"""Installing the CLI into <rime_dir>/private/bin, standalone.

An installed copy is a second copy of tools/rime_copilot/, and copies are
exactly what rotted the original private/bin/ (see task-9-brief.md, "The cost
this must mitigate"). These tests exercise the manifest/drift mechanism that
is meant to make that rot detectable rather than merely discouraged.

Everything here lives inside tempfile.TemporaryDirectory(); nothing touches
~/Library/Rime, and source_root is always a synthetic fixture tree, never
this repository's real tools/ directory.
"""
from __future__ import annotations

import os
import shutil
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import install as I
from rime_copilot import paths


def _write(path: Path, content: str, executable: bool = False) -> Path:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8")
    if executable:
        path.chmod(0o755)
    return path


class InstallFixture(unittest.TestCase):
    """A synthetic tools/ checkout plus a fake compiled builder."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.source_root = root / "checkout" / "tools"
        self.dest = root / "installed"
        self.builder = root / "build" / paths.BUILDER_NAME

        _write(self.source_root / "rime-copilot", "#!/usr/bin/env python3\nentry v1\n",
               executable=True)
        _write(self.source_root / "rime_copilot" / "__init__.py", "")
        _write(self.source_root / "rime_copilot" / "paths.py", "paths v1\n")
        _write(self.source_root / "rime_copilot" / "cli.py", "cli v1\n")
        # Never a payload file, but its presence is what marks source_root as
        # a checkout rather than an installed copy (which never has it).
        (self.source_root / "test").mkdir(parents=True)
        _write(self.builder, "#!/bin/sh\necho fake build_copilot\n", executable=True)

    def tearDown(self):
        self.tmp.cleanup()

    def install(self, commit="abc123", now="2026-08-15T00:00:00Z"):
        I.apply_install(self.source_root, self.dest, self.builder, commit, now)


class PayloadFiles(InstallFixture):
    def test_lists_the_entry_point_and_every_module(self):
        files = I.payload_files(self.source_root)
        self.assertEqual(
            {"rime-copilot", "rime_copilot/__init__.py", "rime_copilot/paths.py",
             "rime_copilot/cli.py"},
            set(files))

    def test_picks_up_a_module_added_without_code_changes(self):
        before = set(I.payload_files(self.source_root))
        self.assertNotIn("rime_copilot/dictdb.py", before)
        _write(self.source_root / "rime_copilot" / "dictdb.py", "dictdb v1\n")
        after = set(I.payload_files(self.source_root))
        self.assertIn("rime_copilot/dictdb.py", after)

    def test_excludes_pycache(self):
        # __pycache__ holds .pyc files, but plant a .py-named file inside it
        # too: payload_files must not descend into it regardless of name.
        _write(self.source_root / "rime_copilot" / "__pycache__" / "paths.cpython-311.pyc",
               "not real bytecode")
        _write(self.source_root / "rime_copilot" / "__pycache__" / "sneaky.py",
               "should never be picked up\n")
        for rel in I.payload_files(self.source_root):
            self.assertNotIn("__pycache__", rel)


class IsSourceCheckout(InstallFixture):
    def test_the_fixture_checkout_qualifies(self):
        self.assertTrue(I.is_source_checkout(self.source_root))

    def test_an_installed_copy_does_not_qualify(self):
        # install into self.dest, then ask whether *that* looks like a
        # checkout -- it must not, since apply_install never writes test/.
        self.install()
        self.assertTrue((self.dest / "rime_copilot").is_dir())
        self.assertFalse(I.is_source_checkout(self.dest))

    def test_a_bare_directory_does_not_qualify(self):
        empty = Path(self.tmp.name) / "nothing"
        empty.mkdir()
        self.assertFalse(I.is_source_checkout(empty))


class ApplyInstallRefusesLaundering(InstallFixture):
    def test_apply_install_refuses_a_source_that_is_not_a_checkout(self):
        not_a_checkout = self.dest  # install target, not yet installed: no test/, no rime_copilot/
        with self.assertRaises(ValueError):
            I.apply_install(not_a_checkout, Path(self.tmp.name) / "dest2", self.builder,
                            "abc123", "2026-08-15T00:00:00Z")

    def test_refuses_installing_from_an_installed_copy(self):
        # The installed copy is the realistic laundering case: it has
        # rime_copilot/ (that's what got installed) but never test/.
        self.install()
        with self.assertRaises(ValueError):
            I.apply_install(self.dest, Path(self.tmp.name) / "dest2", self.builder,
                            "abc123", "2026-08-15T00:00:00Z")


class RoundTrip(InstallFixture):
    def test_drift_is_empty_immediately_after_install(self):
        self.install()
        self.assertEqual([], I.drift(self.dest))

    def test_manifest_records_source_root_and_commit(self):
        self.install(commit="deadbeef")
        installed = I.read_install_manifest(self.dest)
        self.assertEqual("deadbeef", installed.source_commit)
        self.assertEqual(str(self.source_root), installed.source_root)

    def test_manifest_records_none_commit_when_none_given(self):
        self.install(commit=None)
        installed = I.read_install_manifest(self.dest)
        self.assertIsNone(installed.source_commit)

    def test_installed_files_are_readable_copies(self):
        self.install()
        self.assertEqual("cli v1\n", (self.dest / "rime_copilot" / "cli.py").read_text())

    def test_builder_is_installed_alongside_the_package(self):
        self.install()
        self.assertTrue((self.dest / paths.BUILDER_NAME).is_file())

    def test_builder_keeps_its_executable_bit(self):
        self.install()
        installed_builder = self.dest / paths.BUILDER_NAME
        self.assertTrue(os.access(installed_builder, os.X_OK), installed_builder)


class EntryPointShebang(InstallFixture):
    """Regression coverage for pyenv resolving `#!/usr/bin/env python3`
    relative to the caller's cwd, not the script's location: `install` must
    pin the installed entry point's shebang to the absolute path of the
    interpreter that ran install, not copy the source's bare shebang
    through.
    """

    def test_shebang_is_the_installing_interpreter_not_env_python3(self):
        self.install()
        first_line = (self.dest / "rime-copilot").read_text().splitlines()[0]
        self.assertEqual(f"#!{sys.executable}", first_line)
        self.assertNotEqual("#!/usr/bin/env python3", first_line)

    def test_fresh_install_reports_no_drift(self):
        # Guards the manifest-hashing hazard: if the manifest recorded the
        # *source* file's hash while the installed file's shebang got
        # rewritten, this would immediately show "edited in place" for an
        # install nobody has touched.
        self.install()
        self.assertEqual([], I.drift(self.dest))

    def test_payload_modules_are_byte_identical_to_their_sources(self):
        # Only the entry point's first line is rewritten -- every
        # rime_copilot/*.py module is copied through untouched.
        self.install()
        for rel in ("rime_copilot/__init__.py", "rime_copilot/paths.py",
                    "rime_copilot/cli.py"):
            self.assertEqual((self.source_root / rel).read_bytes(),
                             (self.dest / rel).read_bytes(), rel)

    def test_entry_point_remains_executable(self):
        self.install()
        installed_entry_point = self.dest / "rime-copilot"
        self.assertTrue(os.access(installed_entry_point, os.X_OK), installed_entry_point)


class Drift(InstallFixture):
    def test_editing_an_installed_file_in_place_is_named(self):
        self.install()
        (self.dest / "rime_copilot" / "cli.py").write_text("tampered\n", encoding="utf-8")
        lines = I.drift(self.dest)
        self.assertTrue(any("cli.py" in line for line in lines), lines)

    def test_unedited_files_are_not_named_alongside_the_edited_one(self):
        self.install()
        (self.dest / "rime_copilot" / "cli.py").write_text("tampered\n", encoding="utf-8")
        lines = I.drift(self.dest)
        self.assertFalse(any("paths.py" in line for line in lines), lines)

    def test_deleting_an_installed_file_is_named(self):
        self.install()
        (self.dest / "rime_copilot" / "paths.py").unlink()
        lines = I.drift(self.dest)
        self.assertTrue(any("paths.py" in line for line in lines), lines)

    def test_changing_the_source_after_install_is_named(self):
        self.install()
        # dest is untouched; only the repo's copy changes.
        (self.source_root / "rime_copilot" / "paths.py").write_text(
            "paths v2\n", encoding="utf-8")
        lines = I.drift(self.dest)
        self.assertTrue(any("paths.py" in line for line in lines), lines)

    def test_source_change_is_not_reported_as_a_local_edit(self):
        # Distinguishes "the repo moved on" from "someone edited the install"
        # -- both name the file, but only a real local edit changes the
        # installed copy's own hash. This is the scenario a naive
        # implementation (comparing dest to repo only, ignoring the
        # manifest) would get right by accident; assert on the dest content
        # itself to make sure that isn't what's happening.
        self.install()
        (self.source_root / "rime_copilot" / "paths.py").write_text(
            "paths v2\n", encoding="utf-8")
        I.drift(self.dest)
        self.assertEqual("paths v1\n", (self.dest / "rime_copilot" / "paths.py").read_text())

    def test_drift_does_not_raise_when_source_root_is_gone(self):
        self.install()
        shutil.rmtree(self.source_root)
        try:
            lines = I.drift(self.dest)
        except Exception as exc:  # pragma: no cover - failure path
            self.fail(f"drift() raised {exc!r} instead of tolerating a missing source_root")
        self.assertEqual([], lines)

    def test_drift_still_reports_a_local_edit_with_source_root_gone(self):
        # The "must not raise" guard must not become "must not check
        # anything": editing the install and then deleting the checkout
        # should still surface the edit, since that check needs no repo.
        self.install()
        (self.dest / "rime_copilot" / "cli.py").write_text("tampered\n", encoding="utf-8")
        shutil.rmtree(self.source_root)
        lines = I.drift(self.dest)
        self.assertTrue(any("cli.py" in line for line in lines), lines)

    def test_drift_of_an_uninstalled_dest_is_empty(self):
        self.assertEqual([], I.drift(self.dest))

    def test_read_install_manifest_is_none_before_install(self):
        self.assertIsNone(I.read_install_manifest(self.dest))


class ManifestWrittenLast(InstallFixture):
    # payload_files() for this fixture sorts to
    # ["rime-copilot", "rime_copilot/__init__.py", "rime_copilot/cli.py",
    #  "rime_copilot/paths.py"], then the builder. The entry point is
    # installed via a shebang rewrite (write_bytes + copymode), not
    # shutil.copy2, so it is not among the mocked calls below -- copy2 fires
    # once each for __init__.py, cli.py, paths.py, then the builder. Failing
    # on the third call lets the entry point plus two payload modules land
    # successfully first, so this genuinely exercises "partway" -- not
    # "before anything" (n==1) and not "after everything, only the manifest
    # write itself failed" (n==4).
    _FAIL_ON_CALL = 3

    def _install_with_failure_partway(self):
        real_copy2 = shutil.copy2
        calls = {"n": 0}

        def flaky_copy2(src, dst, *a, **kw):
            calls["n"] += 1
            if calls["n"] == self._FAIL_ON_CALL:
                raise RuntimeError("simulated failure partway through install")
            return real_copy2(src, dst, *a, **kw)

        with mock.patch("rime_copilot.install.shutil.copy2", side_effect=flaky_copy2):
            with self.assertRaises(RuntimeError):
                self.install()
        return calls

    def test_no_manifest_exists_after_a_failure_partway_through(self):
        calls = self._install_with_failure_partway()
        self.assertGreaterEqual(calls["n"], self._FAIL_ON_CALL,
                                "the fixture never reached the third file copy")
        self.assertFalse((self.dest / I.INSTALL_MANIFEST).exists())
        self.assertIsNone(I.read_install_manifest(self.dest))

    def test_files_copied_before_the_failure_are_left_on_disk(self):
        # The manifest is what says "not installed" -- apply_install does
        # not need to roll back partial file writes, only avoid claiming
        # completion. Pin that down so a future "clean up on failure" change
        # doesn't get read as breaking this contract by accident.
        self._install_with_failure_partway()
        self.assertTrue((self.dest / "rime-copilot").is_file())
        self.assertTrue((self.dest / "rime_copilot" / "__init__.py").is_file())
        self.assertTrue((self.dest / "rime_copilot" / "cli.py").is_file())
        self.assertFalse((self.dest / "rime_copilot" / "paths.py").exists())


if __name__ == "__main__":
    unittest.main()
