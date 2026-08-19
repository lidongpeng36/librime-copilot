"""Text normalization and sentence splitting. Pure; no I/O, no network.

The n-gram this feeds counts Han collocations of 3 to 4 characters
(`collocation_min_length`/`max_length`, octagram.cc). Two consequences shape
everything here:

  * A window that crosses a sentence boundary is a collocation nobody types.
    Splitting is therefore load-bearing, not tidying -- and the boundary is
    not only punctuation: any non-Han character ends a Han collocation just as
    a full stop does, because the lookup key is a run of Han characters.
  * Latin, digits, code identifiers and file paths cannot appear in a key at
    all. They are not noise to be scrubbed; they are simply invisible, and
    removing them early is what makes the counting cheap.
"""
from __future__ import annotations

import re
import unicodedata

# Reuse the evaluation harness's single definition of "Han" rather than
# writing a second one that must agree with it.
from rime_corpus.corpus import HAN_CLASS

_HAN_RUN = re.compile(f"[{HAN_CLASS}]+")
_WS = re.compile(r"\s+")
_HAN_GAP = re.compile(f"(?<=[{HAN_CLASS}])[ \t]+(?=[{HAN_CLASS}])")


def normalize(text: str) -> str:
    """Full-width to half-width, collapsed whitespace, control chars dropped.

    NFKC also folds full-width Latin and digits onto ASCII, which is what the
    user's own keyboard produces; leaving both forms in would split counts for
    what is the same text.
    """
    text = unicodedata.normalize("NFKC", text)
    text = "".join(ch for ch in text if ch == "\n" or unicodedata.category(ch)[0] != "C")
    return _WS.sub(" ", text).strip()


def join_han_tokens(text: str) -> str:
    """Undo word-separating spaces between Han characters.

    Some corpora ship pre-tokenized -- LCCC stores "你 去 那儿 竟然 不喊 我"
    rather than the sentence a person typed. Left alone this is not a cosmetic
    difference: `han_sentences` treats any non-Han character as a boundary, so
    every utterance would shatter into one-word fragments and NO 3-or-4
    character collocation would survive a word boundary. The corpus would look
    fine, build without error, and teach the grammar nothing -- and the n-gram
    experiment would then read as "domain does not matter".

    Only spaces with Han on both sides are removed, so "SEED 早上" keeps its
    space and Latin text is untouched.
    """
    return _HAN_GAP.sub("", text)


def han_sentences(text: str) -> list[str]:
    """Maximal runs of Han characters, in order.

    The same segmentation the evaluation harness applies to the user's writing
    (`replay.han_runs`), so a collocation counted here is one that could
    actually be produced by a run the harness measures.
    """
    return _HAN_RUN.findall(text)
