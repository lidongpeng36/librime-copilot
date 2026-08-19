"""Turning a downloaded source into the one-sentence-per-line corpus.

Everything here except `stream_source` is pure, so the stage order can be
tested without a download. The order itself matters and is not arbitrary:

  extract -> normalize -> split -> charset filter -> dedup

Splitting before filtering, because the filter's unit is a Han run and a
document is not one. Dedup last, because normalization is what makes two
copies of the same boilerplate compare equal in the first place.
"""
from __future__ import annotations

import gzip
import json
from pathlib import Path
from typing import Iterable, Iterator

from . import charset as _charset, dedup, normalize
from .sources import Source


def texts_from_line(line: str, source: Source) -> list[str]:
    """The raw text a source's JSON line carries, before any processing.

    Two shapes, both real and both verified against the files rather than
    their dataset cards: an LCCC line IS a JSON array of utterances (no
    wrapping object, so `field` is empty), a SkyPile line is an object whose
    "text" field is one document. A line that does not parse is
    skipped rather than raised on -- multi-gigabyte web dumps contain
    occasional broken lines, and refusing the whole corpus for one of them
    trades a complete run for nothing.
    """
    try:
        record = json.loads(line)
    except json.JSONDecodeError:
        return []
    value = record if not source.field else (
        record.get(source.field) if isinstance(record, dict) else None)
    if value is None:
        return []
    if source.is_dialogue:
        return [str(v) for v in value] if isinstance(value, list) else [str(value)]
    return [str(value)]


def sentences(texts: Iterable[str], typeable: frozenset[str],
              pre_tokenized: bool = False, han_only: bool = True) -> Iterator[str]:
    """Normalized text this schema could actually produce.

    `han_only` picks the consumer. The n-gram wants maximal Han runs, because
    that is all octagram ever looks up. A language model wants the punctuation,
    Latin and digits too, because that is what it will be asked to condition
    on -- 0% of the evaluation corpus's scoring contexts end in a Han
    character. Training on Han runs and scoring against punctuated context is
    not a simplification; it hands the model untrained input at exactly the
    position that decides the score. See normalize.text_sentences.
    """
    split = normalize.han_sentences if han_only else normalize.text_sentences
    for text in texts:
        text = normalize.normalize(text)
        if pre_tokenized:
            text = normalize.join_han_tokens(text)
        for sentence in split(text):
            if _charset.is_typeable(sentence, typeable):
                yield sentence


def stream_source(path: Path) -> Iterator[str]:
    """Lines of a downloaded source, transparently gunzipped.

    Decided by content rather than by filename: a `.gz` suffix is a claim, and
    a mislabelled file should fail on the first line rather than produce a
    corpus of binary garbage that the charset filter then silently discards.
    """
    with open(path, "rb") as probe:
        gzipped = probe.read(2) == b"\x1f\x8b"
    opener = gzip.open if gzipped else open
    with opener(path, "rt", encoding="utf-8", errors="replace") as handle:
        for line in handle:
            yield line


def build(path: Path, source: Source, typeable: frozenset[str],
          limit_lines: int | None = None, han_only: bool = True) -> Iterator[str]:
    """The whole pipeline for one source, streaming and deduplicated."""
    def raw() -> Iterator[str]:
        for index, line in enumerate(stream_source(path)):
            if limit_lines is not None and index >= limit_lines:
                return
            yield from sentences(texts_from_line(line, source), typeable,
                                 source.pre_tokenized, han_only)

    yield from dedup.unique(raw())
