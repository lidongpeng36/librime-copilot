"""Deciding whether the prediction database is stale.

This replaces two mtime comparisons that lived in two shell scripts and between
them checked neither `dict.json`, nor `cn_dicts/*`, nor the build logic. When
the YAML-header bug was fixed on 2026-08-14 no mtime changed, so neither script
would have rebuilt the database the fix invalidated.

Content hashes rather than mtimes, because mtime lies in exactly the situation
this tool exists for: files restored from iCloud have fresh mtimes and
identical content, and a mtime rule would rebuild 86 MB and reload the input
method for nothing. Hashing all inputs measures 0.76 s against a 30 s rebuild,
so there is no fast path to be worth its second code path.
"""
from __future__ import annotations

import json
from pathlib import Path

from .dictdb import load_sources
from .paths import sha256_file

# Bump when the build algorithm changes in a way that alters the output for
# unchanged inputs. No input hash can catch that.
RECIPE_VERSION = 1

STAMP_NAME = ".copilot_build_stamp.json"


def input_files(config_path: Path) -> list[Path]:
    """Every file whose content can change what the build produces."""
    return [config_path] + [source.path for source in load_sources(config_path)]


def _key(rime_dir: Path, path: Path) -> str:
    try:
        return path.resolve().relative_to(rime_dir.resolve()).as_posix()
    except ValueError:
        return path.resolve().as_posix()


def compute_stamp(rime_dir: Path, config_path: Path, output: Path) -> dict:
    inputs = {}
    for path in input_files(config_path):
        if not path.is_file():
            raise FileNotFoundError(f"dictionary listed in {config_path} is missing: {path}")
        inputs[_key(rime_dir, path)] = sha256_file(path)
    return {
        "recipe_version": RECIPE_VERSION,
        "output": _key(rime_dir, output),
        "output_sha256": sha256_file(output) if output.is_file() else None,
        "inputs": inputs,
    }


def read_stamp(path: Path) -> "dict | None":
    if not path.is_file():
        return None
    try:
        return json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        return None  # unreadable stamp is no worse than no stamp: rebuild


def write_stamp(path: Path, stamp: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(stamp, ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def rebuild_reason(current: dict, recorded: "dict | None") -> "str | None":
    """Why the database must be rebuilt, or None if it is up to date."""
    if recorded is None:
        return "no build stamp"
    if recorded.get("recipe_version") != current["recipe_version"]:
        return (f"recipe version {recorded.get('recipe_version')} -> "
                f"{current['recipe_version']}")
    if current["output_sha256"] is None:
        return "output is missing"
    if recorded.get("output_sha256") != current["output_sha256"]:
        return "output changed since it was built"

    # New/removed keys are checked in their own pass, ahead of changed-content
    # checks: adding or removing a dictionary always also changes dict.json's
    # own content (it lists the dictionaries), and a single alphabetically
    # ordered pass over the union of keys could report that incidental change
    # to dict.json instead of the actual new/removed input.
    was, now = recorded.get("inputs", {}), current["inputs"]
    for rel in sorted(set(now) - set(was)):
        return f"new input {rel}"
    for rel in sorted(set(was) - set(now)):
        return f"input removed: {rel}"
    for rel in sorted(set(was) & set(now)):
        if was[rel] != now[rel]:
            return f"input changed: {rel}"
    return None
