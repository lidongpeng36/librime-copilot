"""Backing the irreplaceable files up to the iCloud sync directory.

The list below is not invented: it is what `git status --ignored` reports in a
rime-ice checkout, minus derived artifacts, minus the two files Rime's own sync
task owns, minus machine identity. `installation.yaml` carries
`installation_id`; copying one machine's to another makes two installations
claim the same identity.

Restore is deliberately additive. Four devices share this directory, so a local
file whose content the vault does not know about is a conflict, not something
to overwrite.
"""
from __future__ import annotations

import json
import shutil
from dataclasses import asdict, dataclass
from pathlib import Path
from typing import Sequence

from .paths import sha256_file  # re-exported: this module's callers use it too

VAULTED_FILES = (
    "private/custom.dict.yaml",
    "private/dict.json",
    "private.dict.yaml",
    "default.custom.yaml",
    "double_pinyin_flypy.custom.yaml",
    "squirrel.custom.yaml",
    "custom_phrase_double.txt",
)

MANIFEST_NAME = "manifest.json"
FILES_DIR = "files"


@dataclass(frozen=True)
class Record:
    sha256: str
    size: int
    backed_up_at: str
    machine: str


@dataclass(frozen=True)
class Action:
    rel: str
    kind: str
    detail: str = ""


def safe_join(root: Path, rel: str) -> Path:
    """Join `rel` under `root`, refusing anything that escapes it.

    Two independent checks, because neither subsumes the other:

    - A literal `..` component is refused outright. `a/../../root/b` resolves
      back under `root` by coincidence of naming, so the containment check
      below would accept it, but it traverses outside on the way.
    - Containment is then verified against the *resolved* path, because a
      `rel` with no `..` at all still escapes if a component on disk is a
      symlink. Four devices share the vault, so a symlink planted there by
      any of them is a realistic way to redirect a write.

    This is a check, not a lock: a symlink planted between this call and the
    write would still win. Closing that needs O_NOFOLLOW-style handling and is
    out of scope here.
    """
    relpath = Path(rel)
    if relpath.is_absolute():
        raise ValueError(f"absolute path not allowed in the vault list: {rel}")
    if ".." in relpath.parts:
        raise ValueError(f"parent traversal not allowed in the vault list: {rel}")
    anchor = root.resolve()
    candidate = (root / rel).resolve()
    if anchor != candidate and anchor not in candidate.parents:
        raise ValueError(f"path escapes {anchor}: {rel}")
    return root / rel


def read_manifest(vault: Path) -> dict[str, Record]:
    path = vault / MANIFEST_NAME
    if not path.is_file():
        return {}
    raw = json.loads(path.read_text(encoding="utf-8"))
    return {rel: Record(**fields) for rel, fields in raw.get("files", {}).items()}


def write_manifest(vault: Path, records: dict[str, Record]) -> None:
    vault.mkdir(parents=True, exist_ok=True)
    payload = {"version": 1, "files": {rel: asdict(r) for rel, r in sorted(records.items())}}
    path = vault / MANIFEST_NAME
    temporary = path.with_suffix(".json.new")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def plan_restore(rime_dir: Path, vault: Path, *, force: bool = False) -> list[Action]:
    actions = []
    for rel in VAULTED_FILES:
        stored = safe_join(vault / FILES_DIR, rel)
        local = safe_join(rime_dir, rel)
        if not stored.is_file():
            actions.append(Action(rel, "missing-in-vault"))
        elif not local.exists():
            actions.append(Action(rel, "restore"))
        elif sha256_file(local) == sha256_file(stored):
            actions.append(Action(rel, "identical"))
        elif force:
            actions.append(Action(rel, "restore", "forced over local changes"))
        else:
            actions.append(Action(rel, "conflict", "local content is not in the vault"))
    return actions


def apply_restore(rime_dir: Path, vault: Path, actions: Sequence[Action]) -> None:
    for action in actions:
        if action.kind != "restore":
            continue
        stored = safe_join(vault / FILES_DIR, action.rel)
        local = safe_join(rime_dir, action.rel)
        local.parent.mkdir(parents=True, exist_ok=True)
        temporary = local.with_name(local.name + ".new")
        shutil.copy2(stored, temporary)
        temporary.replace(local)


def plan_backup(rime_dir: Path, vault: Path) -> list[Action]:
    actions = []
    for rel in VAULTED_FILES:
        local = safe_join(rime_dir, rel)
        stored = safe_join(vault / FILES_DIR, rel)
        if not local.is_file():
            actions.append(Action(rel, "missing-locally"))
        elif stored.is_file() and sha256_file(local) == sha256_file(stored):
            actions.append(Action(rel, "identical"))
        else:
            actions.append(Action(rel, "backup"))
    return actions


def apply_backup(rime_dir: Path, vault: Path, actions: Sequence[Action],
                 machine: str, now: str) -> None:
    records = read_manifest(vault)
    for action in actions:
        if action.kind != "backup":
            continue
        local = safe_join(rime_dir, action.rel)
        stored = safe_join(vault / FILES_DIR, action.rel)
        stored.parent.mkdir(parents=True, exist_ok=True)
        temporary = stored.with_name(stored.name + ".new")
        shutil.copy2(local, temporary)
        temporary.replace(stored)
        records[action.rel] = Record(sha256=sha256_file(local),
                                     size=local.stat().st_size,
                                     backed_up_at=now, machine=machine)
    write_manifest(vault, records)
