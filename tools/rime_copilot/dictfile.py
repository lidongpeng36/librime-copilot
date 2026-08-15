"""Reading and writing Rime `*.dict.yaml` files.

Every Rime dictionary opens with a `---` … `...` metadata block. Parsing it as
vocabulary is not a cosmetic bug: `columns:` / `  - text` split into pairs that
build_copilot rejects noisily, while `name: custom` splits into three
whitespace-separated columns that it accepts silently, putting `name: -> custom`
into the database. See 7312800.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Iterable, Iterator, Sequence

# Dictionaries with no weight column (others.dict.yaml) assert only that a word
# exists. Give them a floor level with ext/sogou/tencent so they rank behind
# anything carrying a real frequency.
DEFAULT_WEIGHT = 100


@dataclass(frozen=True)
class Entry:
    word: str
    pinyin: str
    weight: float


def iter_body_lines(lines: Iterable[str], source: str) -> Iterator[str]:
    """Yield only the vocabulary, skipping the `---` … `...` metadata block.

    A `---` that never closes means the file is not what we think it is; refuse
    it rather than swallow the whole dictionary as metadata and build an empty
    database without a word of complaint.
    """
    in_header = False
    for line in lines:
        stripped = line.strip()
        if in_header:
            if stripped == "...":
                in_header = False
            continue
        if stripped == "---":
            in_header = True
            continue
        yield line
    if in_header:
        raise ValueError(f"{source}: YAML header opened with `---` and never closed with `...`")


def read_entries(path: Path, *, traditional: bool = False) -> list[Entry]:
    convert = None
    if traditional:
        from opencc import OpenCC  # lazy: pure parsing must not need it
        convert = OpenCC("s2t").convert

    entries: list[Entry] = []
    unusable: list[str] = []
    with open(path, "r", encoding="utf-8") as handle:
        for raw in iter_body_lines(handle, str(path)):
            line = raw.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) == 3:
                word, pinyin, weight = parts
            elif len(parts) == 2:
                # Two shapes exist: `word⇥weight` (tencent) and `word⇥pinyin`
                # (others). An integer second column is a weight.
                word, second = parts
                if second.strip().isdigit():
                    pinyin, weight = _auto_pinyin(word), second
                else:
                    pinyin, weight = second, str(DEFAULT_WEIGHT)
            elif len(parts) == 1:
                word, pinyin, weight = parts[0], _auto_pinyin(parts[0]), str(DEFAULT_WEIGHT)
            else:
                continue

            if convert:
                word = convert(word)
            try:
                numeric = int(weight)
            except ValueError:
                continue
            # A word with whitespace cannot survive build_copilot's whitespace
            # column split. Dropping it beats writing a misaligned entry.
            if any(c.isspace() for c in word):
                unusable.append(word)
                continue
            entries.append(Entry(word, pinyin, numeric))

    if unusable:
        preview = ", ".join(repr(w) for w in unusable[:5])
        print(f"⚠️  {path}: skipped {len(unusable)} word(s) containing whitespace: {preview}")
    return entries


def _auto_pinyin(word: str) -> str:
    # lazy: only dictionaries missing a pinyin column pay for this import
    try:
        from pypinyin import lazy_pinyin
    except ImportError as exc:
        raise ImportError(
            "pypinyin is not importable, but this dictionary has no pinyin "
            "column and needs it to generate one (e.g. tencent.dict.yaml, "
            "which is word+weight only). If pypinyin is installed somewhere, "
            "this is likely the wrong interpreter — a bare shebang can "
            "resolve differently depending on the current directory (e.g. "
            "via pyenv's per-directory .python-version). Run this with the "
            "interpreter that has pypinyin installed."
        ) from exc
    return " ".join(lazy_pinyin(word))


def write_dict(path: Path, *, name: str, version: str, entries: Iterable[Entry],
               comment_lines: Sequence[str] = (), use_preset_vocabulary: bool = False) -> int:
    count = 0
    with open(path, "w", encoding="utf-8") as out:
        for comment in comment_lines:
            out.write(comment.rstrip("\n") + "\n")
        if comment_lines:
            out.write("\n")
        out.write("---\n")
        out.write(f"name: {name}\n")
        out.write(f'version: "{version}"\n')
        out.write("sort: by_weight\n")
        if use_preset_vocabulary:
            out.write("use_preset_vocabulary: true\n")
        out.write("...\n\n")
        for entry in entries:
            out.write(f"{entry.word}\t{entry.pinyin}\t{entry.weight}\n")
            count += 1
    return count
