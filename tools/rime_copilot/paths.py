"""Where things are: the Rime directory, the iCloud sync directory, the builder.

Nothing here touches the network or mutates anything. `installation.yaml` is
read with a regex rather than a YAML parser so that the package keeps its
promise of importing on a stock interpreter.
"""
from __future__ import annotations

import hashlib
import os
import re
import shutil
from pathlib import Path

BUILDER_NAME = "build_copilot"
DEFAULT_RIME_DIR = Path.home() / "Library" / "Rime"

# Relative to a librime checkout root, where the plugin's CMake puts the binary.
_BUILD_TREE_BUILDER = Path("build/plugins/copilot/bin") / BUILDER_NAME


def resolve_rime_dir(explicit: str | None = None) -> Path:
    if explicit:
        return Path(explicit).expanduser()
    from_env = os.environ.get("RIME_DIR")
    if from_env:
        return Path(from_env).expanduser()
    return DEFAULT_RIME_DIR


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def read_installation_scalar(rime_dir: Path, key: str) -> str:
    path = rime_dir / "installation.yaml"
    try:
        text = path.read_text(encoding="utf-8")
    except FileNotFoundError as exc:
        raise FileNotFoundError(f"no installation.yaml at {path}") from exc
    match = re.search(rf'^{re.escape(key)}:\s*"?(.*?)"?\s*$', text, re.M)
    if not match or not match.group(1):
        raise LookupError(f"no `{key}` in {path}")
    return match.group(1)


def read_sync_dir(rime_dir: Path) -> Path:
    return Path(read_installation_scalar(rime_dir, "sync_dir"))


def machine_id(rime_dir: Path) -> str:
    return read_installation_scalar(rime_dir, "installation_id")


def vault_dir(rime_dir: Path) -> Path:
    # Deliberately NOT <sync_dir>/<installation_id>/, which Rime's own sync
    # task manages — same reasoning as tools/sync_telemetry.sh.
    return read_sync_dir(rime_dir) / "copilot_vault"


def find_builder(explicit: str | None = None, start: Path | None = None) -> Path:
    if explicit:
        candidate = Path(explicit).expanduser()
        if not candidate.is_file():
            raise FileNotFoundError(f"builder not found: {candidate}")
        return candidate

    from_env = os.environ.get("COPILOT_BUILD_DIR")
    if from_env:
        candidate = Path(from_env).expanduser() / "bin" / BUILDER_NAME
        if candidate.is_file():
            return candidate

    here = (start or Path(__file__).resolve().parent).resolve()
    for parent in [here, *here.parents]:
        candidate = parent / _BUILD_TREE_BUILDER
        if candidate.is_file():
            return candidate

    found = shutil.which(BUILDER_NAME)
    if found:
        return Path(found)

    raise FileNotFoundError(
        f"cannot find {BUILDER_NAME}. Build it in the librime tree:\n"
        f"  cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build\n"
        f"then it lands at <librime>/{_BUILD_TREE_BUILDER}.\n"
        f"Or pass --builder, or set COPILOT_BUILD_DIR to the build directory."
    )
