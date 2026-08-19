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
# A space run with no Latin alphanumeric on either side is a tokenizer's
# artifact, not writing: "你 去 那儿" and "了 , 快点" both come from LCCC's
# pre-segmentation, while "SEED 早上" and "运行 3 次" are real spacing and keep
# theirs.
_TOKENIZER_GAP = re.compile(r"(?<![A-Za-z0-9])[ \t]+(?![A-Za-z0-9])")
# Sentence enders only. Commas, Latin, digits and code identifiers deliberately
# do NOT split -- see text_sentences.
_SENT_END = re.compile(r"(?<=[。！？!?；;])")
_HAS_HAN = re.compile(f"[{HAN_CLASS}]")


def normalize(text: str) -> str:
    """Collapsed whitespace and control characters dropped. Nothing else.

    Deliberately NOT NFKC. Folding full-width onto half-width looked like
    consistency and is a training/inference mismatch: the evaluation contexts
    contain BOTH forms -- 4062 ASCII commas and 1827 full-width ones -- and
    nothing normalizes them at inference, because `score_candidates` feeds the
    context exactly as harvested. NFKC in training would mean the model never
    sees U+FF0C while being asked to condition on it 1827 times.

    Newlines survive as themselves: the contexts are full of them (3880
    occurrences), being markdown and chat.
    """
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

    Only spaces with no Latin alphanumeric on either side are removed, so
    "SEED 早上" and "运行 3 次" keep theirs while "你 去 那儿" and "了 , 快点"
    lose the separators LCCC inserted. Punctuation counts as a non-Latin
    neighbour on purpose: a model trained on "道歉 !" would learn a space
    before every exclamation mark that no one types.
    """
    return _TOKENIZER_GAP.sub("", text)


def han_sentences(text: str) -> list[str]:
    """Maximal runs of Han characters, in order.

    The same segmentation the evaluation harness applies to the user's writing
    (`replay.han_runs`), so a collocation counted here is one that could
    actually be produced by a run the harness measures.

    Correct for the n-gram, which only ever looks up Han collocations. WRONG
    for a language model -- see `text_sentences`.
    """
    return _HAN_RUN.findall(text)


def text_sentences(text: str) -> list[str]:
    """Sentences with their punctuation, Latin and digits intact.

    What `han_sentences` throws away is exactly what a language model is asked
    to condition on. Measured on the evaluation corpus: **0%** of scoring
    contexts end in a Han character and 77.3% end in something else -- 好的,
    前的项目. 长时间了, -- because the harness splits requests at maximal Han
    runs, so the character before every run is by construction not Han.

    A model trained only on Han runs has never seen those characters. Their
    embeddings sit at initialization, and `score_candidates` conditions the
    FIRST candidate token -- the most discriminative one -- on the hidden state
    that ends with them. Training on Han-only text is therefore not a
    simplification; it removes the model's input at the position that decides
    the score.

    Splits on sentence enders only. Commas do not end a sentence, and Latin
    and digits are kept: the user writes Chinese around code identifiers and
    file paths, and that is the context the model will actually be given.
    """
    out = []
    for chunk in _SENT_END.split(text):
        chunk = chunk.strip()
        # Pure-Latin chunks would spend capacity teaching this model English,
        # which is not its job; it needs Latin as CONTEXT, not as output.
        if chunk and _HAS_HAN.search(chunk):
            out.append(chunk)
    return out
