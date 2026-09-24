"""The word-head table: how often each character begins a multi-character
word in the user's own corpus.

The LLM re-ranker scores P(next text is the candidate), and for a single
character that counts every word it begins -- 91% of P(安 | 可以) is 安装, 安排,
安全. So among single characters the productive word heads score high for a
reason unrelated to what the user is committing. The plugin adds
weight * log(1 - mass) to single-character candidates (src/wordhead_table.h);
this module writes the `mass` column.

Built from the user's corpus, not from jieba's dictionary frequencies: held
out, the corpus table is worth +145 net keystrokes against +137 for jieba's
and +120 for none (the design record, kept locally). Characters the corpus
never shows are left out, which means no penalty -- that conservative choice
beat a jieba fallback too.

test/data/wordhead_golden.txt pins this module's output format to the C++
reader; tools/test/wordhead_test.py regenerates it from a hand-written corpus.
"""
from __future__ import annotations

import re
from collections import Counter
from pathlib import Path
from typing import Callable, Iterable

from .personal import iter_corpus_texts

TABLE_NAME = "wordhead.txt"
HEADER = ("# rime-copilot wordhead: how often each character begins a multi-character word\n"
          "# in the user's own corpus. Generated -- `rime-copilot wordhead` rewrites it.\n")
# Add-two smoothing on each side: a character seen once standalone gets
# 2/5 = 0.4 rather than 0, which is the value the measurement used.
SMOOTHING = 2.0
_HAN = re.compile(r"[一-鿿]")


def count(texts: Iterable[str], segment: Callable[[str], "list[str]"]) -> "tuple[Counter, Counter]":
    """(head, solo): per character, tokens it begins vs tokens it is."""
    head: Counter = Counter()
    solo: Counter = Counter()
    for text in texts:
        for token in segment(text):
            if not token or not _HAN.match(token[0]):
                continue
            if len(token) == 1:
                solo[token] += 1
            else:
                head[token[0]] += 1
    return head, solo


def masses(head: "Counter | dict", solo: "Counter | dict") -> "dict[str, float]":
    return {c: (head.get(c, 0) + SMOOTHING) / (head.get(c, 0) + solo.get(c, 0) + 2 * SMOOTHING)
            for c in set(head) | set(solo)}


def render(mass: "dict[str, float]") -> str:
    """Deterministic bytes: same corpus, same file, so the vault never sees a
    meaningless conflict (the same reason personal.dict.yaml's version is a
    hash and not a date)."""
    return HEADER + "".join(f"{c}\t{m:.4f}\n" for c, m in sorted(mass.items()))


def _jieba_segment(text: str) -> "list[str]":
    import jieba  # lazy: RUNTIME_REQUIREMENTS, not a stock-interpreter module
    return list(jieba.cut(text, HMM=False))


# The C++ reader's rule (src/wordhead_table.h `Parse`): one Han character
# (rerank_detail::IsHanIdeograph's ranges), a tab, a number in [0, 1].
_ENTRY = re.compile(r"^[\u3400-\u4dbf\u4e00-\u9fff\uf900-\ufaff\U00020000-\U0002fa1f]\t(.+)$")


def count_entries(text: str) -> int:
    """How many lines of a table the plugin would actually load. Not the
    non-comment line count: a header-only or malformed table loads nothing,
    and the plugin then runs with the prior OFF."""
    seen = set()
    for line in text.splitlines():
        match = _ENTRY.match(line.rstrip("\r"))
        if not match or line[0] in seen:
            continue
        try:
            value = float(match.group(1))
        except ValueError:
            continue
        if 0.0 <= value <= 1.0:
            seen.add(line[0])
    return len(seen)


def build(corpus: Path,
          segment: "Callable[[str], list[str]] | None" = None) -> "dict[str, float]":
    """The table's masses, computed but not written."""
    head, solo = count(iter_corpus_texts(corpus), segment or _jieba_segment)
    return masses(head, solo)


def write(table: "dict[str, float]", output: Path) -> None:
    staged = output.with_name(output.name + ".new")
    staged.write_text(render(table), encoding="utf-8")
    # Never in place: the plugin may be reading the old file at construction.
    staged.replace(output)


def generate(*, corpus: Path, output: Path,
             segment: "Callable[[str], list[str]] | None" = None) -> int:
    """Write the table; return how many characters it holds."""
    table = build(corpus, segment)
    write(table, output)
    return len(table)
