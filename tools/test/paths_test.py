"""Path and installation.yaml resolution.

Never point these at ~/Library/Rime: it is the live input method
configuration. Everything here runs inside a temporary directory.
"""
from __future__ import annotations

import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import paths

INSTALLATION = '''distribution_code_name: Squirrel
installation_id: "MacBookPro-M4Pro"
rime_version: 1.16.0
sync_dir: "/tmp/does-not-need-to-exist/sync"
'''


class InstallationYaml(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.rime = Path(self.tmp.name)
        (self.rime / "installation.yaml").write_text(INSTALLATION, encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def test_reads_quoted_scalar(self):
        self.assertEqual("MacBookPro-M4Pro", paths.machine_id(self.rime))

    def test_reads_sync_dir(self):
        self.assertEqual(Path("/tmp/does-not-need-to-exist/sync"),
                         paths.read_sync_dir(self.rime))

    def test_reads_unquoted_scalar(self):
        (self.rime / "installation.yaml").write_text(
            "installation_id: BareWord\n", encoding="utf-8")
        self.assertEqual("BareWord", paths.machine_id(self.rime))

    def test_vault_dir_is_a_flat_sibling_not_under_installation_id(self):
        # Rime's own sync task owns <sync_dir>/<installation_id>/. Writing
        # there would put our files inside something Rime prunes.
        vault = paths.vault_dir(self.rime)
        self.assertEqual(Path("/tmp/does-not-need-to-exist/sync/copilot_vault"), vault)
        self.assertNotIn("MacBookPro-M4Pro", str(vault))

    def test_missing_key_raises_with_the_file_named(self):
        (self.rime / "installation.yaml").write_text("rime_version: 1.16.0\n",
                                                     encoding="utf-8")
        with self.assertRaises(LookupError) as caught:
            paths.read_sync_dir(self.rime)
        self.assertIn("installation.yaml", str(caught.exception))

    def test_missing_file_raises_with_the_path_named(self):
        (self.rime / "installation.yaml").unlink()
        with self.assertRaises(FileNotFoundError) as caught:
            paths.machine_id(self.rime)
        self.assertIn(str(self.rime), str(caught.exception))


class ResolveRimeDir(unittest.TestCase):
    def test_explicit_wins(self):
        self.assertEqual(Path("/a/b"), paths.resolve_rime_dir("/a/b"))

    def test_env_var_next(self):
        os.environ["RIME_DIR"] = "/from/env"
        self.addCleanup(os.environ.pop, "RIME_DIR", None)
        self.assertEqual(Path("/from/env"), paths.resolve_rime_dir(None))

    def test_default_last(self):
        os.environ.pop("RIME_DIR", None)
        self.assertEqual(Path.home() / "Library" / "Rime",
                         paths.resolve_rime_dir(None))

    def test_expands_tilde(self):
        self.assertEqual(Path.home() / "x", paths.resolve_rime_dir("~/x"))


class FindBuilder(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def _make(self, rel: str) -> Path:
        p = self.root / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text("#!/bin/sh\n", encoding="utf-8")
        p.chmod(0o755)
        return p

    def test_explicit_path_wins(self):
        exe = self._make("somewhere/build_copilot")
        self.assertEqual(exe, paths.find_builder(str(exe)))

    def test_explicit_missing_path_raises(self):
        with self.assertRaises(FileNotFoundError):
            paths.find_builder(str(self.root / "nope"))

    def test_walks_up_to_the_librime_build_tree(self):
        exe = self._make("librime/build/plugins/copilot/bin/build_copilot")
        start = self.root / "librime/plugins/copilot/tools/rime_copilot"
        start.mkdir(parents=True)
        # Both sides resolved: find_builder resolves its start path, and on
        # macOS tempfile hands out /var/... which resolves to /private/var/...
        self.assertEqual(exe.resolve(), paths.find_builder(None, start=start))

    def test_error_names_the_binary_and_how_to_get_it(self):
        start = self.root / "detached"
        start.mkdir()
        os.environ.pop("COPILOT_BUILD_DIR", None)
        with self.assertRaises(FileNotFoundError) as caught:
            paths.find_builder(None, start=start)
        message = str(caught.exception)
        self.assertIn("build_copilot", message)
        self.assertIn("cmake", message)

    def test_never_accepts_build_predict(self):
        # The retired binary predates 549c6a9's fix to CopilotDb::Build().
        self._make("librime/build/plugins/copilot/bin/build_predict")
        start = self.root / "librime/plugins/copilot/tools/rime_copilot"
        start.mkdir(parents=True)
        os.environ.pop("COPILOT_BUILD_DIR", None)
        with self.assertRaises(FileNotFoundError):
            paths.find_builder(None, start=start)


class Sha256File(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_matches_the_known_digest_of_an_empty_file(self):
        empty = self.dir / "empty"
        empty.write_bytes(b"")
        self.assertEqual(
            "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855",
            paths.sha256_file(empty))

    def test_differs_when_one_byte_differs(self):
        a, b = self.dir / "a", self.dir / "b"
        a.write_bytes(b"content")
        b.write_bytes(b"contenu")
        self.assertNotEqual(paths.sha256_file(a), paths.sha256_file(b))

    def test_reads_files_larger_than_one_chunk(self):
        big = self.dir / "big"
        big.write_bytes(b"x" * ((1 << 20) + 7))
        self.assertEqual(64, len(paths.sha256_file(big)))


if __name__ == "__main__":
    unittest.main()
