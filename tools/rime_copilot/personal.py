"""Generating the derived personal dictionary.

`custom.dict.yaml` carries the user's own commit counts but is imported LAST in
`private.dict.yaml`, where librime keeps the larger weight for any (text, code)
a public table also has (entry_collector.cc de-duplicates only single-syllable
entries; vocabulary.cc:61 sorts homophones by weight descending). Its median
weight is 7 against a flat-100 floor shared by 1.47M ext/tencent/sogou entries,
so 92% of it is invisible and the rest ranks below the floor. This module
produces the file that fixes that: the same vocabulary on a scale that can be
seen, plus the words the corpus has and no dictionary does.

Nothing here imports `rime_corpus`. `install.payload_files` copies only this
package, so an installed CLI that reached for it would die with ImportError on
exactly the machines with no checkout. A corpus record is six JSON keys (`v`,
`id`, `src`, `ts`, `text`, `redacted` -- see `rime_corpus.corpus.make_record`);
the reader below only ever looks at `text`, which is the whole cost of keeping
that dependency one-way.
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
from typing import TYPE_CHECKING, Callable, Iterable, Iterator, Sequence

from .dictfile import Entry
from .paths import sha256_file

if TYPE_CHECKING:  # pragma: no cover
    from .clean import Lexicon

DEFAULT_CORPUS_DIR = Path.home() / ".local" / "share" / "rime-corpus"

# A word must occur at least twice before the corpus is evidence rather than a
# typo, and must be 2-8 characters: one character is already in the 字表 with a
# real frequency, and nine or more is a sentence the segmenter failed on.
MIN_CORPUS_COUNT = 2
MIN_WORD_CHARS = 2
MAX_WORD_CHARS = 8


def corpus_dir(explicit: "str | None" = None) -> Path:
    if explicit:
        return Path(explicit).expanduser()
    from_env = os.environ.get("RIME_CORPUS_DIR")
    if from_env:
        return Path(from_env).expanduser()
    return DEFAULT_CORPUS_DIR


def iter_corpus_texts(directory: Path) -> Iterator[str]:
    """The `text` field of every record in every `*.jsonl` under `directory`.

    A missing directory yields nothing -- a machine with no corpus still gets a
    dictionary, built from `custom.dict.yaml` alone. A malformed LINE, on the
    other hand, is refused: `ingest` appends, so a half-written record is a
    real state, and silently mining a smaller vocabulary while reporting
    success is the failure this repository keeps meeting in other forms.
    """
    if not directory.is_dir():
        return
    for path in sorted(directory.glob("*.jsonl")):
        with open(path, "r", encoding="utf-8") as handle:
            for number, raw in enumerate(handle, start=1):
                line = raw.strip()
                if not line:
                    continue
                try:
                    record = json.loads(line)
                except ValueError as exc:
                    raise ValueError(f"{path}: line {number} is not valid JSON: {exc}") from exc
                text = record.get("text")
                if text:
                    yield text


def compute_version(custom_path: Path, corpus: Path) -> str:
    """A short, content-derived version string for the generated dictionary.

    Not the date. `update` regenerates this file on every run, and the
    generator is deterministic: two runs against the same inputs produce a
    byte-identical vocabulary. A version stamped from `date.today()` was
    therefore the ONLY source of churn -- the first `update` of every new
    calendar day made the local file differ from the vaulted one by that one
    line, and `status` reported a `conflict` for a file whose words never
    changed. Every subsequent day repeated it, forever.

    Hashing the actual inputs instead answers a real question -- which
    custom.dict.yaml and which corpus this file was generated from -- and
    only changes when one of them does. `custom_path` not existing, or
    `corpus` not existing or being empty, are legitimate inputs in their own
    right (see `generate`'s docstring) and are folded in rather than treated
    as errors.
    """
    digest = hashlib.sha256()
    if custom_path.is_file():
        digest.update(b"custom:")
        digest.update(sha256_file(custom_path).encode("ascii"))
    if corpus.is_dir():
        for path in sorted(corpus.glob("*.jsonl")):
            digest.update(b"|corpus:")
            digest.update(path.name.encode("utf-8"))
            digest.update(b":")
            digest.update(sha256_file(path).encode("ascii"))
    return digest.hexdigest()[:12]


def is_han_word(word: str, min_chars: int, max_chars: int) -> bool:
    """Entirely Han, and of a length worth a dictionary entry.

    Rejects rather than trims: a segmenter piece carrying punctuation
    (`你好，`) is a piece the segmenter got wrong, and trimming it would
    invent a word nobody wrote.
    """
    if not (min_chars <= len(word) <= max_chars):
        return False
    # U+4E00-U+9FFF only -- narrower than rime_corpus.corpus.HAN_CLASS (which
    # also covers Ext-A, Compatibility and Ext-B, and mirrors
    # rerank_detail::IsHanIdeograph in src/rerank.h). Deliberate, not a
    # divergence to fix: this module cannot import rime_corpus (see the
    # module docstring), and the 8105 chart every mined word must also clear
    # (`lexicon.in_chart`) is basic-block-only anyway, so the narrower range
    # costs nothing in practice.
    return all("一" <= char <= "鿿" for char in word)


def mine(texts: Iterable[str], segment: Callable[[str], "list[str]"], *,
         min_count: int = MIN_CORPUS_COUNT,
         min_chars: int = MIN_WORD_CHARS,
         max_chars: int = MAX_WORD_CHARS) -> "dict[str, int]":
    """Word -> occurrences, over everything the user has written.

    `segment` is injected so this is testable without jieba's 350k-entry trie,
    and so the oracle can be swapped without touching the counting.
    """
    counts: "dict[str, int]" = {}
    for text in texts:
        for piece in segment(text):
            if is_han_word(piece, min_chars, max_chars):
                counts[piece] = counts.get(piece, 0) + 1
    return {word: n for word, n in counts.items() if n >= min_count}


# The band the generated weights occupy. The floor clears the flat 100 shared
# by ext + tencent + sogou (1.47M entries) so no personal word is invisible;
# the ceiling sits below base's own top band (max 1.9e7, p95 36,705) so a word
# the language genuinely prefers still wins. Measured on the corpus: the whole
# band is worth +0.9 top1 held out, and flattening every entry to one value
# inside it gives up half of that -- the ordering within the band is doing real
# work, not decoration.
WEIGHT_FLOOR = 2000
WEIGHT_CEILING = 200000


def normalise(counts: "dict[str, int]") -> "dict[str, float]":
    """Counts to a 0..1 prominence, logarithmically.

    Log rather than linear because word frequency is long-tailed: a linear
    rescale puts almost everything on the floor (`dictdb.scale_weights` has the
    measurement). Normalising per source is also what lets two sources on
    completely different scales -- Sogou's lifetime counts against the corpus's
    5,428 utterances -- be compared at all.
    """
    if not counts:
        return {}
    largest = max(counts.values())
    denominator = math.log(1 + max(largest, 1))
    if denominator <= 0:
        return {word: 1.0 for word in counts}
    return {word: math.log(1 + max(count, 0)) / denominator for word, count in counts.items()}


def to_weight(fraction: float, floor: int = WEIGHT_FLOOR,
              ceiling: int = WEIGHT_CEILING) -> int:
    clamped = min(1.0, max(0.0, fraction))
    return int(round(floor + clamped * (ceiling - floor)))


def build_entries(custom: "Sequence[Entry]", mined: "dict[str, int]",
                  lexicon: "Lexicon", reading: Callable[[str], str]) -> "list[Entry]":
    """The generated dictionary's entries.

    `lexicon` is `clean.Lexicon` and is consulted for exactly two things: the
    8105 chart, and whether jieba knows a word. `reading` produces a pinyin for
    a mined word; it is injected so this is testable without pypinyin.

    The two sources are normalised SEPARATELY. Sogou's counts accumulated over
    years of typing and the corpus's over 5,428 harvested utterances; adding
    them would let whichever happens to be larger decide the whole ordering.
    Each source instead says "how prominent is this word, for me, relative to
    my own most prominent word", and the larger of the two claims wins.

    Two different identities are in play here, deliberately. `custom_fraction`
    is keyed by (word, pinyin) -- the same identity `dictdb.merge` uses --
    because a heteronym pair (重 zhong, 重 chong) is two distinct entries with
    their own commit counts, not one word wearing two readings: collapsing
    them to a word-level count would let whichever reading was typed more
    often lend ITS weight to the one typed less, and librime sorts a code's
    homophones by weight descending (vocabulary.cc:61), so the rarer reading
    would jump the queue within its own code, ahead of entries the user
    actually prefers there. `known_words`, below -- the set that suppresses a
    duplicate mined entry -- is word-level on purpose: a mined count has no
    reading to key on, so joining it to a custom word is the one place this
    module aggregates by word alone.
    """
    from .clean import has_fragment_shape

    custom_fraction = normalise({(entry.word, entry.pinyin): int(entry.weight)
                                 for entry in custom})
    mined_fraction = normalise(mined)

    known_words = {entry.word for entry in custom}
    results: "list[Entry]" = []

    for entry in custom:
        fraction = max(custom_fraction[(entry.word, entry.pinyin)],
                       mined_fraction.get(entry.word, 0.0))
        results.append(Entry(entry.word, entry.pinyin, to_weight(fraction)))

    for word in sorted(mined):
        if word in known_words:
            continue  # its count already raised the custom entry above
        if not lexicon.in_chart(word):
            continue
        # jieba is a positive oracle: absence proves nothing, presence proves a
        # word. Only judge the SHAPE of something jieba has never seen --
        # otherwise 好的, 是的, 那个 are deleted, which is exactly what two
        # earlier drafts of clean.py's chain did.
        if lexicon.frequency(word) == 0 and has_fragment_shape(word):
            continue
        results.append(Entry(word, reading(word), to_weight(mined_fraction[word])))

    # Same order dictdb.merge and clean.apply_review emit, so a diff against
    # the previous generation shows content rather than a reshuffle.
    results.sort(key=lambda e: (e.word[0], -e.weight))
    return results


DICT_NAME = "personal"

_HEADER_COMMENT = (
    "# Rime dictionary",
    "# encoding: utf-8",
    "#",
    "# GENERATED by `rime-copilot personal`. Do not edit: the next `update`",
    "# overwrites it. Edit the sources instead --",
    "# private/custom.dict.yaml (via `rime-copilot clean`) or the corpus",
    "# (via `rime-corpus ingest`).",
    "#",
    "# Why this file exists: custom.dict.yaml's own weights are 3..2877 with a",
    "# median of 7, against a flat 100 shared by 1.47M ext/tencent/sogou",
    "# entries, so the user's own frequency was invisible to the ordering.",
    "# See docs/superpowers/specs/2026-08-22-lexicon-optimization-design.md.",
)


def generate(*, custom_path: Path, corpus: Path, chart_path: "Path | None",
             output: Path, version: str,
             segment: "Callable[[str], list[str]] | None" = None,
             reading: "Callable[[str], str] | None" = None,
             known: "dict[str, int] | None" = None) -> int:
    """Write the derived dictionary; return how many entries it holds.

    The three oracle parameters exist for tests. Left as None, the real ones
    are loaded -- jieba for segmentation and its dictionary, pypinyin for
    readings -- which is why neither is imported at module level. `segment`
    and `known` are one injected oracle and must be passed together.
    """
    from .clean import Lexicon, read_chart
    from .dictfile import read_entries, write_dict

    if (segment is None) != (known is None):
        raise ValueError(
            "segment and known are one injected oracle, not two: pass both to "
            "drive this without jieba, or neither to load the real one. "
            "Passing one silently discarded it and loaded jieba anyway.")

    custom = read_entries(custom_path) if custom_path.is_file() else []

    if segment is None:
        from .clean import load_lexicon
        oracle = load_lexicon(chart_path)
        segment = oracle.pieces
        lexicon = oracle
    else:
        chart = read_chart(chart_path) if chart_path else None
        lexicon = Lexicon(known=known, chart=chart, segment=segment,
                          tags=lambda word: [])

    if reading is None:
        from .dictfile import auto_pinyin
        reading = auto_pinyin

    mined = mine(iter_corpus_texts(corpus), segment)
    entries = build_entries(custom, mined, lexicon, reading)
    staged = output.with_name(output.name + ".new")
    count = write_dict(staged, name=DICT_NAME, version=version, entries=entries,
                       comment_lines=_HEADER_COMMENT)
    # Never in place: librime may be mid-deploy against the old file, and a
    # half-written dictionary fails the whole build (dict_compiler.cc:54).
    staged.replace(output)
    return count
