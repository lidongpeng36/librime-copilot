"""Deciding what stays in the personal lexicon.

`custom.dict.yaml` was exported from Sogou, which learned sentence-level input,
so most of what it calls a word is a cross-word-boundary fragment: 的问题,
编译的, 这个我, 台机器. As the `top: true` source, every one of those outranks
every public dictionary entry — and lands in the top weight band of
private.predict.db, which is what gives `的` 1,326 continuations.

The chain below is ordered and first-match-wins. It is a pure function of one
entry plus an injected oracle, so the whole of it is testable without jieba.

R9 is load-bearing and easy to mistake for a tuning knob. No dictionary in this
tree can distinguish 好了 (a word) from 上的 (a fragment) — see the design doc
for the measurements. The user's own commit count is the only evidence that
can, so it has to be able to overrule a structural verdict.
"""
from __future__ import annotations

from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Sequence

from .dictfile import Entry, iter_body_lines

# Characters that mark a word boundary rather than carry content. A word
# beginning or ending with one is a fragment unless something else proves
# otherwise.
FUNCTION_WORDS = frozenset("的了着过吗呢吧啊呀哦嗯哈么呗嘛咯喔噢")
PRONOUNS = frozenset("我你他她它这那咱谁哪咋啥您")


@dataclass(frozen=True)
class Thresholds:
    high: int = 100      # R9: own commits that overrule a structural verdict
    low: int = 3         # below this, a pin carries no evidence
    compound: int = 50   # R4: what a multi-piece compound must reach


@dataclass(frozen=True)
class Verdict:
    action: str    # keep | review | drop
    rule: str
    reason: str
    suggest: str   # the default a reviewer sees; equals `action` off review rows


class Lexicon:
    """The oracles the chain consults, injected so tests need no jieba.

    `known` is a word -> corpus frequency map from a segmentation dictionary,
    which is free of cross-boundary fragments by construction. `chart` is the
    set of legal characters; None disables the check (tests that do not care).
    """

    def __init__(self, known: "dict[str, int]", chart: "set[str] | None",
                 segment: Callable[[str], "list[str]"],
                 tags: Callable[[str], "list[str]"]):
        self._known = known
        self._chart = chart
        self._segment = segment
        self._tags = tags

    def frequency(self, word: str) -> int:
        return self._known.get(word, 0)

    def in_chart(self, word: str) -> bool:
        if self._chart is None:
            return True
        return all(c in self._chart for c in word)

    def pieces(self, word: str) -> "list[str]":
        return self._segment(word)

    def is_name(self, word: str) -> bool:
        return any(tag.startswith("nr") for tag in self._tags(word))


def has_fragment_shape(word: str) -> bool:
    if not word:
        return False
    edges = (word[0], word[-1])
    return any(c in FUNCTION_WORDS or c in PRONOUNS for c in edges)


def classify(entry: Entry, lexicon: Lexicon,
             thresholds: Thresholds = Thresholds()) -> Verdict:
    word = entry.word
    weight = entry.weight

    if not lexicon.in_chart(word):
        return Verdict("drop", "R0", "character outside the 8105 chart", "drop")

    if lexicon.frequency(word) > 0:
        if weight >= thresholds.low:
            return Verdict("keep", "R1", "in the segmentation dictionary", "keep")
        return Verdict("drop", "R1",
                       f"in the segmentation dictionary but pinned on {weight:g} commits",
                       "drop")

    if weight >= thresholds.high:
        if has_fragment_shape(word):
            return Verdict("review", "R9-fragment",
                           f"{weight:g} commits, but the shape is a fragment", "drop")
        return Verdict("review", "R9", f"{weight:g} commits, no fragment shape", "keep")

    if has_fragment_shape(word):
        return Verdict("drop", "R2", "begins or ends with a function word or pronoun",
                       "drop")

    pieces = lexicon.pieces(word)
    if len(pieces) == 1:
        if weight < thresholds.low:
            return Verdict("drop", "R3-oov",
                           f"unknown word on only {weight:g} commits", "drop")
        if lexicon.is_name(word):
            return Verdict("review", "R3-name", "tagged as a name", "keep")
        return Verdict("review", "R3-oov", "unknown word, kept whole by the segmenter",
                       "keep")

    if weight >= thresholds.compound and all(len(p) >= 2 for p in pieces):
        return Verdict("review", "R4",
                       "compound of full words: " + "+".join(pieces), "keep")
    return Verdict("drop", "R4", "phrase, not a word: " + "+".join(pieces), "drop")


# The character chart doubles as the legal-character set: anything outside it
# reached the lexicon through a broken input method, not through typing.
CHART_NAME = "cn_dicts/8105.dict.yaml"


def read_chart(path: Path) -> "set[str]":
    chart = set()
    with open(path, "r", encoding="utf-8") as handle:
        for raw in iter_body_lines(handle, str(path)):
            word = raw.split("\t", 1)[0].strip()
            if len(word) == 1:
                chart.add(word)
    return chart


def load_lexicon(chart_path: "Path | None" = None) -> Lexicon:
    """The real oracle: jieba's dictionary plus the character chart.

    jieba is a *positive* oracle only. A word in its dictionary is certainly a
    word; a word absent from it may be a fragment (的问题) or a perfectly real
    term it has never seen (自动驾驶, 内存泄露). The chain must never read
    absence as evidence — see `classify`.
    """
    # lazy: importing jieba builds a 350k-entry trie, and every other command
    # in this CLI would pay for it.
    try:
        import jieba
        import jieba.posseg as posseg
    except ImportError as exc:
        raise ImportError(
            "jieba is not importable, but `clean` needs its segmentation "
            "dictionary to tell a word from a cross-word-boundary fragment. "
            "If jieba is installed somewhere, this is likely the wrong "
            "interpreter — a bare shebang can resolve differently depending "
            "on the current directory (e.g. via pyenv's per-directory "
            ".python-version). Install it with: pip install jieba"
        ) from exc

    jieba.setLogLevel(60)
    jieba.initialize()
    known = {word: freq for word, freq in jieba.dt.FREQ.items() if freq > 0}
    chart = read_chart(chart_path) if chart_path else None
    return Lexicon(known=known,
                   chart=chart,
                   segment=jieba.lcut,
                   tags=lambda w: [token.flag for token in posseg.cut(w)])


# The order a reviewer meets the groups in: the smallest and most consequential
# first, the long tail of unknown words last.
REVIEW_GROUPS = ("R9", "R9-fragment", "R3-name", "R4", "R3-oov")


@dataclass(frozen=True)
class Partition:
    keep: "list[Entry]"
    review: "list[tuple[Entry, Verdict]]"
    drop: "list[tuple[Entry, Verdict]]"


def partition(entries: Sequence[Entry], lexicon: Lexicon,
              thresholds: Thresholds = Thresholds()) -> Partition:
    keep: "list[Entry]" = []
    review: "list[tuple[Entry, Verdict]]" = []
    drop: "list[tuple[Entry, Verdict]]" = []
    for item in entries:
        verdict = classify(item, lexicon, thresholds)
        if verdict.action == "keep":
            keep.append(item)
        elif verdict.action == "review":
            review.append((item, verdict))
        else:
            drop.append((item, verdict))
    return Partition(keep=keep, review=review, drop=drop)


def _row(item: Entry, verdict: Verdict) -> str:
    return f"{verdict.suggest}\t{item.weight:g}\t{item.word}\t{verdict.rule}\t{verdict.reason}"


def render_review(part: Partition, thresholds: Thresholds = Thresholds()) -> str:
    """The file a human annotates.

    Rows carry a default action; within a group they are ordered by weight
    descending (the `keep`-first term in the sort key is a no-op in practice
    — every rule that reaches review suggests the same action for every row
    it produces, so there is never a `keep`/`drop` mix within one group to
    sort). R9-fragment forms its own group, directly after R9, and every row
    in it is already marked `drop` — so the reviewer meets the rows that need
    thought first, and the mechanical 118-of-197 fragment rows never get in
    the way. That is what keeps `high = 100` cheap to review.

    The leading `# thresholds:` line records the thresholds this file was
    generated with, so `--apply` can refuse to apply it under a different
    set (see `parse_review_thresholds`). It is a comment, so `parse_review`
    -- which already skips `#` lines -- reads an old review.tsv without one
    exactly as before.
    """
    lines = [
        f"# thresholds: high={thresholds.high} low={thresholds.low} "
        f"compound={thresholds.compound}",
        "# Edit the first column only: keep or drop. Everything else is context.",
        "# Rows already say what the rule chain would do; change the ones it got wrong.",
        "",
    ]
    by_rule: "dict[str, list[tuple[Entry, Verdict]]]" = {}
    for item, verdict in part.review:
        by_rule.setdefault(verdict.rule, []).append((item, verdict))
    for rule in REVIEW_GROUPS:
        rows = by_rule.pop(rule, [])
        if not rows:
            continue
        rows.sort(key=lambda pair: (pair[1].suggest != "keep", -pair[0].weight))
        lines.append(f"# {rule}  ({len(rows)} rows)")
        lines.extend(_row(item, verdict) for item, verdict in rows)
        lines.append("")
    # A rule that reaches review without a place in REVIEW_GROUPS is a bug, but
    # losing its rows silently would be a worse one.
    for rule, rows in sorted(by_rule.items()):
        lines.append(f"# {rule}  ({len(rows)} rows, ungrouped)")
        lines.extend(_row(item, verdict) for item, verdict in rows)
        lines.append("")
    return "\n".join(lines) + "\n"


def render_drop(part: Partition) -> str:
    """Every deletion with the rule that made it. Audit only; nothing reads it back."""
    rows = sorted(part.drop, key=lambda pair: (pair[1].rule, -pair[0].weight))
    lines = ["# weight\tword\trule\treason", ""]
    lines.extend(f"{item.weight:g}\t{item.word}\t{verdict.rule}\t{verdict.reason}"
                 for item, verdict in rows)
    return "\n".join(lines) + "\n"


def parse_review_thresholds(text: str) -> "Thresholds | None":
    """The `# thresholds:` header `render_review` writes, or None if absent.

    Absence is not an error: a review.tsv predating this header, or one
    hand-crafted without it, must still be readable -- `--apply` treats None
    as "nothing to compare against" rather than refusing outright.
    """
    for raw in text.splitlines():
        line = raw.strip()
        if not line.startswith("# thresholds:"):
            continue
        fields: "dict[str, int]" = {}
        for token in line[len("# thresholds:"):].split():
            key, _, value = token.partition("=")
            try:
                fields[key] = int(value)
            except ValueError:
                continue
        return Thresholds(high=fields.get("high", Thresholds.high),
                          low=fields.get("low", Thresholds.low),
                          compound=fields.get("compound", Thresholds.compound))
    return None


def parse_review(text: str) -> "dict[str, str]":
    """Read an annotated review file.

    Refuses a malformed row rather than skipping it. A typo that silently
    dropped a decision would delete a word the reviewer meant to save, and
    nothing downstream would show it. Two rows disagreeing on one word reach
    that same failure by another path — a copy-pasted row during hand-editing
    is realistic — so it is refused too, rather than resolved by "last write
    wins". A row repeated with the *same* action is a harmless duplicate, not
    a lost decision, and stays legal.

    Entries here are identified by word alone, not the package's other
    identity of `(word, pinyin)` that `dictdb.merge` uses (see dictdb.py). Two
    entries sharing a word but differing in pinyin — a heteronym pair — would
    take a single decision for both; the conflict check above only catches it
    when the two rows disagree, turning that case into an error instead of a
    silent wrong answer, not into a correct per-pinyin decision.
    """
    decisions: "dict[str, str]" = {}
    first_seen: "dict[str, int]" = {}
    for number, raw in enumerate(text.splitlines(), start=1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        parts = raw.split("\t")
        if len(parts) < 3:
            raise ValueError(f"review line {number}: expected at least 3 tab-separated "
                             f"columns, got {len(parts)}: {raw!r}")
        action, _weight, word = parts[0].strip(), parts[1], parts[2].strip()
        if action not in ("keep", "drop"):
            raise ValueError(f"review line {number}: action must be keep or drop, "
                             f"got {action!r}")
        if word in decisions and decisions[word] != action:
            raise ValueError(
                f"review line {number}: {word!r} was already decided {decisions[word]!r} "
                f"on line {first_seen[word]}, now {action!r} — pick one")
        decisions[word] = action
        first_seen.setdefault(word, number)
    return decisions


def apply_review(part: Partition, decisions: "dict[str, str]") -> "list[Entry]":
    """The surviving lexicon.

    A review row the file no longer mentions falls back to the default the
    chain suggested, so a truncated or partially edited review file loses
    nothing silently.
    """
    survivors = list(part.keep)
    for item, verdict in part.review:
        if decisions.get(item.word, verdict.suggest) == "keep":
            survivors.append(item)
    # Same order dictdb.merge emits, so a diff against the previous dictionary
    # shows content changes rather than a reshuffle.
    survivors.sort(key=lambda e: (e.word[0], -e.weight))
    return survivors
