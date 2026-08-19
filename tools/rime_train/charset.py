"""Which characters the decoder can actually produce, and the filter built on it.

The schema's dictionary imports `cn_dicts/8105` -- the 通用规范汉字表 -- as its
character table, so a character outside it cannot be typed under this schema at
all. A collocation containing one is therefore not merely rare: it is
unreachable, and counting it spends the n-gram's budget on entries the decoder
can never look up.

That gives a filter with a real criterion instead of a heuristic threshold, and
it disposes of traditional text as a side effect rather than as a special case:
traditional forms are outside the table, so a traditional sentence is dropped
by the same rule that drops a rare variant. No OpenCC dependency, and no
conversion artifacts in the counts.
"""
from __future__ import annotations

from pathlib import Path

from rime_corpus.corpus import HAN_CLASS
import re

_HAN = re.compile(f"[{HAN_CLASS}]")


def load_charset(dict_path: Path) -> frozenset[str]:
    """Every single character the given `.dict.yaml` lists.

    Parsed with the dictionary reader `rime_copilot` already owns, so the
    `---`/`...` header and the several column shapes are handled in one place
    rather than two.
    """
    from rime_copilot.dictfile import read_entries

    return frozenset(e.word for e in read_entries(dict_path) if len(e.word) == 1)


def is_typeable(sentence: str, charset: frozenset[str]) -> bool:
    """True when every Han character in `sentence` is in `charset`.

    All-or-nothing rather than a ratio: a ratio is unstable on the short runs
    that dominate chat text, where a single out-of-table character is a third
    of a three-character sentence, and it would admit collocations that are
    partly unreachable anyway.
    """
    return all(ch in charset for ch in sentence if _HAN.match(ch))
