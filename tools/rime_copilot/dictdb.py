"""Merging Rime dictionaries into the prefix→suffix pairs build_copilot eats.

Weight convention, which must match src/db_provider.h and src/rerank.h:
**larger = more likely**. Writing a rank here silently inverts every ordering
in the plugin.
"""
from __future__ import annotations

import json
import math
from collections import defaultdict
from dataclasses import dataclass
from pathlib import Path
from typing import Sequence

from .dictfile import Entry


@dataclass(frozen=True)
class Source:
    path: Path
    top: bool = False
    scale: float | None = None
    scale_range: "tuple[int, int] | None" = None
    boost: "str | None" = None


def load_sources(config_path: Path) -> list[Source]:
    with open(config_path, "r", encoding="utf-8") as handle:
        config = json.load(handle)
    sources = []
    for item in config:
        path = Path(item["dict"]).expanduser()
        if "scale" in item and "range" in item:
            raise ValueError(f"{path}: `scale` and `range` are mutually exclusive")
        scale_range = tuple(item["range"]) if "range" in item else None
        boost = item.get("boost")
        if boost is not None:
            if boost != "log":
                raise ValueError(f"{path}: unknown boost {boost!r}; the only mode is 'log'")
            if not item.get("top"):
                raise ValueError(f"{path}: `boost` only applies to a `top` source")
            if "scale" in item or "range" in item:
                # _apply_shaping runs before the boost, so a linear/range
                # rescale would compose non-linearly with the log
                # normalisation -- refuse rather than silently produce a
                # weight nobody chose.
                raise ValueError(f"{path}: `boost` is mutually exclusive with "
                                 f"`scale`/`range`")
        sources.append(Source(path=path, top=bool(item.get("top")),
                              scale=item.get("scale"), scale_range=scale_range,
                              boost=boost))
    return sources


def scale_weights(entries: Sequence[Entry], target_min: int, target_max: int) -> list[Entry]:
    """Linear min-max rescale.

    Use sparingly. Word frequency is long-tailed, so a linear rescale flattens
    almost everything: measured on one dictionary, 1.07% of 542,928 entries
    landed above the lower bound and the rest tied. Prefer `scale`, or keep the
    original frequencies.
    """
    if not entries:
        return []
    weights = [e.weight for e in entries]
    low, high = min(weights), max(weights)
    if low == high:
        return [Entry(e.word, e.pinyin, 100) for e in entries]
    return [Entry(e.word, e.pinyin,
                  int((e.weight - low) / (high - low) * (target_max - target_min)) + target_min)
            for e in entries]


def merge(loaded: Sequence["tuple[Source, list[Entry]]"]) -> list[Entry]:
    """Combine every source, then sort by (first character, descending weight).

    A `top` source is not a weight replacement but an offset stacked on top:
    `ceiling + existing + own`. Every one of its entries therefore outranks
    every ordinary entry while its own frequency order survives inside the
    boost. Replacing outright was measured to invert real frequencies.
    """
    merged: dict[tuple[str, str], float] = {}

    for source, entries in loaded:
        if source.top:
            continue
        entries = _apply_shaping(source, entries)
        for e in entries:
            key = (e.word, e.pinyin)
            if key not in merged or e.weight > merged[key]:
                merged[key] = e.weight

    ceiling = max(merged.values(), default=0)
    for source, entries in loaded:
        if not source.top:
            continue
        shaped = _apply_shaping(source, entries)
        # `boost: "log"` gives the personal frequency the SAME RANGE as the
        # public one — both 0..ceiling — instead of 1-2877 against a term up to
        # ~19M. It does not make the personal term lexicographically first, and
        # a public weight near the ceiling can still win; see the test that
        # pins that. Log rather than linear because commit counts are
        # long-tailed: 34.7% of this lexicon is a single commit, and a linear
        # rescale would flatten everything below the top few hundred.
        # Still additive, not a replacement: replacing outright was measured to
        # invert real frequencies (see the `top` note above).
        span = None
        if source.boost == "log":
            largest = max((e.weight for e in shaped), default=0)
            span = math.log1p(largest) if largest > 0 else 0
        for e in shaped:
            key = (e.word, e.pinyin)
            own = e.weight
            if span:
                own = ceiling * math.log1p(e.weight) / span
            merged[key] = ceiling + merged.get(key, 0) + own

    result = [Entry(word, pinyin, weight) for (word, pinyin), weight in merged.items()]
    result.sort(key=lambda e: (e.word[0], -e.weight))
    return result


def _apply_shaping(source: Source, entries: Sequence[Entry]) -> list[Entry]:
    if source.scale is not None:
        return [Entry(e.word, e.pinyin, e.weight * source.scale) for e in entries]
    if source.scale_range is not None:
        return scale_weights(entries, source.scale_range[0], source.scale_range[1])
    return list(entries)


def write_pairs(entries: Sequence[Entry], out_path: Path, max_per_key: float) -> int:
    """Split every word into prefix→suffix pairs, largest weight per pair.

    `entries` arrives sorted by first character, so a word's prefixes all share
    a block and deduplication can happen one block at a time instead of holding
    six million pairs in memory. The dedup is required: one word with two
    readings produces identical pairs, and duplicates waste ranking positions.
    """
    total = 0
    with open(out_path, "w", encoding="utf-8") as out:
        block_first: str | None = None
        block: dict[tuple[str, str], float] = {}

        def flush() -> int:
            written = 0
            counts: dict[str, int] = defaultdict(int)
            for (prefix, rest), weight in block.items():
                counts[prefix] += 1
                if counts[prefix] > max_per_key:
                    continue
                out.write(f"{prefix}\t{rest}\t{weight}\n")
                written += 1
            block.clear()
            return written

        for e in entries:
            if e.word[:1] != block_first:
                total += flush()
                block_first = e.word[:1]
            for i in range(1, len(e.word)):
                key = (e.word[:i], e.word[i:])
                if e.weight > block.get(key, 0):
                    block[key] = e.weight
        total += flush()
    return total
