"""The check that keeps the training corpus out of the evaluation corpus.

The sources are public web text and the eval set is the user's private chat, so
overlap is unlikely. Unlikely is not evidence, and this project has already
been bitten once by the other direction of the same mistake: replaying the
corpus trained Rime's user dictionary on it and top-1 accuracy went 32.8% ->
99.2% between two passes (`rime_corpus/replay.py`). That one was caught by
measuring twice. This one has to be caught by construction, because a training
corpus that quietly contains the test set produces a number nobody can refute
from the outside.

What counts as overlap is chosen to be meaningful rather than merely detectable.
A shared 3-character collocation is not evidence of anything -- common phrases
are common. A shared *sentence*, or a shared span long enough that coincidence
is implausible, is.
"""
from __future__ import annotations

from typing import Iterable

# Long enough that a shared span is not ordinary Chinese. Chosen against the
# eval corpus's own shape: its Han runs average 6.0 characters, so a 10-run
# match is longer than a typical whole sentence in it.
MIN_SHARED_SPAN = 10


def fingerprints(sentences: Iterable[str], span: int = MIN_SHARED_SPAN) -> set[str]:
    """Every `span`-character window of every sentence long enough to have one.

    Sentences shorter than `span` are deliberately NOT fingerprinted, and this
    is the whole difference between a check and a noise generator. An earlier
    version fingerprinted them whole, on the reasoning that a chat corpus is
    full of short sentences and a windowed check would miss them. Run against
    17.5M LCCC sentences it reported 861 "overlaps", every one of them 嗯,
    好的, 是啊, 哈哈, 你好 -- not leakage, just Chinese.

    A short sentence cannot be evidence of leakage, because it cannot be
    distinguished from the language itself without frequency data this check
    does not have. Only a span long enough to be implausible by chance counts,
    and short eval sentences are simply outside what this can protect.
    """
    marks: set[str] = set()
    for sentence in sentences:
        for i in range(len(sentence) - span + 1):
            marks.add(sentence[i:i + span])
    return marks


def overlaps(training: Iterable[str], eval_marks: set[str],
             span: int = MIN_SHARED_SPAN) -> list[str]:
    """Training sentences that share a fingerprint with the eval corpus.

    Returned rather than filtered. A corpus with a handful of hits and a corpus
    with thousands mean completely different things -- the second means the
    source is not what it claims to be -- and silently trimming would erase
    that distinction along with the evidence for it.
    """
    hits = []
    for sentence in training:
        marks = fingerprints([sentence], span)
        if marks & eval_marks:
            hits.append(sentence)
    return hits
