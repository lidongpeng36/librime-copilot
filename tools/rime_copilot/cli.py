"""The `rime-copilot` command.

Orchestration only: every decision lives in a module that can be tested without
a network, an input method, or ~/Library/Rime.
"""
from __future__ import annotations

import argparse
import datetime
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path
from typing import Sequence

from . import dictdb, dictfile, freshness, install, paths, scel, vault

DEFAULT_SQUIRREL = Path("/Library/Input Methods/Squirrel.app/Contents/MacOS/Squirrel")
SOGOU_DICT_NAME = "sogou.dict.yaml"
PREDICT_DB_NAME = "private.predict.db"
CONFIG_NAME = "dict.json"
SHRINK_FLOOR = 0.9


def _now() -> str:
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")


def _private(rime_dir: Path) -> Path:
    return rime_dir / "private"


def _install_dest(rime_dir: Path) -> Path:
    return _private(rime_dir) / "bin"


def _stamp_path(rime_dir: Path, output: Path) -> Path:
    """The stamp file describing a build that wrote to `output`.

    One stamp per described output, not one per Rime directory -- `--output`
    lets `build` target a database other than the default, and a shared
    stamp would let a build of one output overwrite the bookkeeping for
    another (see `freshness.STAMP_NAME`'s neighbours in git history for the
    incident this fixes). The default output keeps the long-lived
    `STAMP_NAME` -- a real stamp already exists there on deployed machines,
    describing a current, valid build, and there is no reason to orphan it.
    Any other `--output` gets a stamp named after it, beside it.
    """
    default_output = (_private(rime_dir) / PREDICT_DB_NAME).resolve()
    if output.resolve() == default_output:
        return _private(rime_dir) / freshness.STAMP_NAME
    return output.parent / (output.name + ".stamp.json")


def _print_missing_requirements(interpreter: str) -> None:
    """Name every dependency `interpreter` lacks, and the exact command that
    would install them.

    Printed by both `install` and `status` for the same reason drift is: an
    interpreter is a thing that rots after you pin it -- a virtualenv gets
    rebuilt, a package gets pruned -- and the failure it causes surfaces
    somewhere far away, in the middle of a subcommand, as an ImportError.
    """
    missing = install.missing_requirements(interpreter)
    if not missing:
        return
    print(f"warning: {interpreter} is missing {len(missing)} of the pipeline's "
          f"dependencies:")
    for requirement in missing:
        print(f"  {requirement.module:<10} needed by {requirement.breaks}")
    packages = " ".join(r.package for r in missing)
    print(f"  install with: {interpreter} -m pip install {packages}")


def _print_installed_status(rime_dir: Path) -> None:
    dest = _install_dest(rime_dir)
    installed = install.read_install_manifest(dest)
    if installed is None:
        print("installed: not installed")
        return
    commit7 = installed.source_commit[:7] if installed.source_commit else "unknown"
    interpreter = install.installed_interpreter(dest)
    if not Path(installed.source_root).is_dir():
        print(f"installed: {dest} @ {commit7} (source repo not present; cannot compare)")
    else:
        lines = install.drift(dest)
        if not lines:
            print(f"installed: {dest} @ {commit7} (in sync)")
        else:
            names = [line.split(":", 1)[0] for line in lines]
            print(f"installed: {dest} @ {commit7} — {len(lines)} file(s) differ from the "
                 f"repo: {', '.join(names)}")
    if interpreter is None:
        return
    print(f"python:    {interpreter}")
    # Same family as file drift, and just as invisible: editing
    # `private/.python-version` looks like it should take effect, but the
    # shebang was frozen at install time and nothing re-reads it.
    declared = install.declared_interpreter(dest)
    if declared is not None and declared.interpreter not in (None, interpreter):
        print(f"           {declared.version_file} now names `{declared.version}` "
              f"({declared.interpreter}); re-run install to pin it")
    _print_missing_requirements(interpreter)


def _vault_dir_or_error(rime_dir: Path) -> "tuple[Path | None, str | None]":
    """`paths.vault_dir`, or an actionable message instead of a raw traceback.

    `sync_dir` is not something Squirrel writes -- verified against
    ~/repo/librime/src/rime/lever/deployment_tasks.cc: the write-back there
    (lines 136-163) sets installation_id/install_time/update_time/
    distribution_*/rime_version, and nothing else. A genuinely new Mac's
    installation.yaml has no `sync_dir` until someone adds it by hand, so
    this is not a rare misconfiguration -- it is the first-run state.
    """
    try:
        return paths.vault_dir(rime_dir), None
    except (FileNotFoundError, LookupError) as exc:
        installation_yaml = rime_dir / "installation.yaml"
        message = (
            f"cannot find the vault: {exc}\n"
            f"`sync_dir` is added to installation.yaml by hand -- Squirrel "
            f"never writes it. Add a line like:\n"
            f'  sync_dir: "/Users/you/Library/Mobile Documents/com~apple~CloudDocs/RimeSync"\n'
            f"to {installation_yaml}, pointing at wherever you keep (or want "
            f"to create) the iCloud-synced vault. Write that path down "
            f"somewhere durable outside this repo: installation.yaml is "
            f"deliberately never itself backed up, so this is the only "
            f"record of where the vault lives."
        )
        return None, message


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
    else:
        try:
            reason = freshness.rebuild_reason(
                freshness.compute_stamp(rime_dir, config, output),
                freshness.read_stamp(_stamp_path(rime_dir, output)))
            print(f"build:    {reason or 'up to date'}")
        except FileNotFoundError as exc:
            # status is what you run when something is already wrong; it reports.
            print(f"build:    cannot tell ({exc})")

    _print_installed_status(rime_dir)
    return 0


def cmd_backup(args) -> int:
    rime_dir = args.rime_dir
    store, error = _vault_dir_or_error(rime_dir)
    if store is None:
        print(error)
        return 1
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
    store, error = _vault_dir_or_error(rime_dir)
    if store is None:
        print(error)
        return 1
    actions = vault.plan_restore(rime_dir, store, force=args.force)
    for action in actions:
        print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
    conflicts = [a for a in actions if a.kind == "conflict"]
    missing = [a for a in actions if a.kind == "missing-in-vault"]
    if missing:
        # Not a failure: plan_backup silently skips a file the user does not
        # have (kind "missing-locally"), so the vault legitimately never
        # gets it -- that round trip must not read as blocked. It is also
        # the state before iCloud has pulled a shared vault down onto a new
        # Mac: an unmaterialized file has no name on disk yet, so
        # plan_restore sees the same "not a file" it would for a file that
        # was simply never backed up. Name both causes; there is no way to
        # tell them apart from here.
        print(f"{len(missing)} file(s) not in the vault -- either never backed up "
             f"from this machine, or iCloud has not materialized {store} yet "
             f"(wait a moment and re-run)")
    if args.dry_run:
        return 1 if conflicts else 0
    vault.apply_restore(rime_dir, store, actions)
    if conflicts:
        # Non-zero so a scripted restore cannot mistake a real conflict for
        # success. --force resolves it. missing-in-vault does not block --
        # see above.
        print(f"{len(conflicts)} file(s) not restored (conflict)", file=sys.stdout)
        return 1
    return 0


def cmd_fetch(args) -> int:
    rime_dir = args.rime_dir
    scel_dir = _private(rime_dir) / "scel"
    target = _private(rime_dir) / SOGOU_DICT_NAME
    if args.dry_run:
        print(f"would download {len(scel.DICT_URLS)} dictionaries into {scel_dir}")
        return 0

    try:
        scel.download_all(scel.DICT_URLS, scel_dir)
    except RuntimeError as exc:
        # An unreachable Sogou, or a drift in the scraped HTML selectors --
        # third-party HTML, so it will drift eventually. cmd_update decides
        # whether this is fatal; here it is just reported, not raised.
        print(f"fetch failed: {exc}")
        return 1

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
        if after < before * SHRINK_FLOOR:
            staged.unlink()
            print(f"refusing to overwrite: {after} lines is under "
                 f"{SHRINK_FLOOR:.0%} of {before}")
            return 1
    staged.replace(target)
    print(f"{written} entries -> {target}")
    return 0


def _missing_config_hint(rime_dir: Path, config: Path) -> str:
    """A trailing clause naming where the missing `config` can be had, or ""
    when there is nothing true to say.

    `dict.json` is one of the vaulted files, so on a machine that has just
    run `install` the overwhelmingly likely reason `build` cannot find it is
    that `restore` has not run yet -- it is in the vault, one command away.
    Saying only "no <path>" left that to be remembered rather than read, and
    `build` is reached through `update`, where it is the first thing a fresh
    machine stops on.

    Silent when the vault has no copy either, or cannot be found at all: a
    pointer to a `restore` that would restore nothing is worse than the bare
    error, and looking for the hint must never turn one clear failure into a
    different, noisier one.
    """
    try:
        store = paths.vault_dir(rime_dir)
        relative = config.resolve().relative_to(rime_dir.resolve())
        stored = vault.safe_join(store / vault.FILES_DIR, str(relative))
    except (FileNotFoundError, LookupError, OSError, ValueError):
        return ""
    if not stored.is_file():
        return ""
    return " -- it is in the vault; run `rime-copilot restore` first"


def cmd_build(args) -> int:
    rime_dir = args.rime_dir
    config = Path(args.config).expanduser() if args.config else _private(rime_dir) / CONFIG_NAME
    output = Path(args.output).expanduser() if args.output else _private(rime_dir) / PREDICT_DB_NAME
    stamp_path = _stamp_path(rime_dir, output)

    # `build` needs the config to do anything at all -- fail loud rather than
    # let a missing file surface as a raw traceback out of compute_stamp.
    # (status treats the same condition as informational and returns 0; build
    # cannot proceed at all, so it is a real failure.)
    if not config.is_file():
        print(f"cannot build: no {config}{_missing_config_hint(rime_dir, config)}")
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


def _git_commit(source_root: Path) -> "str | None":
    # Best-effort only: install must still work when git is unavailable or
    # source_root is not a git repo (e.g. a tarball checkout).
    try:
        result = subprocess.run(["git", "-C", str(source_root), "rev-parse", "HEAD"],
                                capture_output=True, text=True, timeout=10)
    except (OSError, subprocess.SubprocessError):
        return None
    if result.returncode != 0:
        return None
    return result.stdout.strip() or None


def _interpreter_from_spec(spec: str) -> "tuple[str | None, str | None]":
    """`--python`'s value as an absolute interpreter path, or why it is not one."""
    # A bare name is resolved on PATH, so `--python python3.11` works without
    # anyone having to look up where pyenv put it.
    found = str(Path(spec).expanduser()) if os.sep in spec else shutil.which(spec)
    if not found or not Path(found).is_file() or not os.access(found, os.X_OK):
        return None, f"--python: no executable interpreter at {spec}"
    # abspath, deliberately not resolve(): a virtualenv's `bin/python3` is a
    # symlink to the base interpreter, and pinning the resolved target would
    # pin the one interpreter that does *not* have the environment's
    # packages -- the exact failure --python exists to fix.
    return os.path.abspath(found), None


def _choose_interpreter(spec: "str | None", dest: Path
                        ) -> "tuple[str | None, str, str | None]":
    """The interpreter to pin, where that choice came from, and any error.

    In order: `--python`, then the `.python-version` the destination itself
    declares, then the interpreter running `install`.

    The last is the one that must not be first. Under pyenv, `python3` is a
    shim resolving `.python-version` from the *caller's* current directory,
    so `install` run from the checkout pins whatever environment some parent
    of the *checkout* names -- one chosen for an unrelated project, with no
    reason to have this pipeline's dependencies. The destination's own
    `.python-version` is the declaration that is actually about this
    installation, and it lives next to it, so it says the same thing however
    `install` was invoked.
    """
    if spec is not None:
        interpreter, error = _interpreter_from_spec(spec)
        return interpreter, "--python", error

    declared = install.declared_interpreter(dest)
    if declared is None:
        return (sys.executable,
                f"the interpreter running install; no {install.PYTHON_VERSION_FILE} "
                f"at or above {dest}", None)
    if declared.interpreter is None:
        # An unresolvable declaration is worth saying out loud and then
        # working around: refusing to install over it would strand the very
        # machine that is trying to get set up.
        print(f"warning: {declared.version_file} names `{declared.version}`, but "
              f"{declared.problem}")
        return (sys.executable,
                "the interpreter running install; pass --python to choose another", None)
    return declared.interpreter, f"{declared.version_file} (`{declared.version}`)", None


def cmd_install(args) -> int:
    rime_dir = args.rime_dir
    source_root = Path(__file__).resolve().parent.parent
    dest = Path(args.dest).expanduser() if args.dest else _install_dest(rime_dir)

    if not install.is_source_checkout(source_root):
        print(f"refusing to install: {source_root} does not look like a rime-copilot "
             f"checkout -- installing from an installed copy would launder drift")
        return 1

    # Resolved before anything is written, and reported as part of the plan:
    # which interpreter gets pinned is the single most consequential thing
    # `install` decides, and the one a `--dry-run` most needs to reveal.
    interpreter, provenance, error = _choose_interpreter(args.python, dest)
    if interpreter is None:
        print(error)
        return 1

    builder = paths.find_builder(args.builder)
    actions = install.plan_install(source_root, dest, builder)
    for action in actions:
        print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
    print(f"  {'python':<17} {interpreter}")
    print(f"  {'':<17} from {provenance}")
    _print_missing_requirements(interpreter)
    if args.dry_run:
        return 0

    commit = _git_commit(source_root)
    # Pinned explicitly rather than left to apply_install's own default, so
    # one value drives the entry point's shebang and the dependency check
    # alike -- and so `--python` has somewhere to reach (see install.py's
    # module docstring and README "Keeping the installed copy honest").
    install.apply_install(source_root, dest, builder, commit, _now(), interpreter=interpreter)
    print(f"installed to {dest} @ {commit[:7] if commit else 'unknown'}")
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
    fetch_code = cmd_fetch(args)
    if fetch_code != 0:
        # A fetch failure (unreachable Sogou, or a scraper-selector drift --
        # both third-party HTML this pipeline does not control) must not
        # kill `update` outright when a perfectly good sogou.dict.yaml from
        # a previous fetch is already sitting there: warn and fall through
        # to `build`, which will just rebuild from what is on disk. Only
        # fatal when there is no dictionary at all to fall back on.
        existing = _private(args.rime_dir) / SOGOU_DICT_NAME
        if existing.is_file():
            print(f"fetch failed; continuing with the existing {existing}")
        else:
            print(f"fetch failed and no existing {existing} to fall back on")
            return fetch_code

    for step in (cmd_build, cmd_deploy):
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
        if name == "build":
            command.add_argument("--config",
                                 help="dictionary-list JSON (default: <rime-dir>/private/"
                                      f"{CONFIG_NAME})")
            command.add_argument("--output",
                                 help="where to write the built db (default: <rime-dir>/"
                                      f"private/{PREDICT_DB_NAME})")
        command.set_defaults(func=func)

    deploy = sub.add_parser("deploy")
    deploy.add_argument("--squirrel")
    deploy.set_defaults(func=cmd_deploy)

    install_cmd = sub.add_parser("install")
    install_cmd.add_argument("--dest", help="where to install (default: <rime-dir>/private/bin)")
    install_cmd.add_argument("--builder", help=f"path to {paths.BUILDER_NAME}")
    install_cmd.add_argument(
        "--python",
        help="interpreter to pin in the installed entry point's shebang "
             "(default: whatever the destination's own .python-version "
             "declares, else the interpreter running install)")
    install_cmd.set_defaults(func=cmd_install)
    return parser


def main(argv: "Sequence[str] | None" = None) -> int:
    args = build_parser().parse_args(argv)
    for attribute, default in (("builder", None), ("max_per_key", -1),
                               ("force_build", False), ("force", False),
                               ("squirrel", None), ("config", None), ("output", None)):
        if not hasattr(args, attribute):
            setattr(args, attribute, default)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
