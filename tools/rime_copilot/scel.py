"""Sogou cell dictionaries: unpacking the binary, and fetching new ones.

`unpack` takes bytes rather than a path so it is pure and testable against
synthetic blobs. Only `download_all` touches the network, and only it imports
`requests` and `bs4`.

Format reference:
  https://github.com/archerhu/scel2mmseg/blob/master/scel2mmseg.py
"""
from __future__ import annotations

import io
import struct
from pathlib import Path
from typing import Sequence

from .dictfile import DEFAULT_WEIGHT, Entry

DICT_URLS = (
    "https://pinyin.sogou.com/dict/detail/index/4",      # 网络流行新词
    "https://pinyin.sogou.com/dict/detail/index/15130",  # 中国历史词汇大全
    "https://pinyin.sogou.com/dict/detail/index/11508",  # 中国风景名胜
    "https://pinyin.sogou.com/dict/detail/index/19447",  # 香港特别行政区城市信息精选
)

# Sogou answers errors with HTTP 200 and an HTML body. These keep the garbage
# out rather than letting it overwrite a good dictionary.
SCEL_MAGIC = b"\x40\x15\x00\x00"
MIN_SCEL_SIZE = 4096

_PINYIN_TABLE = 0x1540 + 4
_WORD_TABLE = {0x44: 0x2628, 0x45: 0x26c4}


def unpack(data: bytes) -> list[tuple[str, str]]:
    if len(data) < 5 or data[4] not in _WORD_TABLE:
        raise ValueError(f"not a supported .scel: mask byte {data[4:5]!r}")
    word_table = _WORD_TABLE[data[4]]
    stream = io.BytesIO(data)

    syllables = _read_pinyin_table(stream, word_table)
    stream.seek(word_table)
    records: list[tuple[str, str]] = []
    size = len(data)
    while stream.tell() < size:
        header = stream.read(4)
        if len(header) < 4:
            break
        homophones, index_bytes = struct.unpack("<HH", header)
        # An odd index-array length is trailing garbage, not a record: the
        # array is u16s. struct.unpack would demand index_bytes - 1 bytes and
        # raise on the complete-but-odd buffer, which the truncation guard
        # below cannot catch because nothing was truncated.
        if index_bytes % 2:
            break
        indices = stream.read(index_bytes)
        if len(indices) < index_bytes:
            break
        try:
            pinyin = " ".join(syllables[i] for i in
                              struct.unpack(f"<{index_bytes // 2}H", indices))
        except KeyError:
            break  # unknown syllable index: the rest cannot be trusted
        for _ in range(homophones):
            length_bytes = stream.read(2)
            if len(length_bytes) < 2:
                return records
            (length,) = struct.unpack("<H", length_bytes)
            word = stream.read(length)
            if len(word) < length or len(stream.read(12)) < 12:
                return records
            try:
                records.append((pinyin, word.decode("utf-16-le")))
            except UnicodeDecodeError:
                return records
    return records


def _read_pinyin_table(stream: io.BytesIO, word_table: int) -> dict[int, str]:
    stream.seek(_PINYIN_TABLE)
    syllables: dict[int, str] = {}
    previous = -1
    while stream.tell() < word_table:
        index, length = struct.unpack("<HH", stream.read(4))
        syllables.setdefault(index, stream.read(length).decode("utf-16-le"))
        if index - previous != 1:
            break  # ran off the end of the table into padding
        previous = index
    return syllables


def merge_scel_dir(indir: Path, weight: int = DEFAULT_WEIGHT) -> tuple[list[Entry], list[str]]:
    """Unpack every .scel in `indir`, deduplicated across files.

    The cell dictionaries overlap: measured at roughly 7,000 duplicate lines in
    a 150,000-line merge. First occurrence wins, preserving file order.
    """
    entries: list[Entry] = []
    names: list[str] = []
    seen: set[tuple[str, str]] = set()
    for path in sorted(indir.glob("*.scel")):
        names.append(path.stem)
        for pinyin, word in unpack(path.read_bytes()):
            if (word, pinyin) in seen:
                continue
            seen.add((word, pinyin))
            entries.append(Entry(word, pinyin, weight))
    return entries, names


def download_all(urls: Sequence[str] = DICT_URLS, outdir: Path = Path("scel")) -> list[Path]:
    """Download each dictionary, validating before anything reaches disk.

    Raises if any download fails, so a caller cannot mistake a partial set for
    a complete one and overwrite a good dictionary with a shrunken rebuild.
    """
    import requests  # lazy: unpacking must not need the network stack
    from bs4 import BeautifulSoup
    from urllib.parse import urljoin

    headers = {"User-Agent": "Mozilla/5.0"}
    outdir.mkdir(parents=True, exist_ok=True)
    written: list[Path] = []
    failures: list[str] = []

    for url in urls:
        try:
            page = requests.get(url, headers=headers, timeout=10)
            page.raise_for_status()
            soup = BeautifulSoup(page.text, "html.parser")
            title_tag = soup.select_one("div.dict_detail_title")
            title = title_tag.text.strip() if title_tag else "unknown"
            button = soup.select_one("div#dict_dl_btn a")
            if not button:
                raise RuntimeError("no download button on the detail page")

            body = requests.get(urljoin(url, button["href"]), headers=headers, timeout=15)
            body.raise_for_status()
            data = body.content
            if len(data) < MIN_SCEL_SIZE:
                raise RuntimeError(f"only {len(data)} bytes; probably not a dictionary")
            if not data.startswith(SCEL_MAGIC):
                raise RuntimeError(f"header is not .scel: {data[:8]!r}")

            safe = "".join(c for c in title if c.isalnum() or c in "（）-_" or c.isspace())
            target = outdir / f"{safe}.scel"
            partial = target.with_suffix(".scel.part")
            partial.write_bytes(data)
            partial.replace(target)  # atomic: never leaves half a file
            written.append(target)
            print(f"[✔] {target} ({len(data)} bytes)")
        except Exception as exc:  # noqa: BLE001 — report every URL, fail at the end
            print(f"[✘] {url}: {exc}")
            failures.append(url)

    if failures:
        raise RuntimeError(f"{len(failures)}/{len(urls)} downloads failed; "
                           f"refusing to rebuild from an incomplete set")
    return written
