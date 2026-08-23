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

# What carries an EOS inside a plain string, so the warm cache goes on comparing
# plain strings and no consumer needs a token type. U+0002 (STX) cannot occur in
# real text -- normalize() drops every control character before this is inserted.
EOS_CARRIER = "\x02"
_SENT_END_CHARS = "。！？!?；;"


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


def scoring_form(text: str) -> str:
    r"""The training stream's shape for ONE continuous string.

    `text_sentences` answers "what sentences does this document contribute to
    the corpus" -- it splits, strips and DISCARDS. Inference asks a different
    question of the same pipeline: "what would the stream look like at the point
    the user's caret is". Same normalization and the same boundaries, but NOT
    the same EOS placement -- see rule 2 and the divergence noted below --
    because at inference there is nothing to select: the text before the caret
    is what the user wrote.

    Three rules, each traceable to the pipeline that produced the deployed
    model:

      1. normalize() -- control characters out, `\s+` to one space, strip. A
         newline is `\s`, so it folds to a space exactly as in training. It is
         emphatically NOT an EOS: that would render the user's line break as the
         symbol the model learned to read as a full stop.
      2. an EOS after every sentence ender, mirroring _SENT_END. `train.py`
         (line 48) appends EOS after every non-empty corpus LINE,
         unconditionally -- it never inspects the line's last character.
         Because a corpus line is `text_sentences` output, the two rules
         coincide at every INTERNAL boundary. They diverge at the end of the
         input -- see below.
      3. no whitespace on either side of an EOS. normalize() ends in .strip()
         and text_sentences strips EVERY chunk, so the training stream contains
         no whitespace adjacent to token 2 anywhere in 4.5B tokens. Producing
         "你好。\x02 我们" would be a shape the model has never seen, and it is
         the rule an implementation gets wrong by default.

    Deliberately absent, and NOT an omission -- divergences from the training
    chain, each traded off on purpose:

      * text_sentences also drops chunks with no Han, and charset.is_typeable
        drops sentences carrying an out-of-table Han character. Both are
        corpus SELECTION. Copying them here would delete the user's own text
        from its own context. The train/inference gap that leaves is real,
        grows with context length, and is closed by a corpus change if the
        factorial says it is worth one -- not here.
      * `train.py`'s EOS is unconditional per line; a dialogue line with no
        terminal punctuation ("在吗", "好的") still gets one. `scoring_form`
        emits no EOS at the end of an unfinished context. This is the correct
        divergence, not a bug to fix: at the caret the sentence is unfinished,
        and an EOS would tell the model "this is finished" while we are asking
        it to predict the continuation.

    There is no unconditional trailing EOS: a context ending mid-sentence -- the
    common live case, since the caret is where the user stopped typing -- ends
    with no EOS at all, even though the training chain always closes a line
    with one.

    C++ `AlignToTrainingForm` (src/scoring_form.h) must agree with this function
    exactly; the golden fixture emitted by `rime-train scoring-form` is what
    holds the two together.
    """
    out: list[str] = []
    for chunk in _SENT_END.split(normalize(text)):
        chunk = chunk.strip()
        if not chunk:
            continue
        out.append(chunk)
        if chunk[-1] in _SENT_END_CHARS:
            out.append(EOS_CARRIER)
    return "".join(out)
