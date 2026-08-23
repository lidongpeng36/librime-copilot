"""Fixtures that hold the C++ scoring form and the Python one to one truth.

The two implementations do NOT have the same shape -- `scoring_form` folds one
string, `text_sentences` splits and discards -- so they cannot be diffed
directly. The agreement is pinned in two hops instead: `rime_train_test.py`
checks `scoring_form` against the real training chain, and this file's fixture
checks C++ `AlignToTrainingForm` against `scoring_form`.

THE INPUTS ARE HAND-WRITTEN AND MUST STAY THAT WAY. The obvious improvement --
sample the evaluation corpus so the fixture covers real text -- would commit the
author's private messages to a public remote. See CLAUDE.md, "Where the design
records live". Python owns the `out` column; a human owns the `in` column.
"""
from __future__ import annotations

import json
from typing import TextIO

from . import normalize

# One case per rule, plus the combinations that have actually gone wrong. Keep
# them short: a reviewer must be able to check an expected output by eye.
SCORING_FORM_CASES: tuple[str, ...] = (
    # rule 2 -- the sentence boundary, in all three positions
    "你好。我们走",
    "你好。",
    "好吗？好的！结束；对",
    # rule 3 -- no whitespace beside an EOS, which rules 1+2 alone get wrong
    "你好。 我们走",
    "  你好。\n我们走  ",
    # rule 1 -- whitespace folding, including the exotic blanks Python's \s
    # matches and powerlevel10k actually emits
    "你好\n我们走",
    "你好 我们走",
    "你好　我们走",
    # control characters normalize() drops outright: normalize() deletes every
    # category-C character before _WS.sub(" ") ever runs, `\n` alone excepted.
    # Tab, CR, VT, FF and NEL are all Cc -- they are DELETED, not folded to a
    # space, even though every regex's `\s` and every `str.isspace()` call
    # would call them whitespace. Contrast the U+3000 case above: it is
    # category Zs (not C), so it survives this filter and folds to a space;
    # these are removed entirely, so "你好\t\t我们走" becomes "你好我们走"
    # with no space at all -- an implementer who files that case under "rule 1
    # folding" gets it backwards.
    "你好\x07世界",
    "你好\t\t我们走",
    "你好\r我们走",
    "\u4f60\u597d\u200b\u4e16\u754c",  # a zero-width space, written as an escape on purpose
    # commas do NOT split
    "你好，我们走",
    "你好, 我们走",
    # full-width and ASCII both survive: NFKC is deliberately not applied
    "ＡＢ１２ 混排。半角也在。",
    # Latin as context, which is why the corpus keeps punctuation at all
    "运行 3 次都失败了。改一下 config.py 再试",
    # the deliberate divergence from text_sentences: a Han-free chunk is kept
    "ok then。好的。",
    # degenerate inputs
    "",
    "   ",
    "。",
    "。。。",
)


def emit_scoring_form(sink: TextIO) -> int:
    """Write one `{"in": ..., "out": ...}` line per case. Returns the count.

    `ensure_ascii=True` on purpose: the EOS carrier is U+0002, and a literal
    control byte in a committed fixture is invisible in every diff and every
    review. The escape `\\u0002` is not.
    """
    n = 0
    for case in SCORING_FORM_CASES:
        record = {"in": case, "out": normalize.scoring_form(case)}
        sink.write(json.dumps(record, ensure_ascii=True, sort_keys=True) + "\n")
        n += 1
    return n
