"""The `rime-copilot` command.

Orchestration only: every decision lives in a module that can be tested without
a network, an input method, or ~/Library/Rime.
"""
from __future__ import annotations

import argparse
import datetime
import json
import os
import re
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
CLEAN_DIR_NAME = "clean_out"
CUSTOM_DICT_NAME = "custom.dict.yaml"
RAW_SUFFIX = ".raw"
CLEAN_STAMP_NAME = ".copilot_clean_stamp.json"

# The exact build the PoC and the integration measured (see
# docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md and
# 2026-08-17-llm-rerank-design.md). Changing the default here silently
# invalidates every measured number in those docs for anyone who fetches
# fresh -- keep it pinned to what was actually measured, not "the newest
# quant".
MODEL_URL = ("https://huggingface.co/unsloth/Qwen3-0.6B-GGUF/resolve/main/"
            "Qwen3-0.6B-Q4_K_M.gguf")
MODEL_NAME = "Qwen3-0.6B-q4_K_M.gguf"
# The real file is ~397 MB (unsloth/Qwen3-0.6B-GGUF, checked 2026-08-17). A
# floor well under that but far above an HTML error page or a truncated
# connection catches both failure modes without pinning an exact byte count
# a re-quantization upstream would break.
MIN_MODEL_SIZE = 300_000_000

# The octagram n-gram grammar that scores Poet's sentence paths. Squirrel
# already ships librime-octagram.dylib; only this file is missing, and without
# it sentence decoding runs on summed dictionary weights with no language
# model at all.
#
# Measured on the 3287 corpus runs that produce a sentence candidate:
# exact-match precision 59.4% -> 69.3%, 9.6% fewer commits over the corpus, at
# no measurable keystroke cost. Use the WORD-level model: the character-level
# one (zh-hans-t-essay-bgc, 10MB) measures 59.1%, indistinguishable from
# having no grammar, so it is not a cheaper substitute.
GRAMMAR_URL = ("https://raw.githubusercontent.com/lotem/rime-octagram-data/"
              "hans/zh-hans-t-essay-bgw.gram")
GRAMMAR_NAME = "zh-hans-t-essay-bgw.gram"
# The real file is ~41 MB (lotem/rime-octagram-data@hans, checked 2026-08-17).
# Same reasoning as MIN_MODEL_SIZE: far below the truth, far above an error
# page. The bgc model is 10 MB, so this floor also refuses it if the URL is
# edited to the wrong branch or filename.
MIN_GRAMMAR_SIZE = 20_000_000


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


_GRAMMAR_LANGUAGE = re.compile(r"^\s*(?:[\"']?grammar/language[\"']?|language)\s*:\s*[\"']?([^\"'#\s]+)")


def grammar_state(rime_dir: Path) -> tuple[str, str]:
    """(state, detail) for the octagram model the schemas ask for.

    Four states, and the third is the reason this exists at all:

      unconfigured  no schema names a grammar -- sentence decoding runs on
                    summed dictionary weights with no language model
      ok            configured, and the file it names is present
      missing       configured, but the file is NOT there. librime does not
                    fail on this; Octagram simply has no db and Query returns
                    a constant (octagram.cc:110), so the schema silently
                    decodes as though unconfigured. Nothing else reports it.
      unreadable    the schema files could not be read

    Scans `*.custom.yaml` and `build/*.schema.yaml` because the patch lives in
    the former and the deployed result in the latter, and the two disagreeing
    is itself worth seeing -- a patch that was never redeployed reads as
    configured while the running IME has no grammar.
    """
    names: dict[str, list[str]] = {}
    try:
        sources = sorted(rime_dir.glob("*.custom.yaml")) + sorted(
            (rime_dir / "build").glob("*.schema.yaml"))
        for path in sources:
            in_grammar_block = False
            for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
                stripped = line.strip()
                if stripped.startswith("grammar:"):
                    in_grammar_block = True
                    continue
                match = _GRAMMAR_LANGUAGE.match(line)
                # A bare `language:` counts only directly under `grammar:`;
                # other blocks use the same key for unrelated things.
                if match and ("grammar/language" in stripped or in_grammar_block):
                    names.setdefault(match.group(1), []).append(path.name)
                    in_grammar_block = False
                elif stripped and not line.startswith((" ", "\t")):
                    in_grammar_block = False
    except OSError as exc:
        return "unreadable", str(exc)

    if not names:
        return "unconfigured", "no schema names a grammar (no language model in sentence decoding)"

    parts = []
    worst = "ok"
    for name, where in sorted(names.items()):
        # octagram appends the suffix itself (ResourceType, octagram.cc:21).
        path = rime_dir / f"{name}.gram"
        if path.is_file():
            parts.append(f"{name} ({path.stat().st_size} bytes)")
        else:
            worst = "missing"
            parts.append(f"{name} MISSING at {path}, named by {', '.join(sorted(set(where)))}")
    return worst, "; ".join(parts)


def _print_grammar_status(rime_dir: Path) -> None:
    state, detail = grammar_state(rime_dir)
    print(f"grammar:  {state} -- {detail}")
    if state == "unconfigured":
        print("          `rime-copilot fetch-grammar` installs one and prints the patch")
    elif state == "missing":
        print("          `rime-copilot fetch-grammar` downloads it")


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

    stamp_path = _private(rime_dir) / CLEAN_STAMP_NAME
    if stamp_path.is_file():
        try:
            stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
            counts = stamp.get("counts", {})
            print(f"lexicon:  cleaned {stamp.get('cleaned_at')}, "
                  f"{counts.get('surviving')} entries")
        except (OSError, ValueError) as exc:
            print(f"lexicon:  stamp unreadable ({exc})")
    else:
        print("lexicon:  not cleaned")

    _print_grammar_status(rime_dir)
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


def _fetch_model_write(url: str, dest: Path) -> int:
    """Streams `url` to `dest`, returns bytes written.

    The only function in this module that touches the network -- isolated so
    tests can replace it wholesale (mirrors how the Fetch tests already patch
    around `scel.download_all` rather than mocking `requests` internals).
    """
    import requests  # lazy: this module must still import on a stock interpreter

    with requests.get(url, stream=True, timeout=30) as resp:
        resp.raise_for_status()
        written = 0
        with open(dest, "wb") as fh:
            for chunk in resp.iter_content(chunk_size=1 << 20):
                fh.write(chunk)
                written += len(chunk)
        return written


def _print_fetch_model_config(target: Path) -> None:
    # `model` is relative to the Rime user dir, not the process cwd -- see
    # copilot_engine.cc's ResourceResolver comment. `private/<name>` is
    # therefore what belongs in the schema regardless of where --rime-dir
    # points on this machine.
    print("Paste into a schema patch (ahead of `copilot/rerank/llm/enable: true`):")
    print("  copilot:")
    print("    rerank:")
    print("      llm:")
    print("        enable: true")
    print(f"        model: private/{target.name}")


def _print_fetch_grammar_config(target: Path) -> None:
    # Resolved by librime's ResourceResolver against the Rime user dir, the
    # same rule the gguf follows -- verified, not assumed: the harness measured
    # `private/<stem>` and a bare `<stem>` to the same 2277/3287.
    #
    # `language` takes the name WITHOUT the .gram suffix: octagram declares
    # ResourceType{"gram_db", "", ".gram"} (octagram.cc:21) and appends it.
    print("Paste into a schema patch:")
    print(f"  grammar/language: private/{target.name[: -len('.gram')]}")


def _fetch_artifact(args, *, target: Path, url: str, minimum: int,
                    command: str, show_config) -> int:
    """Download `url` to `target`, refusing anything implausibly small.

    Shared by fetch-model and fetch-grammar: both install one large binary
    under private/ that the schema then references by a relative path, and
    both have the same two failure modes worth guarding -- a half-written file
    a reader might mmap, and an HTML error page saved under the name of a
    model.
    """
    if target.is_file() and not args.force:
        print(f"already present, skipping: {target} ({target.stat().st_size} bytes)")
        show_config(target)
        return 0

    if args.dry_run:
        print(f"would download {url} -> {target}")
        return 0

    target.parent.mkdir(parents=True, exist_ok=True)
    # Never in place: a reader (llama.cpp mmaps the gguf; octagram mmaps the
    # .gram) must never see a half-written file -- same reasoning as `build`'s
    # staged rename.
    partial = target.with_name(target.name + ".part")
    try:
        written = _fetch_model_write(url, partial)
    except Exception as exc:  # noqa: BLE001 -- report, don't traceback
        partial.unlink(missing_ok=True)
        print(f"{command} failed: {exc}")
        return 1

    if written < minimum:
        partial.unlink(missing_ok=True)
        print(f"refusing to install: only {written} bytes, expected at least "
             f"{minimum} -- probably an error page, not a model")
        return 1

    partial.replace(target)
    print(f"{written} bytes -> {target}")
    show_config(target)
    return 0


def cmd_fetch_model(args) -> int:
    return _fetch_artifact(
        args,
        target=_private(args.rime_dir) / (args.name or MODEL_NAME),
        url=args.url or MODEL_URL,
        minimum=MIN_MODEL_SIZE,
        command="fetch-model",
        show_config=_print_fetch_model_config,
    )


def cmd_fetch_grammar(args) -> int:
    return _fetch_artifact(
        args,
        target=_private(args.rime_dir) / (args.name or GRAMMAR_NAME),
        url=args.url or GRAMMAR_URL,
        minimum=MIN_GRAMMAR_SIZE,
        command="fetch-grammar",
        show_config=_print_fetch_grammar_config,
    )


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


def cmd_clean(args) -> int:
    from . import clean as cleanup

    rime_dir = args.rime_dir
    source = _private(rime_dir) / CUSTOM_DICT_NAME
    if not source.is_file():
        print(f"no {source}")
        return 1

    entries = dictfile.read_entries(source)
    # M5: read_entries silently drops a row with 4+ tab columns or a
    # non-integer weight (only the whitespace case warns). That was fine
    # feeding a rebuildable database; it is not fine feeding a destructive
    # rewrite of a source nothing can regenerate.
    vocab_lines = dictfile.count_vocabulary_lines(source)
    if vocab_lines != len(entries):
        print(f"refusing: {source} has {vocab_lines} vocabulary line(s) but only "
             f"{len(entries)} parsed as entries ({vocab_lines - len(entries)} would be "
             f"silently dropped by a rewrite) -- inspect the file with dictfile before "
             f"running clean again")
        return 1

    chart = rime_dir / cleanup.CHART_NAME
    lexicon = cleanup.load_lexicon(chart if chart.is_file() else None)
    thresholds = cleanup.Thresholds(high=args.threshold_high, low=args.threshold_low)
    part = cleanup.partition(entries, lexicon, thresholds)

    print(f"lexicon: {len(entries)} entries")
    print(f"  keep   {len(part.keep)}")
    print(f"  review {len(part.review)}")
    print(f"  drop   {len(part.drop)}")

    out = _private(rime_dir) / CLEAN_DIR_NAME
    review_path = out / "review.tsv"

    if not args.apply:
        # Below the --force check on purpose: a dry run writes nothing, so
        # refusing it to protect annotations would be a refusal with nothing
        # to protect -- and `clean --dry-run` is the documented first step of
        # the whole sequence, which would then fail on every run after the
        # first.
        if args.dry_run:
            return 0
        if review_path.is_file() and not args.force:
            # 1,533 hand-annotated rows is real work, and the natural
            # reason to re-run `clean` is exactly retuning the thresholds
            # (see the I3 guard below) -- an unguarded overwrite here would
            # lose every annotation silently.
            print(f"refusing to overwrite {review_path}: it may hold hand-annotated "
                 f"keep/drop decisions that would be lost -- pass --force to "
                 f"regenerate it anyway")
            return 1
        out.mkdir(parents=True, exist_ok=True)
        review_path.write_text(cleanup.render_review(part, thresholds), encoding="utf-8")
        (out / "drop.tsv").write_text(cleanup.render_drop(part), encoding="utf-8")
        print(f"wrote {review_path}")
        print(f"wrote {out / 'drop.tsv'}")
        print("annotate review.tsv, then run: rime-copilot clean --apply")
        return 0

    if not review_path.is_file():
        print(f"no {review_path} — run `rime-copilot clean` first")
        return 1
    review_text = review_path.read_text(encoding="utf-8")

    # I3: apply with the thresholds the review file was generated with, not
    # whatever this invocation happens to pass -- generating with
    # --threshold-high 2000 then applying with the default silently applies a
    # different partition than the human reviewed.
    recorded = cleanup.parse_review_thresholds(review_text)
    if recorded is not None and recorded != thresholds:
        print(f"refusing: {review_path} was generated with thresholds "
             f"(high={recorded.high}, low={recorded.low}) but this run would apply "
             f"(high={thresholds.high}, low={thresholds.low}) -- re-run with "
             f"--threshold-high {recorded.high} --threshold-low {recorded.low} to match "
             f"what was reviewed, or regenerate review.tsv (with --force) under "
             f"--threshold-high {thresholds.high} --threshold-low {thresholds.low} and "
             f"re-annotate it")
        return 1

    # parse_review refuses a malformed row, and a word decided two conflicting
    # ways, rather than guessing -- its message already names the line numbers.
    # Surface it the way every other refusal in this function surfaces, instead
    # of as a traceback: this one is reached by hand-editing a 1,500-row TSV,
    # which is exactly when a stack trace is least useful.
    try:
        decisions = cleanup.parse_review(review_text)
    except ValueError as exc:
        print(f"refusing: {review_path} — {exc}")
        return 1
    survivors = cleanup.apply_review(part, decisions)
    print(f"  surviving {len(survivors)}")
    if args.dry_run:
        return 0

    # C1: a clean stamp with no `.raw` (or a `.raw` that disagrees with the
    # stamp's recorded hash) means "already cleaned somewhere, pristine
    # original missing or wrong here". Proceeding would preserve the
    # ALREADY-CLEANED live file as if it were the Sogou export below, and a
    # later `backup` would then push that fake "pristine" copy over the real
    # one in the vault -- destroying the only copy of the export, everywhere.
    raw = source.with_suffix(source.suffix + RAW_SUFFIX)
    stamp_path = _private(rime_dir) / CLEAN_STAMP_NAME
    if stamp_path.is_file():
        try:
            stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
        except (OSError, ValueError) as exc:
            print(f"refusing: {stamp_path} exists but is unreadable ({exc}) -- a clean "
                 f"stamp is present, so this machine may not be seeing a first clean; "
                 f"fix or remove the stamp before retrying")
            return 1
        if not raw.is_file():
            print(f"refusing: {stamp_path} records that this lexicon has already been "
                 f"cleaned, but {raw} is not present here. Applying now would preserve "
                 f"the already-cleaned {source.name} as if it were the pristine Sogou "
                 f"export, and a later `rime-copilot backup` would push that fake "
                 f"original over the real one in the vault. Run `rime-copilot restore` "
                 f"to fetch {raw.name} from the vault, or wait a moment if iCloud has "
                 f"not materialized it yet, then retry")
            return 1
        expected = stamp.get("raw_sha256")
        actual = paths.sha256_file(raw)
        if expected is not None and actual != expected:
            print(f"refusing: {raw} does not match the pristine original recorded in "
                 f"{stamp_path} (expected sha256 {expected}, found {actual}). This looks "
                 f"like a different or already-cleaned file. Run `rime-copilot restore` "
                 f"to fetch the correct {raw.name} from the vault before applying again")
            return 1

    # The export is not regenerable. Preserve it before anything overwrites the
    # live file -- and never a second time, or the pristine copy becomes a copy
    # of an already-cleaned lexicon. Staged then renamed, like every other write
    # in this package that threatens something live or irreplaceable (see
    # cmd_fetch's `staged`/`.replace()` and vault.py's `apply_backup`): a plain
    # in-place copy left `raw` truncated-but-existing on an interrupted run,
    # and the `not raw.exists()` guard would then treat that fragment as the
    # preserved original and let the next `--apply` overwrite the live file
    # for good.
    if not raw.exists():
        temporary_raw = raw.with_name(raw.name + ".new")
        shutil.copy2(source, temporary_raw)
        temporary_raw.replace(raw)
        print(f"preserved the original at {raw}")

    # Same reasoning, lower stakes: dictfile.write_dict opens `source` in "w"
    # mode and streams into it, so an interruption here would truncate the
    # live dictionary (the pristine `raw` above would still be intact).
    staged = source.with_name(source.name + ".new")
    dictfile.write_dict(staged, name="custom",
                        version=datetime.date.today().isoformat(), entries=survivors)
    staged.replace(source)
    _write_clean_stamp(rime_dir, raw, source, part, len(survivors), thresholds)
    print(f"rewrote {source}")
    print("back the pristine export up with: rime-copilot backup")
    return 0


def _write_clean_stamp(rime_dir, raw, source, part, surviving, thresholds) -> None:
    stamp = {
        "cleaned_at": _now(),
        "raw_sha256": paths.sha256_file(raw),
        "result_sha256": paths.sha256_file(source),
        "counts": {"keep": len(part.keep), "review": len(part.review),
                   "drop": len(part.drop), "surviving": surviving},
        "thresholds": {"high": thresholds.high, "low": thresholds.low,
                       "compound": thresholds.compound},
    }
    path = _private(rime_dir) / CLEAN_STAMP_NAME
    path.write_text(json.dumps(stamp, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")


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

    fetch_model = sub.add_parser("fetch-model",
                                 help="download the gguf used by copilot/rerank/llm")
    fetch_model.add_argument("--url", help=f"override the download URL (default: {MODEL_URL})")
    fetch_model.add_argument("--name",
                             help=f"filename under private/ (default: {MODEL_NAME})")
    fetch_model.add_argument("--force", action="store_true",
                             help="re-download even if the file already exists")
    fetch_model.set_defaults(func=cmd_fetch_model)

    fetch_grammar = sub.add_parser(
        "fetch-grammar",
        help="download the octagram n-gram that scores sentence candidates")
    fetch_grammar.add_argument("--url", help=f"override the download URL (default: {GRAMMAR_URL})")
    fetch_grammar.add_argument("--name",
                               help=f"filename under private/ (default: {GRAMMAR_NAME})")
    fetch_grammar.add_argument("--force", action="store_true",
                               help="re-download even if the file already exists")
    fetch_grammar.set_defaults(func=cmd_fetch_grammar)

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

    clean_cmd = sub.add_parser("clean",
                               help="prune the Sogou-exported personal lexicon")
    clean_cmd.add_argument("--apply", action="store_true",
                           help="read review.tsv back and rewrite custom.dict.yaml")
    clean_cmd.add_argument("--threshold-high", type=int, default=100,
                           help="commits that overrule a structural verdict (default: 100)")
    clean_cmd.add_argument("--threshold-low", type=int, default=3,
                           help="commits below which a pin carries no evidence (default: 3)")
    clean_cmd.add_argument("--force", action="store_true",
                           help="regenerate review.tsv even though one already exists "
                                "(discards any hand-annotated decisions in it)")
    clean_cmd.set_defaults(func=cmd_clean)

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
                               ("squirrel", None), ("config", None), ("output", None),
                               ("url", None), ("name", None),
                               ("apply", False), ("threshold_high", 100),
                               ("threshold_low", 3)):
        if not hasattr(args, attribute):
            setattr(args, attribute, default)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
