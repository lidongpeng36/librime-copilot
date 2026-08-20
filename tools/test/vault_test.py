"""Vault backup and restore.

The conflict rule is the load-bearing one: four devices share the sync
directory, so restore must never silently overwrite local content the vault
does not know about.
"""
from __future__ import annotations

import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import vault as V


class SafeJoin(unittest.TestCase):
    def test_joins_a_relative_path(self):
        self.assertEqual(Path("/root/a/b"), V.safe_join(Path("/root"), "a/b"))

    def test_rejects_parent_traversal(self):
        with self.assertRaises(ValueError):
            V.safe_join(Path("/root"), "../outside")

    def test_rejects_an_absolute_path(self):
        with self.assertRaises(ValueError):
            V.safe_join(Path("/root"), "/etc/passwd")

    def test_rejects_traversal_that_returns_inside(self):
        with self.assertRaises(ValueError):
            V.safe_join(Path("/root"), "a/../../root/b")

    def test_rejects_a_symlink_that_escapes_root(self):
        # The vault is shared by four devices. A symlink planted inside it
        # (by any of them, deliberately or not) has no literal `..` in the
        # rel string, so only resolving the path catches the escape.
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp) / "root"
            outside = Path(tmp) / "elsewhere"
            root.mkdir()
            outside.mkdir()
            (root / "escape").symlink_to(outside)
            with self.assertRaises(ValueError):
                V.safe_join(root, "escape/custom.dict.yaml")


class VaultedList(unittest.TestCase):
    def test_never_includes_machine_identity(self):
        # installation.yaml carries installation_id, which Rime's own sync
        # keys on; sharing it would make two machines claim one identity.
        self.assertNotIn("installation.yaml", V.VAULTED_FILES)
        self.assertNotIn("user.yaml", V.VAULTED_FILES)

    def test_never_includes_derived_artifacts(self):
        for rel in V.VAULTED_FILES:
            self.assertNotIn("predict.db", rel)
            self.assertNotIn("sogou.dict.yaml", rel)
            self.assertFalse(rel.startswith("build/"), rel)

    def test_includes_the_irreplaceable_export(self):
        self.assertIn("private/custom.dict.yaml", V.VAULTED_FILES)


class RoundTrip(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.rime = root / "Rime"
        self.vault = root / "vault"
        for rel in V.VAULTED_FILES:
            target = self.rime / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_text(f"content of {rel}\n", encoding="utf-8")

    def tearDown(self):
        self.tmp.cleanup()

    def backup(self):
        actions = V.plan_backup(self.rime, self.vault, machine="TestMac")
        V.apply_backup(self.rime, self.vault, actions, machine="TestMac", now="2026-08-14T00:00:00Z")
        return actions

    def test_backup_then_restore_reproduces_every_file(self):
        self.backup()
        for rel in V.VAULTED_FILES:
            (self.rime / rel).unlink()
        actions = V.plan_restore(self.rime, self.vault)
        self.assertTrue(all(a.kind == "restore" for a in actions), actions)
        V.apply_restore(self.rime, self.vault, actions)
        for rel in V.VAULTED_FILES:
            self.assertEqual(f"content of {rel}\n",
                             (self.rime / rel).read_text(encoding="utf-8"))

    def test_manifest_records_machine_and_hash(self):
        self.backup()
        manifest = V.read_manifest(self.vault)
        record = manifest["private/custom.dict.yaml"]
        self.assertEqual("TestMac", record.machine)
        self.assertEqual(64, len(record.sha256))
        self.assertEqual(V.sha256_file(self.rime / "private/custom.dict.yaml"),
                         record.sha256)

    def test_unchanged_file_is_identical_not_restore(self):
        self.backup()
        actions = {a.rel: a.kind for a in V.plan_restore(self.rime, self.vault)}
        self.assertEqual("identical", actions["private/custom.dict.yaml"])

    def test_locally_modified_file_is_a_conflict(self):
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        actions = {a.rel: a.kind for a in V.plan_restore(self.rime, self.vault)}
        self.assertEqual("conflict", actions["private/custom.dict.yaml"])

    def test_conflict_is_not_applied(self):
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        actions = V.plan_restore(self.rime, self.vault)
        V.apply_restore(self.rime, self.vault, actions)
        self.assertEqual("mine\n",
                         (self.rime / "private/custom.dict.yaml").read_text(encoding="utf-8"))

    def test_force_turns_a_conflict_into_a_restore(self):
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        actions = V.plan_restore(self.rime, self.vault, force=True)
        V.apply_restore(self.rime, self.vault, actions)
        self.assertEqual("content of private/custom.dict.yaml\n",
                         (self.rime / "private/custom.dict.yaml").read_text(encoding="utf-8"))

    def test_file_absent_from_the_vault_is_reported_not_skipped_silently(self):
        self.backup()
        (self.vault / "files" / "private/custom.dict.yaml").unlink()
        actions = {a.rel: a.kind for a in V.plan_restore(self.rime, self.vault)}
        self.assertEqual("missing-in-vault", actions["private/custom.dict.yaml"])

    def test_backup_skips_files_absent_locally(self):
        (self.rime / "squirrel.custom.yaml").unlink()
        actions = {a.rel: a.kind
                   for a in V.plan_backup(self.rime, self.vault, machine="TestMac")}
        self.assertEqual("missing-locally", actions["squirrel.custom.yaml"])

    def test_second_backup_of_unchanged_files_is_a_no_op(self):
        self.backup()
        actions = V.plan_backup(self.rime, self.vault, machine="TestMac")
        self.assertTrue(all(a.kind == "identical" for a in actions), actions)

    def test_backing_up_over_another_machines_copy_is_a_conflict(self):
        # The incident this rule exists for: Mac-Mini held the June-2025
        # pristine export as its live custom.dict.yaml, `restore` correctly
        # refused to overwrite it (conflict), and then `backup` pushed that
        # file over the cleaned 8231-entry result another machine had put in
        # the vault eleven days earlier. restore protects local content;
        # nothing protected the vault.
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        actions = {a.rel: a.kind
                   for a in V.plan_backup(self.rime, self.vault, machine="OtherMac")}
        self.assertEqual("conflict", actions["private/custom.dict.yaml"])

    def test_backing_up_over_your_own_copy_is_not_a_conflict(self):
        # Updating a file you yourself last backed up is the normal case and
        # must stay frictionless -- otherwise every edit needs --force.
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("mine\n", encoding="utf-8")
        actions = {a.rel: a.kind
                   for a in V.plan_backup(self.rime, self.vault, machine="TestMac")}
        self.assertEqual("backup", actions["private/custom.dict.yaml"])

    def test_a_backup_conflict_is_not_written_to_the_vault(self):
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        actions = V.plan_backup(self.rime, self.vault, machine="OtherMac")
        V.apply_backup(self.rime, self.vault, actions,
                       machine="OtherMac", now="2026-08-20T00:00:00Z")
        self.assertEqual("content of private/custom.dict.yaml\n",
                         (self.vault / "files" / "private/custom.dict.yaml")
                         .read_text(encoding="utf-8"))

    def test_a_backup_conflict_leaves_the_manifest_record_alone(self):
        # A clobbered record is how the provenance that `status` reports gets
        # lost -- the file would still say it came from the machine that was
        # overwritten.
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        actions = V.plan_backup(self.rime, self.vault, machine="OtherMac")
        V.apply_backup(self.rime, self.vault, actions,
                       machine="OtherMac", now="2026-08-20T00:00:00Z")
        record = V.read_manifest(self.vault)["private/custom.dict.yaml"]
        self.assertEqual("TestMac", record.machine)

    def test_force_turns_a_backup_conflict_into_a_backup(self):
        self.backup()
        (self.rime / "private/custom.dict.yaml").write_text("newer\n", encoding="utf-8")
        actions = V.plan_backup(self.rime, self.vault, machine="OtherMac", force=True)
        V.apply_backup(self.rime, self.vault, actions,
                       machine="OtherMac", now="2026-08-20T00:00:00Z")
        self.assertEqual("newer\n",
                         (self.vault / "files" / "private/custom.dict.yaml")
                         .read_text(encoding="utf-8"))

    def test_a_stored_file_the_manifest_does_not_know_is_a_conflict(self):
        # No record means no provenance, so there is no way to tell whose
        # content is about to be replaced. Refuse rather than guess.
        self.backup()
        V.write_manifest(self.vault, {})
        (self.rime / "private/custom.dict.yaml").write_text("stale\n", encoding="utf-8")
        actions = {a.rel: a.kind
                   for a in V.plan_backup(self.rime, self.vault, machine="TestMac")}
        self.assertEqual("conflict", actions["private/custom.dict.yaml"])

    def test_a_file_not_yet_in_the_vault_is_a_backup_not_a_conflict(self):
        actions = {a.rel: a.kind
                   for a in V.plan_backup(self.rime, self.vault, machine="TestMac")}
        self.assertEqual("backup", actions["private/custom.dict.yaml"])

    def test_restore_creates_missing_parent_directories(self):
        self.backup()
        import shutil
        shutil.rmtree(self.rime / "private")
        actions = V.plan_restore(self.rime, self.vault)
        V.apply_restore(self.rime, self.vault, actions)
        self.assertTrue((self.rime / "private/custom.dict.yaml").is_file())


if __name__ == "__main__":
    unittest.main()
