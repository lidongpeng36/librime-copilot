"""Installing the CLI into `<rime_dir>/private/bin`, so the pipeline runs
without a librime checkout present.

An installed copy is a second copy of `tools/rime_copilot/`, and copies are
exactly what rotted the original `private/bin/`: a hand-copied
`make_copilot_db.py`, a year-old `build_predict` binary, a `from_dict.py`
nobody remembered wrote a rank where a weight belonged. This module cannot
make people careful instead; it records what it installed (`INSTALL_MANIFEST`)
so `drift()` can say, every time `status` runs, exactly how the installed
copy has diverged -- from itself (edited or deleted in place) or from the
repo it came from (which has since moved on).
"""
from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
from dataclasses import asdict, dataclass
from pathlib import Path

from .paths import BUILDER_NAME, sha256_file
from .vault import Action  # same (rel, kind, detail) shape install's plan needs

INSTALL_MANIFEST = ".installed.json"
ENTRY_POINT = "rime-copilot"
PACKAGE = "rime_copilot"

# Nothing beyond the CLI itself. `rime_ctx_report.sh` used to be here, and its
# comment explained that a .tmux.conf hook has to name a path that outlives a
# git checkout. It moved to rime-copilot-clients on 2026-08-31, where the tmux
# plugin sets the hook against its own directory -- so there is no longer a
# path for anyone to name wrongly.
_EXTRA_PAYLOAD: "tuple[str, ...]" = ()

# Present in a real checkout, never in an installed copy: apply_install only
# ever writes payload_files() plus the builder into dest, and test/ is not
# among them. Used to refuse installing *from* an installed copy, which would
# otherwise quietly launder drift into a new "source of truth".
_CHECKOUT_MARKER = "test"


@dataclass(frozen=True)
class Installed:
    source_commit: "str | None"
    source_root: str
    installed_at: str
    files: dict[str, str]


def payload_files(source_root: Path) -> list[str]:
    """The entry point, every `rime_copilot/*.py`, and `_EXTRA_PAYLOAD`.

    Derived from the filesystem, not a hardcoded list, so a module added
    later is installed without editing this function. The package listing is
    non-recursive, so `__pycache__` -- a directory, never matched by `*.py`
    -- is excluded without a special case for it.
    """
    files = []
    if (source_root / ENTRY_POINT).is_file():
        files.append(ENTRY_POINT)
    package = source_root / PACKAGE
    if package.is_dir():
        for path in sorted(package.glob("*.py")):
            files.append(f"{PACKAGE}/{path.name}")
    for rel in _EXTRA_PAYLOAD:
        if (source_root / rel).is_file():
            files.append(rel)
    return files


def is_source_checkout(source_root: Path) -> bool:
    """Whether `source_root` looks like `tools/` in a real checkout.

    `rime_copilot/` alone cannot tell a checkout from an installed copy --
    that directory is exactly what gets installed. `test/` is never
    installed, so its presence is the signal.
    """
    return (source_root / PACKAGE).is_dir() and (source_root / _CHECKOUT_MARKER).is_dir()


def _rewrite_shebang(source: Path, interpreter: str) -> bytes:
    """`source`'s bytes with its shebang line replaced to name `interpreter`.

    Asserts the source actually starts with a shebang instead of blindly
    overwriting whatever its first line happens to be -- `payload_files()` is
    derived from the filesystem, so a future payload file that isn't a
    shebang script must fail loudly here rather than get its first line
    silently mangled.
    """
    data = source.read_bytes()
    first_line, sep, rest = data.partition(b"\n")
    assert sep and first_line.startswith(b"#!"), (
        f"{source}: expected the first line to be a shebang (#!...), found {first_line!r}")
    return f"#!{interpreter}\n".encode("utf-8") + rest


def _entry_point_body(path: Path) -> bytes:
    """`path`'s content after its first line -- for comparing the entry point
    while ignoring a shebang that install() legitimately rewrites."""
    return path.read_bytes().partition(b"\n")[2]


def _payload_matches(rel: str, a: Path, b: Path) -> bool:
    """Whether two copies of the payload file at `rel` should count as the
    same file for plan/drift purposes.

    The entry point's shebang is rewritten at install time to the installing
    interpreter's absolute path (see `_rewrite_shebang`), so a byte-for-byte
    compare against the checkout's `#!/usr/bin/env python3` would call every
    fresh install "different from the repo". Compare bodies only for it;
    every other payload file is compared byte-for-byte, as before.
    """
    if rel == ENTRY_POINT:
        return _entry_point_body(a) == _entry_point_body(b)
    return sha256_file(a) == sha256_file(b)


def installed_interpreter(dest: Path) -> "str | None":
    """The interpreter the installed entry point at `dest` actually runs
    under, read from its shebang. `None` when nothing is installed there, or
    its first line is not a shebang.

    Read from disk rather than recorded in the manifest because the shebang
    is what the kernel obeys: a hand-edited one must be reported as the
    truth it is, not papered over by a record of what `install` once
    intended.
    """
    try:
        with (dest / ENTRY_POINT).open("rb") as handle:
            first_line = handle.readline()
    except OSError:
        return None
    if not first_line.startswith(b"#!"):
        return None
    return first_line[2:].decode("utf-8", "replace").strip() or None


PYTHON_VERSION_FILE = ".python-version"


@dataclass(frozen=True)
class Declared:
    """The interpreter the install destination itself asks for."""
    version_file: Path
    version: str
    interpreter: "str | None"  # None when `version` could not be resolved to a path
    problem: "str | None"      # why, when it could not


def find_python_version_file(dest: Path) -> "Path | None":
    """The nearest `.python-version` at or above `dest`, as pyenv would find
    it -- nearest first, walking up to the filesystem root.

    Deliberately our own search rather than letting `pyenv which` do the
    walking: pyenv always answers, falling back to the global version when
    no file exists anywhere, and "the destination declared an interpreter"
    and "pyenv has a global default" are not remotely the same statement.
    Only the first is a reason to override the interpreter running install.
    """
    for directory in [dest, *dest.parents]:
        candidate = directory / PYTHON_VERSION_FILE
        if candidate.is_file():
            return candidate
    return None


def declared_interpreter(dest: Path) -> "Declared | None":
    """The interpreter `dest` declares for itself via `.python-version`, or
    `None` when it declares nothing.

    `~/Library/Rime/private/.python-version` already said `rime` while
    `install` was pinning whatever the caller's shell resolved from some
    unrelated parent directory. The declaration sits next to the thing that
    needs it and survives being installed from anywhere, which is exactly
    what `sys.executable` does not do.

    Never raises: a declaration naming a version pyenv cannot resolve is a
    thing to report and fall back from, not a reason `install` should fail.
    """
    version_file = find_python_version_file(dest)
    if version_file is None:
        return None
    try:
        content = version_file.read_text(encoding="utf-8")
    except OSError as exc:
        return Declared(version_file, "", None, f"cannot be read: {exc}")
    # pyenv accepts several versions in one file; the first is the one that
    # provides `python3`, and the only one worth naming in a plan line.
    names = [line.strip() for line in content.splitlines() if line.strip()]
    if not names:
        return None
    version = names[0]

    # `pyenv which python3` rather than guessing `$PYENV_ROOT/versions/<v>/
    # bin/python3`: pyenv owns the mapping from a name to a path -- plain
    # versions, named virtualenvs, `system` -- and reimplementing it here
    # would be a second, quietly diverging copy of it.
    #
    # PYENV_DIR, not just cwd: pyenv searches from `${PYENV_DIR:-$PWD}`, and
    # `$PWD` in the child comes from the *inherited environment variable*,
    # which subprocess's `cwd=` does not update. Passing cwd alone therefore
    # resolved against wherever `install` was invoked from -- reporting the
    # destination's `rime` while handing back the checkout's `llama`, which
    # is the whole bug wearing a correct-looking label. PYENV_VERSION is
    # dropped so an ambient `pyenv shell` cannot outrank the file either.
    directory = str(version_file.parent)
    environment = {k: v for k, v in os.environ.items() if k != "PYENV_VERSION"}
    environment["PYENV_DIR"] = directory
    environment["PWD"] = directory
    try:
        result = subprocess.run(["pyenv", "which", "python3"], cwd=directory,
                                capture_output=True, text=True, env=environment, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return Declared(version_file, version, None,
                        "pyenv is not on PATH, so the name cannot be resolved to a path")
    if result.returncode != 0:
        detail = result.stderr.strip().splitlines()
        return Declared(version_file, version, None,
                        detail[-1] if detail else "pyenv could not resolve it")
    # Taken as given, not resolved: a virtualenv's bin/python3 is a symlink
    # to its base interpreter, the one interpreter without its packages.
    path = result.stdout.strip()
    if not path or not os.access(path, os.X_OK):
        return Declared(version_file, version, None,
                        f"pyenv named {path or '(nothing)'}, which is not an executable")
    return Declared(version_file, version, path, None)


@dataclass(frozen=True)
class Requirement:
    module: str   # what the code imports
    package: str  # what pip installs to provide it -- not always the same
    breaks: str   # what stops working without it


# Every third-party module this package imports, and what its absence costs.
# The imports are lazy by design, so that the CLI starts on a stock
# interpreter -- which also means a missing one surfaces as an ImportError
# partway through a subcommand, on a machine whose whole point is to have no
# checkout to read. Install time is the one moment the whole set is cheap to
# check at once, so it checks the whole set: `pypinyin` alone was not enough
# once, on an interpreter that had it and had no `bs4`.
RUNTIME_REQUIREMENTS = (
    Requirement("pypinyin", "pypinyin",
                "`build` and `update` on any dictionary without a pinyin column "
                "(e.g. tencent.dict.yaml), and `personal`/`update` for any "
                "corpus-mined word (it has no pinyin column either)"),
    Requirement("jieba", "jieba",
                "`clean` (telling a real word from a Sogou sentence fragment), "
                "and `personal`/`update` for the same oracle (personal.py's "
                "load_lexicon)"),
    Requirement("requests", "requests", "`fetch` (downloading .scel cell dictionaries)"),
    Requirement("bs4", "beautifulsoup4", "`fetch` (finding the .scel download links)"),
)

REQUIREMENTS_NAME = "requirements.txt"


def requirements_text() -> str:
    """RUNTIME_REQUIREMENTS rendered as a pip requirements file.

    Generated rather than hand-kept, because a hand-kept copy is what the
    tree already had and it was wrong in both available ways: it had gone
    stale (no `jieba`, added to RUNTIME_REQUIREMENTS long after) and it
    named import names instead of pip names (`bs4`, a forwarding shim, in
    place of `beautifulsoup4`). Nothing in the tree read the file, so its
    only reader was a human provisioning a new machine -- who would end up
    without `clean` and with no indication why.

    The `package` field is the one that belongs here: `module` is what the
    code imports, and the two differ exactly where getting it wrong is
    quietest.
    """
    header = (f"# Generated from RUNTIME_REQUIREMENTS in rime_copilot/install.py.\n"
              f"# Regenerate with: rime-copilot install --write-requirements\n"
              f"# `rime-copilot status` checks the pinned interpreter against the\n"
              f"# same list, so this file and that check cannot disagree.\n")
    return header + "".join(f"{r.package}\n" for r in RUNTIME_REQUIREMENTS)


def write_requirements(source_root: Path) -> Path:
    """Regenerate `source_root/requirements.txt`. Returns the path written."""
    path = source_root / REQUIREMENTS_NAME
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(requirements_text(), encoding="utf-8")
    temporary.replace(path)
    return path


# One subprocess for the whole set: an interpreter start costs far more than
# the imports do, and a check that scales its cost with the number of
# dependencies is a check someone eventually moves out of `status`.
_PROBE = """
import sys
for name in sys.argv[1:]:
    try:
        __import__(name)
    except Exception:
        print(name)
"""


def missing_requirements(interpreter: str,
                         requirements: "tuple[Requirement, ...] | None" = None
                         ) -> list[Requirement]:
    """Which of `requirements` `interpreter` cannot import.

    Runs it as a subprocess rather than importing here: the two are the same
    binary when `install` runs without `--python`, but the question is always
    about the *pinned* interpreter, and a subprocess keeps the answer true
    when they differ.

    An interpreter that cannot be run at all counts as missing everything --
    a pinned virtualenv that has since been deleted is the loudest form of
    this problem, not an exemption from reporting it.
    """
    requirements = RUNTIME_REQUIREMENTS if requirements is None else requirements
    try:
        result = subprocess.run([interpreter, "-c", _PROBE, *(r.module for r in requirements)],
                                capture_output=True, text=True)
    except OSError:
        return list(requirements)
    if result.returncode != 0:
        return list(requirements)
    missing = set(result.stdout.split())
    return [r for r in requirements if r.module in missing]


def plan_install(source_root: Path, dest: Path, builder: Path) -> list[Action]:
    actions = []
    for rel in [*payload_files(source_root), BUILDER_NAME]:
        src = builder if rel == BUILDER_NAME else source_root / rel
        target = dest / rel
        if not target.is_file():
            actions.append(Action(rel, "install"))
        elif _payload_matches(rel, target, src):
            actions.append(Action(rel, "identical"))
        else:
            actions.append(Action(rel, "update"))
    return actions


def _install_one(src: Path, target: Path, *, interpreter: "str | None" = None) -> str:
    target.parent.mkdir(parents=True, exist_ok=True)
    # Temp-then-rename, matching vault.apply_backup: an installed script must
    # never be observed half-written by whatever process runs it next.
    temporary = target.with_name(target.name + ".new")
    if interpreter is None:
        shutil.copy2(src, temporary)
    else:
        temporary.write_bytes(_rewrite_shebang(src, interpreter))
        shutil.copymode(src, temporary)  # keep the executable bit; content already differs
    temporary.replace(target)
    # Hashed after the rewrite, not the source: the manifest must record what
    # actually landed on disk, so drift() compares dest against dest's own
    # past, not against a source file whose shebang was never installed as-is.
    return sha256_file(target)


def apply_install(source_root: Path, dest: Path, builder: Path,
                  commit: "str | None", now: str,
                  interpreter: "str | None" = None) -> None:
    if not is_source_checkout(source_root):
        raise ValueError(
            f"refusing to install from {source_root}: not a rime-copilot checkout "
            f"(no {PACKAGE}/ + {_CHECKOUT_MARKER}/ found there) -- installing from an "
            f"installed copy would launder drift instead of reporting it")

    interpreter = interpreter or sys.executable
    dest.mkdir(parents=True, exist_ok=True)
    files = {}
    for rel in payload_files(source_root):
        files[rel] = _install_one(source_root / rel, dest / rel,
                                  interpreter=interpreter if rel == ENTRY_POINT else None)
    # Not recorded in `files`: the builder is a compiled, per-architecture
    # artifact that does not come from source_root, so it is not part of the
    # payload drift() compares against the repo.
    _install_one(builder, dest / BUILDER_NAME)

    # Written last: an install interrupted after copying some files but
    # before this point must read back as "not installed", not "complete".
    _write_manifest(dest, Installed(source_commit=commit, source_root=str(source_root),
                                    installed_at=now, files=files))


def _write_manifest(dest: Path, installed: Installed) -> None:
    path = dest / INSTALL_MANIFEST
    temporary = path.with_name(path.name + ".new")
    temporary.write_text(json.dumps(asdict(installed), ensure_ascii=False, indent=2) + "\n",
                         encoding="utf-8")
    temporary.replace(path)


def read_install_manifest(dest: Path) -> "Installed | None":
    path = dest / INSTALL_MANIFEST
    if not path.is_file():
        return None
    try:
        raw = json.loads(path.read_text(encoding="utf-8"))
    except (ValueError, OSError):
        return None  # unreadable manifest is no worse than none: reads as not installed
    return Installed(**raw)


def drift(dest: Path) -> list[str]:
    """Human-readable lines describing how the installed copy at `dest` has
    diverged from what `apply_install` recorded. `[]` when in sync.

    Never raises when the recorded source_root is gone -- that is the normal
    standalone case this whole module exists for -- it just cannot compare
    against a repo that is not there, and skips that one check.
    """
    installed = read_install_manifest(dest)
    if installed is None:
        return []

    source_root = Path(installed.source_root)
    source_present = source_root.is_dir()
    lines = []
    for rel, recorded_sha256 in sorted(installed.files.items()):
        # `rel` is dest-relative for everything under `dest` (the ordinary
        # payload) but ABSOLUTE for the tmux reporter's operative copy, which
        # does not live under `dest` at all -- `dest / rel` already resolves
        # correctly either way (pathlib discards the left side when the right
        # is absolute), but `source_root / rel` would too, which would
        # compare that file against ITSELF instead of against the repo's
        # copy. Go by basename against source_root for an absolute key.
        name = Path(rel).name
        target = dest / rel
        if not target.is_file():
            lines.append(f"{name}: missing")
            continue
        current_sha256 = sha256_file(target)
        if current_sha256 != recorded_sha256:
            lines.append(f"{name}: edited in place")
        elif source_present:
            repo_file = source_root / (name if Path(rel).is_absolute() else rel)
            if repo_file.is_file() and not _payload_matches(rel, repo_file, target):
                lines.append(f"{name}: differs from the repo")
    return lines
