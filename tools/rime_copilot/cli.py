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
PERSONAL_DICT_NAME = "personal.dict.yaml"
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


_CTX_HOOKS = ("after-select-pane", "after-select-window")

CTX_MEMORY_ENABLE_KEY = "copilot/context_memory/enable"

# Tolerant of how the hook is actually written, because `set-hook -g` is not
# the only correct spelling and matching it literally reported `missing` on a
# machine that was configured right. `-ga` (append) is in fact the form the
# README recommends -- plain `-g` REPLACES any existing hook of that name and
# silently loses it. Flags are matched as a group so `-ga`, `-g -a`, doubled
# spaces and no flag at all all read as present.
_CTX_HOOK_RES = {
    hook: re.compile(r"set-hook\s+(?:-\w+\s+)*" + re.escape(hook) + r"\b")
    for hook in _CTX_HOOKS
}
# The reporter path named inside a hook body, so `status` can check the script
# is actually there. Quotes and the surrounding `run-shell -b "..."` are not
# parsed -- only the path token is needed.
_CTX_SCRIPT_RE = re.compile(r"""([^\s'"]*rime_ctx_report\.sh)""")


def _context_memory_enabled(built_schemas: "list[Path]") -> bool:
    """Whether any built schema turns per-context ascii_mode memory on.

    The BUILT schema, not the source one, for the reason `_processor_order_status`
    reads it: a `.custom.yaml` patch can set this without touching any
    `.schema.yaml`. Read through `_config_leaves` so the flat key, the nested
    block and the flow map Rime deploys all count as the same setting.
    """
    for path in built_schemas:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        for leaf, value in _config_leaves(text):
            if leaf == CTX_MEMORY_ENABLE_KEY and value.strip().lower() == "true":
                return True
    return False


def _context_memory_status(tmux_conf: Path, enabled: bool) -> "str | None":
    """One status line for the context-memory tmux hooks, or None for silence.

    `None` when the feature is off and nothing suggests the user meant to turn
    it on. This is an off-by-default feature, and a line printed on every run
    on every machine -- most of which have no tmux at all -- is exactly the
    always-on noise that teaches people to stop reading `status`.

    The one exception is a machine whose `.tmux.conf` HAS the hooks while
    `enable` is false: that user followed the README, got no behaviour, and
    the old line said `hook installed` at them. Half-done setup is worth a
    line; nothing at all is not.

    With the feature on, this reports a degraded mode, never an error --
    without the hooks it still works through the polling rung. It exists
    because the difference is invisible from outside.
    """
    text = tmux_conf.read_text(errors="replace") if tmux_conf.exists() else ""
    missing = [hook for hook in _CTX_HOOKS if not _CTX_HOOK_RES[hook].search(text)]

    if not enabled:
        if missing:
            return None
        return (f"context memory: off ({CTX_MEMORY_ENABLE_KEY} is false), but the tmux "
                f"hooks are installed -- set it true in your schema patch, or drop them")

    if missing:
        where = "no ~/.tmux.conf" if not text else "not found in ~/.tmux.conf"
        return ("context memory: polling; missing tmux hook(s): "
                + ", ".join(missing)
                + f" ({where}; a hook set from a source-file'd fragment is not visible here)"
                + " -- see README 'Context memory'")

    # The hooks are there, so the path they name had better be too. `status`
    # used to match the set-hook lines and stop, which reported `hook
    # installed` for a hook naming a script inside a checkout that had since
    # been moved -- at which point the hook fires and does nothing, forever.
    for match in _CTX_SCRIPT_RE.finditer(text):
        script = Path(match.group(1)).expanduser()
        if not script.is_file():
            return f"context memory: tmux hook names {script}, which does not exist"
        if not os.access(script, os.X_OK):
            return (f"context memory: tmux hook names {script}, which is not executable "
                    f"-- `chmod +x` it, or run `rime-copilot install` again")
    # A hook that names the script but does not hand it the pane. Before
    # 2026-08-31 that was the documented form, and the script asked tmux
    # instead -- `display-message -p` with no `-t`, which resolves against the
    # INVOKING client rather than the hook's target. Measured on tmux 3.7c: a
    # `select-window -t copilot:3` issued from another session's pane made the
    # hook report `librime:1 %3 claude` instead of `%5`. Interactive
    # prefix-key switches are correct by accident (there the invoking client
    # is the one being switched), so this is invisible in ordinary use, writes
    # a plausible-looking key, and appears in no log. A status line is the
    # only place it can surface.
    stale = [ln.strip() for ln in text.splitlines()
             if "rime_ctx_report" in ln and not ln.lstrip().startswith("#")
             and "#{pane_id}" not in ln]
    if stale:
        return ("context memory: tmux hook does not pass the pane in -- append "
                "'#{pane_id}' '#{pane_current_command}' '#{socket_path}' to the "
                "run-shell command; without them the reporter asks tmux, which "
                "answers for the invoking client and can name a pane in another "
                "session -- see README 'Context memory'")

    # NOT "no per-keystroke query", which this line claimed until 2026-08-31.
    # The hooks stop CONTEXT MEMORY from polling; they do not stop AutoSpacer,
    # which calls GetSurroundingContext() on every non-composing keystroke and
    # spawns tmux there regardless. Measured live on the day the hooks were
    # first installed on a machine: the pushed rung took over (`via bridge` in
    # the ctxmem log) and the spawn count did not move. A status line that
    # reports a saving nobody made is how a reader stops trusting the rest.
    return "context memory: hook installed (identity is pushed, not polled)"


def _ordered_list_block(text: str, opener: str, item: "re.Pattern" = None) -> "list[str]":
    """Items of the list under `opener`, in file order.

    The opener line opens the block; any non-item, non-blank line closes it,
    so the next key's contents cannot leak in. `_processor_order_status` needs
    relative position, and `_scan_block` wraps this for the callers that only
    need membership -- one scanner, two shapes.
    """
    if item is None:
        item = _LIST_ITEM
    found: "list[str]" = []
    inside = False
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("#"):
            continue
        if stripped.startswith(opener):
            inside = True
            continue
        if not inside:
            continue
        match = item.match(line)
        if match:
            found.append(match.group(1))
        elif stripped:
            inside = False
    return found


def _processor_order_status(built_schemas: "list[Path]") -> str:
    """One status line for whether `copilot` precedes `ascii_composer`.

    `AsciiComposer` returns `kRejected` for letter keys while `ascii_mode` is
    true, and the engine breaks the processor loop on `kRejected`, so a
    `copilot` ordered after it never runs in English mode at all -- context
    memory then dies in the English-to-Chinese direction only, which reads as
    flakiness rather than misconfiguration. The constructor already logs a
    `LOG(WARNING)` for this; this line is a second witness, in the place
    people actually look. A schema that does not list `copilot` at all does
    not use this plugin -- that is not a misconfiguration, so it is silent.
    Reads the BUILT schema, not the source `.schema.yaml`: a `.custom.yaml`
    patch can reorder `engine/processors` without touching the source.
    """
    flagged: "list[str]" = []
    any_copilot = False
    for path in built_schemas:
        try:
            text = path.read_text(encoding="utf-8", errors="replace")
        except OSError:
            continue
        order = _ordered_list_block(text, "processors:")
        if "copilot" not in order:
            continue
        any_copilot = True
        if ("ascii_composer" in order
                and order.index("ascii_composer") < order.index("copilot")):
            flagged.append(path.name)
    if flagged:
        return (
            "context memory: processor order WRONG in "
            + ", ".join(flagged)
            + " -- copilot must precede ascii_composer in engine/processors, "
              "or context memory dies English-to-Chinese only"
        )
    if not any_copilot:
        return "context memory: processor order n/a (no schema lists copilot)"
    return "context memory: processor order ok (copilot precedes ascii_composer)"


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


# One `key: value` line. The key may be quoted and may itself be a slash
# path (`"copilot/rerank/llm/model"`); the value is absent when the line only
# opens a block. Allowing a leading `-` lets a list item's inline key through,
# which costs nothing and keeps the indent bookkeeping below honest.
_KEY_LINE = re.compile(
    r"""^(?P<indent>[ \t]*)(?:-[ \t]+)?(?P<q>["']?)(?P<key>[^#'"]+?)(?P=q)[ \t]*"""
    r""":(?:[ \t]+(?P<value>.*?))?[ \t]*$""")


def _unquote(text: str) -> str:
    """A scalar as written, minus its quotes and any trailing comment."""
    text = text.strip()
    # Quoted values are taken whole first, so a `#` inside quotes survives.
    if len(text) >= 2 and text[0] == text[-1] and text[0] in "\"'":
        return text[1:-1]
    return text.split("#", 1)[0].strip().strip("\"'")


def _split_flow(body: str) -> list[str]:
    """Top-level comma-separated items of a flow map body, respecting quotes
    and nested `{}` / `[]`."""
    items: list[str] = []
    current: list[str] = []
    depth = 0
    quote = ""
    for char in body:
        if quote:
            current.append(char)
            if char == quote:
                quote = ""
            continue
        if char in "\"'":
            quote = char
        elif char in "{[":
            depth += 1
        elif char in "}]":
            depth -= 1
        elif char == "," and depth == 0:
            items.append("".join(current))
            current = []
            continue
        current.append(char)
    if "".join(current).strip():
        items.append("".join(current))
    return items


def _flow_leaves(base: str, body: str):
    for item in _split_flow(body):
        key, sep, value = item.partition(":")
        if not sep:
            continue
        path = base + "/" + _unquote(key)
        value = value.strip()
        if value.startswith("{"):
            yield from _flow_leaves(path, value[1:value.rfind("}")])
        else:
            yield path, _unquote(value)


def _config_leaves(text: str):
    """(path, value) for every scalar in a Rime config file.

    The same setting reaches `status` in three shapes and they must all read
    as one path. A patch may write a flat `"copilot/rerank/llm/model"` key or
    the same subtree nested -- both are correct, because a patch key whose
    value is a map replaces that node wholesale either way -- and the schema
    Rime deploys puts small maps in FLOW style, which lands the key on its
    parent's line where nothing line-anchored can see it. Matching a bare
    `model:` instead is not the fix: `copilot/llm/model` is the next-word
    prediction model, a different feature.

    Scanned rather than parsed, for the reason the rest of this module is: no
    YAML dependency for a handful of keys. A leading `patch/` is dropped so a
    patch and a deployed schema yield the same paths.
    """
    stack: list[tuple[int, str]] = []
    for line in text.splitlines():
        if not line.strip() or line.strip().startswith("#"):
            continue
        match = _KEY_LINE.match(line)
        if not match:
            continue
        indent = len(match.group("indent").expandtabs(8))
        while stack and stack[-1][0] >= indent:
            stack.pop()
        key = _unquote(match.group("key"))
        path = "/".join([k for _, k in stack] + [key])
        if path.startswith("patch/"):
            path = path[len("patch/"):]
        value = match.group("value")
        if value is None or not value.strip():
            stack.append((indent, key))
        elif value.strip().startswith("{"):
            body = value.strip()
            yield from _flow_leaves(path, body[1:body.rfind("}")])
        else:
            yield path, _unquote(value)


RERANK_MODEL_KEY = "copilot/rerank/llm/model"

_GRAMMAR_LANGUAGE = re.compile(r"^\s*(?:[\"']?grammar/language[\"']?|language)\s*:\s*[\"']?([^\"'#\s]+)")


_SCHEMA_LIST_ITEM = re.compile(r"^\s*-\s*schema\s*:\s*[\"']?([^\"'#\s]+)")
_LIST_ITEM = re.compile(r"^\s*-\s*[\"']?([^\"'#\s]+)")


def _scan_block(path: Path, opener: str, item: re.Pattern) -> set[str]:
    """Items of the list under `opener`, in patch form or deployed form.

    Scanned, not parsed, for the reason the rest of this module is: no YAML
    dependency for a handful of keys. Any non-item line closes the block, so
    the next key's contents cannot leak in.

    Membership only. `_ordered_list_block` is the same scan and is the one
    truth for it; two near-copies of a hand-rolled scanner would drift the
    first time either grows a case.
    """
    return set(_ordered_list_block(path.read_text(encoding="utf-8", errors="replace"),
                                   opener, item))


def enabled_schemas(rime_dir: Path) -> set[str] | None:
    """Schema ids the running IME loads, or None when that cannot be told.

    `build/` only ever grows: a schema built once stays there after it leaves
    schema_list, so the stock luna_pinyin/bopomofo/terra outputs sit next to
    the one schema actually in use. The grammar and model checks scanned all
    of them and reported `zh-hant-t-essay-bgw MISSING` on every run of a
    machine that was correctly configured -- noise in the one command whose
    whole value is that every line deserves reading.

    Deployed list first (`build/default.yaml`), the patch second: a machine
    that has never deployed still has an answer, and it is the one that will
    take effect.

    Returns None -- NOT an empty set -- when neither can be read. The callers
    must then scan everything, as before. Narrowing on a guess would turn a
    genuinely missing file into an `ok`, which is the failure this whole
    command exists to prevent; a stale extra line is the safe direction.
    """
    ids: set[str] = set()
    try:
        for path in (rime_dir / "build" / "default.yaml",
                     rime_dir / "default.custom.yaml"):
            if path.is_file():
                ids = _scan_block(path, "schema_list:", _SCHEMA_LIST_ITEM)
                if ids:
                    break
    except OSError:
        return None
    if not ids:
        return None

    # `schema/dependencies` is loaded by the IME without ever appearing in
    # schema_list -- double_pinyin_flypy pulls in melt_eng and radical_pinyin
    # that way. Filtering on schema_list alone would trade a false MISSING for
    # a false ok, the worse of the two. Transitive, and each id is queued at
    # most once, so a dependency cycle terminates.
    pending = list(ids)
    while pending:
        schema = rime_dir / "build" / f"{pending.pop()}.schema.yaml"
        if not schema.is_file():
            continue
        try:
            deps = _scan_block(schema, "dependencies:", _LIST_ITEM)
        except OSError:
            continue
        for dep in deps - ids:
            ids.add(dep)
            pending.append(dep)
    return ids


def _source_schema_id(rime_dir: Path, path: Path) -> str | None:
    """The schema a config file belongs to, or None if it is not a schema's.

    Only a positive identification is allowed to filter: `default.custom.yaml`
    and `squirrel.custom.yaml` patch global config rather than a schema, and
    dropping them for not being in schema_list would be filtering on a name
    collision. A `<id>.custom.yaml` counts as a schema patch only when a
    schema of that name is actually present.
    """
    name = path.name
    if name.endswith(".schema.yaml"):
        return name[: -len(".schema.yaml")]
    if name.endswith(".custom.yaml"):
        stem = name[: -len(".custom.yaml")]
        if ((rime_dir / "build" / f"{stem}.schema.yaml").is_file()
                or (rime_dir / f"{stem}.schema.yaml").is_file()):
            return stem
    return None


def _schema_sources(rime_dir: Path) -> list[Path]:
    """Config files worth scanning: the patches and the deployed results.

    Both forms, because a patch that was never redeployed reads as configured
    while the running IME has no such setting -- the two disagreeing is itself
    worth seeing. Filtered to the schemas the IME loads (`enabled_schemas`).
    """
    sources = sorted(rime_dir.glob("*.custom.yaml")) + sorted(
        (rime_dir / "build").glob("*.schema.yaml"))
    enabled = enabled_schemas(rime_dir)
    if enabled is None:
        return sources
    kept = []
    for path in sources:
        schema_id = _source_schema_id(rime_dir, path)
        if schema_id is None or schema_id in enabled:
            kept.append(path)
    return kept


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

    Reads `_schema_sources`: the patch and the deployed result, for the
    schemas the IME actually loads. A schema nothing loads any more still has
    its build output sitting in `build/`, and reporting on that is noise.
    """
    names: dict[str, list[str]] = {}
    try:
        sources = _schema_sources(rime_dir)
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


def model_state(rime_dir: Path) -> tuple[str, str]:
    """(state, detail) for the neural re-ranking model the schemas ask for.

    Same three states as `grammar_state`, and the same reason for existing:
    librime does not fail when a schema names a model that is not there.
    LlmScorer logs one load failure and never retries (`Loaded()` stays false
    forever), the filter's fallback chain reads that as `llm_skip=kNoModel`,
    and re-ranking quietly falls back to the db -- which is exactly what a
    working install looks like from outside.

    Scanned rather than parsed, over the same `_schema_sources` as the grammar
    check: this package has no YAML dependency and adding one for one key
    would be the tail wagging the dog.
    """
    names: dict[str, list[str]] = {}
    try:
        for path in _schema_sources(rime_dir):
            text = path.read_text(encoding="utf-8", errors="replace")
            for leaf, value in _config_leaves(text):
                if leaf == RERANK_MODEL_KEY and value:
                    names.setdefault(value, []).append(path.name)
    except OSError as exc:
        return "unreadable", str(exc)

    if not names:
        return "unconfigured", "no schema names a neural re-ranking model"

    parts = []
    worst = "ok"
    for name, where in sorted(names.items()):
        path = rime_dir / name
        if path.is_file():
            parts.append(f"{name} ({path.stat().st_size // 1048576} MB)")
        else:
            worst = "missing"
            parts.append(f"{name} MISSING at {path}, named by {', '.join(sorted(set(where)))}")
    return worst, "; ".join(parts)


def _print_lexicon_status(rime_dir: Path) -> None:
    """Report the clean stamp *checked against the file it describes*.

    The stamp is vaulted, so it reaches every machine -- including one that
    never applied the cleaning it describes. Reading the stamp alone made
    Mac-Mini report `cleaned 2026-08-17T09:13:29Z, 8231 entries` while its
    live lexicon was the 1MB pristine export, which is precisely the kind of
    silent divergence `status` exists to catch.
    """
    stamp_path = _private(rime_dir) / CLEAN_STAMP_NAME
    if not stamp_path.is_file():
        print("lexicon:  not cleaned")
        return
    try:
        stamp = json.loads(stamp_path.read_text(encoding="utf-8"))
    except (OSError, ValueError) as exc:
        print(f"lexicon:  stamp unreadable ({exc})")
        return

    counts = stamp.get("counts", {})
    summary = (f"cleaned {stamp.get('cleaned_at')}, "
               f"{counts.get('surviving')} entries")
    result = stamp.get("result_sha256")
    lexicon = _private(rime_dir) / CUSTOM_DICT_NAME

    # A stamp written before these hashes existed cannot be checked. Report
    # what it says rather than inventing a mismatch out of a missing field.
    if not result:
        print(f"lexicon:  {summary}")
        return
    if not lexicon.is_file():
        print(f"lexicon:  stamp says {summary}, but {CUSTOM_DICT_NAME} is missing")
        return

    actual = paths.sha256_file(lexicon)
    if actual == result:
        print(f"lexicon:  {summary}")
    elif actual == stamp.get("raw_sha256"):
        print(f"lexicon:  stamp says {summary}, but this machine holds the "
              f"pristine export, not that result")
        print("          `rime-copilot restore` brings the cleaned lexicon down "
              "-- do NOT `backup` first, that is what overwrote it")
    else:
        print(f"lexicon:  stamp says {summary}, but {CUSTOM_DICT_NAME} matches "
              f"neither that result nor the raw it was cleaned from")
        print("          re-run `clean`, or `restore` the shared copy")


def _print_model_status(rime_dir: Path) -> None:
    state, detail = model_state(rime_dir)
    print(f"model:    {state} -- {detail}")
    if state == "missing":
        print("          `rime-copilot install-model --from PATH` copies one in")


def _print_grammar_status(rime_dir: Path) -> None:
    state, detail = grammar_state(rime_dir)
    print(f"grammar:  {state} -- {detail}")
    if state == "unconfigured":
        print("          `rime-copilot fetch-grammar` installs one and prints the patch")
    elif state == "missing":
        print("          `rime-copilot fetch-grammar` downloads it")


# `\S+` stops at the first space, so a trailing `# comment` (every real entry
# in private.dict.yaml carries one) is not swallowed into the table name.
# Same shape rime_corpus.lattice.import_tables uses on this exact file.
_IMPORT_LIST_ITEM = re.compile(r"^\s+-\s+(\S+)")


def _import_tables(dict_path: Path) -> "list[str]":
    """The `import_tables:` list of a Rime .dict.yaml, or [] if it has none.

    A deliberately narrow parser -- an `import_tables:` key followed by
    `  - name` items, terminated by the first line that is neither. No YAML
    dependency: this package's promise of importing on a stock interpreter
    would break for a six-line list.
    """
    if not dict_path.is_file():
        return []
    tables: "list[str]" = []
    collecting = False
    for raw in dict_path.read_text(encoding="utf-8").splitlines():
        stripped = raw.strip()
        if not collecting:
            if stripped.startswith("import_tables:"):
                collecting = True
            continue
        if stripped.startswith("#") or not stripped:
            continue
        item = _IMPORT_LIST_ITEM.match(raw)
        if not item:
            break
        tables.append(item.group(1))
    return tables


def _report_missing_import_tables(rime_dir: Path) -> None:
    """Any import table `private.dict.yaml` names that is not on disk.

    Not cosmetic: dict_compiler.cc:54 returns false on the first missing path,
    so this state is not a degraded dictionary but no dictionary at all -- the
    input method stops converting. It is reachable by ordinary means (a
    `restore` that brought private.dict.yaml before the CLI that generates
    private/personal.dict.yaml), so status must name it.
    """
    dict_path = rime_dir / "private.dict.yaml"
    missing = [name for name in _import_tables(dict_path)
               if not (rime_dir / f"{name}.dict.yaml").is_file()]
    if not missing:
        return
    for name in missing:
        print(f"import table: {name} is named by {dict_path.name} but "
              f"{name}.dict.yaml is not on disk -- the dictionary build will fail")
    if any(name.endswith("personal") for name in missing):
        print("  fix: rime-copilot personal")


def cmd_status(args) -> int:
    rime_dir = args.rime_dir
    print(f"rime dir: {rime_dir}")
    try:
        store = paths.vault_dir(rime_dir)
        print(f"vault:    {store}")
        records = vault.read_manifest(store)
        # The same value apply_backup stores (installation_id, not a hostname);
        # comparing anything else would silently never match.
        this_machine = paths.machine_id(rime_dir)
        for action in vault.plan_restore(rime_dir, store):
            if action.kind == "identical":
                continue
            print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
            if action.kind != "conflict":
                continue
            # A conflict is two very different situations wearing one word,
            # and the manifest already knows which: it records the machine
            # that last backed each file up. Local-differs-from-my-own-backup
            # is not a conflict at all -- it is an un-backed-up edit, and the
            # consequence is silent, because another machine's `restore` will
            # quietly bring down the older content and report success. That is
            # exactly what happened to double_pinyin_flypy.custom.yaml after a
            # day of config changes.
            record = records.get(action.rel)
            if record is not None and record.machine == this_machine:
                print(f"                    edited here since your own backup "
                      f"({record.backed_up_at}); other machines will restore the "
                      f"OLDER copy until you run `rime-copilot backup`")
            elif record is not None:
                print(f"                    the vault's copy came from "
                      f"{record.machine} at {record.backed_up_at}; reconcile by "
                      f"hand before backup or restore --force")
    except (FileNotFoundError, LookupError) as exc:
        print(f"vault:    unavailable ({exc})")

    _report_missing_import_tables(rime_dir)

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

    _print_lexicon_status(rime_dir)
    _print_grammar_status(rime_dir)
    _print_model_status(rime_dir)
    _print_installed_status(rime_dir)
    # Both lines are gated on the feature actually being on. It ships off, and
    # a line that prints on every run on every machine -- including the ones
    # with no tmux -- is how "read every line of `status`" stops being a
    # discipline. The processor-order check still fires for everyone who
    # enabled it, which is the only population it protects.
    built_schemas = sorted((rime_dir / "build").glob("*.schema.yaml"))
    ctx_enabled = _context_memory_enabled(built_schemas)
    ctx_line = _context_memory_status(Path.home() / ".tmux.conf", ctx_enabled)
    if ctx_line:
        print(ctx_line)
    if ctx_enabled:
        print(_processor_order_status(built_schemas))
    return 0


def cmd_backup(args) -> int:
    rime_dir = args.rime_dir
    store, error = _vault_dir_or_error(rime_dir)
    if store is None:
        print(error)
        return 1
    this_machine = paths.machine_id(rime_dir)
    actions = vault.plan_backup(rime_dir, store, machine=this_machine,
                                force=args.force)
    for action in actions:
        print(f"  {action.kind:<17} {action.rel} {action.detail}".rstrip())
    conflicts = [a for a in actions if a.kind == "conflict"]
    if args.dry_run:
        return 1 if conflicts else 0
    vault.apply_backup(rime_dir, store, actions, machine=this_machine, now=_now())
    if conflicts:
        # Non-zero for the same reason restore is: a scripted backup must
        # not mistake a refusal for success. The wording says which way to
        # reconcile, because both directions are real -- the other machine's
        # copy may be the newer one (take it with `restore --force`) or the
        # stale one (`backup --force`).
        print(f"{len(conflicts)} file(s) not backed up (conflict) -- another "
              f"machine's copy would be overwritten. Compare them, then take "
              f"theirs with `restore --force` or keep yours with `backup --force`")
        return 1
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


GGUF_MAGIC = b"GGUF"


def cmd_install_model(args) -> int:
    """Copy a locally-built neural re-ranking model into private/.

    Not `fetch`: this model has no public URL. It is the output of ~14 hours
    of training on a corpus that is not distributed, so on a second machine it
    arrives by scp, rsync or a shared drive rather than by download -- and it
    is deliberately NOT vaulted, because the vault is for what cannot be
    regenerated and holds 1 MB today; a 42 MB derived artifact pushed to four
    devices on every retrain does not belong in it.
    """
    source = Path(args.source).expanduser()
    if not source.is_file():
        print(f"no such file: {source}", file=sys.stderr)
        return 1
    with open(source, "rb") as handle:
        if handle.read(4) != GGUF_MAGIC:
            # Copying the wrong file installs something llama.cpp refuses at
            # load time, which surfaces as re-ranking silently using the db.
            print(f"{source} is not a GGUF file (bad magic)", file=sys.stderr)
            return 1

    private = _private(args.rime_dir)
    target = private / (args.name or source.name)
    if target.is_file() and not args.force:
        print(f"already present, skipping: {target} ({target.stat().st_size} bytes)")
        _print_install_model_config(target)
        return 0
    if args.dry_run:
        print(f"would copy {source} -> {target}")
        return 0

    private.mkdir(parents=True, exist_ok=True)
    # Staged, like every other large file this tool installs: llama.cpp mmaps
    # the model, and a reader must never see a half-written one.
    partial = target.with_name(target.name + ".part")
    shutil.copyfile(source, partial)
    partial.replace(target)
    print(f"{target.stat().st_size} bytes -> {target}")
    _print_install_model_config(target)
    return 0


def _print_install_model_config(target: Path) -> None:
    # Nested, not flat "a/b/c" keys. Both forms work -- this patch replaces
    # the whole `copilot` node either way -- but the flat form's result
    # depends on patch keys applying in lexicographic order (they are a
    # std::map), which is not something a pasted snippet should rest on.
    print("Paste into a schema patch:")
    print("  copilot:")
    print("    rerank:")
    print("      llm:")
    print("        enable: true")
    print(f"        model: private/{target.name}")


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


def cmd_personal(args) -> int:
    """Regenerate the derived personal dictionary.

    Reads custom.dict.yaml and the harvested corpus; writes
    private/personal.dict.yaml, which private.dict.yaml imports. Does NOT feed
    the prediction database -- dict.json already reads custom.dict.yaml as a
    `top` source with `boost: "log"`, and listing a file derived from it would
    stack that offset twice.
    """
    from . import personal as personal_dict
    from .clean import CHART_NAME

    rime_dir = args.rime_dir
    custom = _private(rime_dir) / CUSTOM_DICT_NAME
    output = _private(rime_dir) / PERSONAL_DICT_NAME
    corpus = personal_dict.corpus_dir(args.corpus_dir)
    chart = rime_dir / CHART_NAME

    if not custom.is_file() and not corpus.is_dir():
        print(f"nothing to build from: no {custom} and no corpus at {corpus}")
        return 1

    if not corpus.is_dir() and output.is_file():
        # Regenerating from custom.dict.yaml alone is strictly worse than a
        # corpus-mined file already on disk -- it forgets every corpus-mined
        # word -- and this command cannot tell "no corpus, ever" from "the
        # corpus that produced this file just isn't HERE" (it travels by
        # iCloud, symlinked into ~/.local/share/rime-corpus, so "not here"
        # means the symlinks are absent or iCloud has not finished
        # downloading; see vault.py's VAULTED_FILES comment on
        # private/personal.dict.yaml). The safe read
        # is to leave the good file alone rather than silently downgrade it --
        # this is the ordinary state of a machine that only ever consumes the
        # vault, not a failure.
        print(f"leaving {output} alone: no corpus at {corpus}, and rebuilding "
              f"from {custom.name} alone would replace it with a dictionary "
              f"missing every corpus-mined word")
        print("          expected on a machine that only consumes the vault -- "
              "`restore` already brought the good copy down")
        print("          to regenerate it here for real, harvest a corpus "
              "first: rime-corpus ingest")
        return 0

    if args.dry_run:
        print(f"would write {output} from {custom} and {corpus}")
        return 0

    output.parent.mkdir(parents=True, exist_ok=True)
    version = personal_dict.compute_version(custom, corpus)
    count = personal_dict.generate(custom_path=custom, corpus=corpus,
                                   chart_path=chart if chart.is_file() else None,
                                   output=output, version=version)
    if count == 0:
        print(f"wrote {output}: 0 entries -- an empty import table; nothing in "
              f"{custom.name} or the corpus survived to contribute a word")
    else:
        print(f"wrote {output}: {count} entries")
    if not corpus.is_dir():
        print(f"note: no corpus at {corpus} -- built from {custom.name} alone. "
              f"Harvest one with: rime-corpus ingest")
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

    if args.write_requirements:
        # A maintenance action on the checkout, not an install: it writes
        # back into the source tree, so it runs before -- and instead of --
        # copying anything to the destination.
        if not install.is_source_checkout(source_root):
            print(f"refusing: {source_root} is not a rime-copilot checkout",
                  file=sys.stderr)
            return 1
        print(f"wrote {install.write_requirements(source_root)}")
        return 0

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

    # Before build/deploy: private.dict.yaml imports private/personal, and a
    # missing import table fails the whole dictionary build
    # (dict_compiler.cc:54) rather than degrading quietly.
    code = cmd_personal(args)
    if code != 0:
        return code

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
    backup = sub.add_parser("backup")
    backup.add_argument("--force", action="store_true",
                        help="overwrite a vault copy another machine backed up")
    backup.set_defaults(func=cmd_backup)

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

    install_model = sub.add_parser(
        "install-model",
        help="copy a locally-built neural re-ranking model into private/")
    install_model.add_argument("--from", dest="source", required=True,
                               metavar="PATH", help="the .gguf to install")
    install_model.add_argument("--name", help="filename under private/ (default: as given)")
    install_model.add_argument("--force", action="store_true",
                               help="overwrite an existing file of the same name")
    install_model.set_defaults(func=cmd_install_model)

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
        if name == "update":
            # update chains cmd_personal (build does not), and without this
            # the corpus location is only reachable through the environment
            # -- a test (or a run) that does not name it reads the
            # developer's real corpus at $RIME_CORPUS_DIR, or the
            # ~/.local/share/rime-corpus default.
            command.add_argument(
                "--corpus-dir",
                help="harvested corpus (default: $RIME_CORPUS_DIR, else "
                     "~/.local/share/rime-corpus)")
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

    personal_cmd = sub.add_parser(
        "personal",
        help="regenerate private/personal.dict.yaml from custom.dict.yaml + the corpus")
    personal_cmd.add_argument(
        "--corpus-dir",
        help="harvested corpus (default: $RIME_CORPUS_DIR, else "
             "~/.local/share/rime-corpus)")
    personal_cmd.set_defaults(func=cmd_personal)

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
    install_cmd.add_argument(
        "--write-requirements", action="store_true",
        help="regenerate tools/requirements.txt from RUNTIME_REQUIREMENTS "
             "and exit (writes to the checkout, installs nothing)")
    install_cmd.set_defaults(func=cmd_install)
    return parser


def main(argv: "Sequence[str] | None" = None) -> int:
    args = build_parser().parse_args(argv)
    for attribute, default in (("builder", None), ("max_per_key", -1),
                               ("force_build", False), ("force", False),
                               ("squirrel", None), ("config", None), ("output", None),
                               ("url", None), ("name", None),
                               ("apply", False), ("threshold_high", 100),
                               ("threshold_low", 3), ("corpus_dir", None)):
        if not hasattr(args, attribute):
            setattr(args, attribute, default)
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
