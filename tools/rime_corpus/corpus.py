"""The corpus record: filtering, content-hash dedupe, and JSONL on disk.

The corpus stores TEXT, not keystrokes and not pinyin. That is what lets one
corpus serve a 全拼 user, a 双拼 user, and a future model's training set --
conversion happens at replay time (see speller.py).

The unit is an utterance (one chat message, one prompt), not a sentence.
Sentence splitting is a replay policy that will be tuned repeatedly; freezing
it here would force a corpus rebuild every time it changes.
"""
from __future__ import annotations

import hashlib
import json
import os
import re
from pathlib import Path
from typing import Iterable, Iterator

from . import redact as _redact

VERSION = 1
MAX_CHARS = 500
DEFAULT_CORPUS_DIR = Path.home() / ".local" / "share" / "rime-corpus"

# Han ideographs. Mirrors rerank_detail::IsHanIdeograph in src/rerank.h -- the
# same definition of "context" the plugin uses, so the corpus and the thing
# being measured agree on what Chinese is.
#
# Defined ONCE as a character class, because replay.py needs the same
# definition to find runs. Two regexes that must agree is one regex too many.
#
# ESCAPES ONLY. Written as literal characters this silently became
# [U+8C48-U+FAFF] -- 豈 U+8C48 and 豈 U+F900 are indistinguishable in every
# font -- which matched Hangul and the whole Private Use Area as "Han".
HAN_CLASS = "一-鿿㐀-䶿豈-﫿𠀀-𯨟"
_HAN = re.compile(f"[{HAN_CLASS}]")


def corpus_dir(explicit: str | None = None) -> Path:
    """Where the corpus lives.

    Deliberately outside <rime_dir>: vault.py's VAULTED_FILES is an explicit
    allowlist and would not pick it up, but ~/Library/Rime is the target of
    Rime's own sync and of whole-directory backups. Private text has no
    business there.
    """
    if explicit:
        return Path(explicit).expanduser()
    from_env = os.environ.get("RIME_CORPUS_DIR")
    if from_env:
        return Path(from_env).expanduser()
    return DEFAULT_CORPUS_DIR


def has_han(text: str) -> bool:
    return bool(_HAN.search(text))


def han_chars(text: str) -> int:
    """How many Han characters. Public so cli.py need not reach into _HAN."""
    return len(_HAN.findall(text))


def make_record(src: str, ts: str, text: str) -> dict | None:
    """Redact, filter, and shape one utterance. None when it does not qualify."""
    text = text.strip()
    # Conservative: checked against raw length before redaction, since placeholders are shorter than what they replace.
    if not text or len(text) > MAX_CHARS:
        return None
    text, categories = _redact.redact(text)
    # Filtered AFTER redaction: a placeholder is not Han, so an utterance whose
    # only "Chinese" was inside a redacted span correctly drops out here.
    if not has_han(text):
        return None
    digest = hashlib.sha256(text.encode("utf-8")).hexdigest()[:16]
    return {
        "v": VERSION,
        "id": digest,
        "src": src,
        "ts": ts,
        "text": text,
        "redacted": categories,
    }


def iter_records(path: Path) -> Iterator[dict]:
    if not path.exists():
        return
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                yield json.loads(line)


def load_ids(path: Path) -> set[str]:
    return {record["id"] for record in iter_records(path)}


def append(path: Path, records: Iterable[dict]) -> int:
    """Append records whose id is not already present. Returns how many landed."""
    seen = load_ids(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    written = 0
    with open(path, "a", encoding="utf-8") as handle:
        for record in records:
            if record is None or record["id"] in seen:
                continue
            seen.add(record["id"])
            handle.write(json.dumps(record, ensure_ascii=False) + "\n")
            written += 1
    return written
