"""The `rime-copilot` command.

Orchestration only: every decision lives in a module that can be tested without
a network, an input method, or ~/Library/Rime.
"""
from __future__ import annotations

import argparse
import datetime
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence

from . import dictdb, dictfile, freshness, paths, scel, vault

DEFAULT_SQUIRREL = Path("/Library/Input Methods/Squirrel.app/Contents/MacOS/Squirrel")
SOGOU_DICT_NAME = "sogou.dict.yaml"
PREDICT_DB_NAME = "private.predict.db"
CONFIG_NAME = "dict.json"
SHRINK_FLOOR = 0.9


def _now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _private(rime_dir: Path) -> Path:
    return rime_dir / "private"


def cmd_status(args) -> int:
    rime_dir = args.rime_dir
    print(f"rime dir: {rime_dir}")
    try:
        store = paths.vault_dir(rime_dir)
        print(f"vault:    {store}")
        for action in vault.plan_restore(rime_dir, store):
            if action.kind != "identical":
                print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
    except (FileNotFoundError, LookupError) as exc:
        print(f"vault:    unavailable ({exc})")

    config = _private(rime_dir) / CONFIG_NAME
    output = _private(rime_dir) / PREDICT_DB_NAME
    if not config.is_file():
        print(f"build:    no {config}")
        return 0
    try:
        reason = freshness.rebuild_reason(
            freshness.compute_stamp(rime_dir, config, output),
            freshness.read_stamp(_private(rime_dir) / freshness.STAMP_NAME))
    except FileNotFoundError as exc:
        # status is what you run when something is already wrong; it reports.
        print(f"build:    cannot tell ({exc})")
        return 0
    print(f"build:    {reason or 'up to date'}")
    return 0


def cmd_backup(args) -> int:
    rime_dir = args.rime_dir
    store = paths.vault_dir(rime_dir)
    actions = vault.plan_backup(rime_dir, store)
    for action in actions:
        print(f"  {action.kind:<17} {action.rel}")
    if args.dry_run:
        return 0
    vault.apply_backup(rime_dir, store, actions,
                       machine=paths.machine_id(rime_dir), now=_now())
    print(f"backed up to {store}")
    return 0


def cmd_restore(args) -> int:
    rime_dir = args.rime_dir
    store = paths.vault_dir(rime_dir)
    actions = vault.plan_restore(rime_dir, store, force=args.force)
    for action in actions:
        print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
    blocked = [a for a in actions if a.kind in ("conflict", "missing-in-vault")]
    if args.dry_run:
        return 1 if blocked else 0
    vault.apply_restore(rime_dir, store, actions)
    if blocked:
        # Non-zero so a scripted restore cannot mistake a partial one for
        # success. --force resolves conflicts; a missing file needs a backup
        # from the machine that has it.
        print(f"{len(blocked)} file(s) not restored", file=sys.stdout)
        return 1
    return 0


def cmd_fetch(args) -> int:
    rime_dir = args.rime_dir
    scel_dir = _private(rime_dir) / "scel"
    target = _private(rime_dir) / SOGOU_DICT_NAME
    if args.dry_run:
        print(f"would download {len(scel.DICT_URLS)} dictionaries into {scel_dir}")
        return 0

    scel.download_all(scel.DICT_URLS, scel_dir)
    entries, names = scel.merge_scel_dir(scel_dir)
    staged = target.with_name(target.name + ".new")
    written = dictfile.write_dict(
        staged, name="sogou", version="1.0", entries=entries,
        use_preset_vocabulary=True,
        comment_lines=["# Rime dictionary", "# encoding: utf-8", "#",
                       "# Sogou Pinyin Dict - 搜狗细胞词库", "#",
                       "#   https://pinyin.sogou.com/dict/", "#", "# 包括:", "#",
                       *[f"# * {name}" for name in names]])

    # A partly failed download must not silently shrink the vocabulary.
    if target.is_file():
        before = sum(1 for _ in open(target, encoding="utf-8"))
        after = sum(1 for _ in open(staged, encoding="utf-8"))
        if after * 10 < before * 9:
            staged.unlink()
            print(f"refusing to overwrite: {after} lines is under 90% of {before}")
            return 1
    staged.replace(target)
    print(f"{written} entries -> {target}")
    return 0


def cmd_build(args) -> int:
    rime_dir = args.rime_dir
    config = _private(rime_dir) / CONFIG_NAME
    output = _private(rime_dir) / PREDICT_DB_NAME
    stamp_path = _private(rime_dir) / freshness.STAMP_NAME

    # `build` needs the config to do anything at all -- fail loud rather than
    # let a missing file surface as a raw traceback out of compute_stamp.
    # (status treats the same condition as informational and returns 0; build
    # cannot proceed at all, so it is a real failure.)
    if not config.is_file():
        print(f"cannot build: no {config}")
        return 1
    try:
        current = freshness.compute_stamp(rime_dir, config, output)
    except FileNotFoundError as exc:
        # Same exception compute_stamp raises when dict.json exists but names
        # a dictionary that does not -- the state a fresh machine passes
        # through: `restore` brings back dict.json before the dictionaries it
        # names exist. cmd_status guards this call; build must too, but
        # unlike status it genuinely cannot proceed, so it fails non-zero
        # rather than status's informational 0 -- cmd_update chains
        # fetch -> build -> deploy, and a zero here would let it reload the
        # input method after a build that never happened.
        print(f"cannot build: {exc}")
        return 1
    reason = freshness.rebuild_reason(current, freshness.read_stamp(stamp_path))
    if reason is None and not args.force_build:
        print("up to date")
        return 0
    print(f"rebuilding: {reason or 'forced'}")
    if args.dry_run:
        return 0

    builder = paths.find_builder(args.builder)
    loaded = [(source, dictfile.read_entries(source.path))
              for source in dictdb.load_sources(config)]
    entries = dictdb.merge(loaded)
    max_per_key = args.max_per_key if args.max_per_key > 0 else float("inf")

    with tempfile.NamedTemporaryFile("w", suffix=".tsv", delete=False) as handle:
        pairs_path = Path(handle.name)
    try:
        pairs = dictdb.write_pairs(entries, pairs_path, max_per_key)
        print(f"{len(entries)} words -> {pairs} prefix/suffix pairs")
        # Never in place: Squirrel mmaps this file, so truncating it can hand
        # the running input method half a database. Rename keeps the old inode
        # valid until the reload.
        staged = output.with_name(output.name + ".new")
        with open(pairs_path, "r", encoding="utf-8") as stdin:
            subprocess.run([str(builder), str(staged)], stdin=stdin, check=True)
        staged.replace(output)
    finally:
        pairs_path.unlink(missing_ok=True)

    freshness.write_stamp(stamp_path, freshness.compute_stamp(rime_dir, config, output))
    print(f"built {output}")
    return 0


def cmd_deploy(args) -> int:
    squirrel = args.squirrel or DEFAULT_SQUIRREL
    if args.dry_run:
        print(f"would run {squirrel} --reload")
        return 0
    if not Path(squirrel).is_file():
        print(f"Squirrel not found at {squirrel}; pass --squirrel")
        return 1
    subprocess.run([str(squirrel), "--reload"], check=True)
    print("reloaded")
    return 0


def cmd_update(args) -> int:
    for step in (cmd_fetch, cmd_build, cmd_deploy):
        code = step(args)
        if code != 0:
            return code
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(prog="rime-copilot",
                                     description="Rime copilot data pipeline")
    parser.add_argument("--rime-dir", type=paths.resolve_rime_dir,
                        default=paths.resolve_rime_dir(None))
    parser.add_argument("--dry-run", action="store_true")
    parser.add_argument("-v", "--verbose", action="store_true")
    sub = parser.add_subparsers(dest="command", required=True)

    sub.add_parser("status").set_defaults(func=cmd_status)
    sub.add_parser("backup").set_defaults(func=cmd_backup)

    restore = sub.add_parser("restore")
    restore.add_argument("--force", action="store_true",
                         help="overwrite local files the vault does not know about")
    restore.set_defaults(func=cmd_restore)

    sub.add_parser("fetch").set_defaults(func=cmd_fetch)

    for name, func in (("build", cmd_build), ("update", cmd_update)):
        command = sub.add_parser(name)
        command.add_argument("--builder", help=f"path to {paths.BUILDER_NAME}")
        command.add_argument("--max-per-key", type=int, default=-1,
                             help="cap continuations per key (<=0 means unlimited)")
        command.add_argument("--force-build", action="store_true")
        command.add_argument("--squirrel", help="path to the Squirrel binary")
        command.set_defaults(func=func)

    deploy = sub.add_parser("deploy")
    deploy.add_argument("--squirrel")
    deploy.set_defaults(func=cmd_deploy)
    return parser


def main(argv: "Sequence[str] | None" = None) -> int:
    args = build_parser().parse_args(argv)
    for attribute, default in (("builder", None), ("max_per_key", -1),
                               ("force_build", False), ("force", False),
                               ("squirrel", None)):
        if not hasattr(args, attribute):
            setattr(args, attribute, default)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
