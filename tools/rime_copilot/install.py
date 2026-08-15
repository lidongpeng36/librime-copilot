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
    """The entry point plus every `rime_copilot/*.py`, relative to `source_root`.

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


def interpreter_has_pypinyin(interpreter: str) -> bool:
    """Whether `interpreter` -- an absolute path, not necessarily this
    process -- can import `pypinyin`.

    Runs it as a subprocess rather than `import pypinyin` here: this process
    and the interpreter being pinned happen to be the same binary at install
    time, but the question is about the pinned interpreter, and phrasing it
    as a subprocess check keeps that true even if the two are ever
    different.
    """
    result = subprocess.run([interpreter, "-c", "import pypinyin"],
                            capture_output=True)
    return result.returncode == 0


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
        name = Path(rel).name
        target = dest / rel
        if not target.is_file():
            lines.append(f"{name}: missing")
            continue
        current_sha256 = sha256_file(target)
        if current_sha256 != recorded_sha256:
            lines.append(f"{name}: edited in place")
        elif source_present:
            repo_file = source_root / rel
            if repo_file.is_file() and not _payload_matches(rel, repo_file, target):
                lines.append(f"{name}: differs from the repo")
    return lines
