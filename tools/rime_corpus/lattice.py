"""Can our lexicon even produce what the user wrote?

S0-a of the neural-decoding project. A lattice decoder can only ever output a
path through the lexicon, so its ceiling is fixed before any model exists. The
question is not "would a better model rank gold higher" -- it is "is gold in
the search space at all, and how many pieces does it take to spell out".

Reachability alone is close to vacuous, and was expected to be. The schema's
dictionary imports `cn_dicts/8105`: 8782 single-character entries covering the
whole 通用规范汉字表. Any Han text is therefore reachable as a chain of single
characters, and a "coverage" number that only asks reachability would report
~99% and mean nothing.

What carries information is the SHAPE of the cheapest path. A run the lexicon
knows as two words is a different problem for a decoder than the same run it
can only spell one character at a time: in the first case word weights carry
most of the answer, in the second they carry none and every character is an
independent homophone choice. So this module reports, per unit of text:

  reachable      is gold a path at all
  min_words      fewest dictionary entries whose texts and readings
                 concatenate to exactly gold
  singles        how many of those are single characters -- the fallback

Ties are broken toward fewer single-character entries, so `singles` is the
best case among the shortest paths rather than an arbitrary one.

When gold is NOT reachable the attribution is exhaustive, and cheaply so:
given a single-character entry for every character, the all-singles path
always exists, so gold is unreachable IF AND ONLY IF some character has no
single-character entry carrying the reading we assigned it. Two causes, which
mean entirely different things:

  missing_char     the character is not in the dictionary at all
  missing_reading  it is, but not with this reading

`missing_reading` is a measurement artifact at least as often as a lexicon
gap. Readings come from `speller.syllables`, i.e. pypinyin, which assigns them
without knowing what the sentence means; a 多音字 it guesses wrong is
indistinguishable here from one the lexicon really lacks. `Gap` therefore
carries the readings the lexicon DOES have for that character, so the two can
be told apart by eye rather than by assumption.

Readings are matched at the 全拼 syllable level, not at the typed-key level.
The schema's algebra maps syllables onto keys and can merge distinct syllables
onto one key, which changes how AMBIGUOUS the lattice is but never changes
what is reachable. Reachability is all this module claims, so `Speller` is not
needed here.

One known bias, in a known direction: `cn_dicts/tencent` declares
`columns: [text, weight]` and carries no readings at all. librime derives them
by re-encoding each phrase against the tables loaded before it, which can
yield SEVERAL readings per phrase; `dictfile.read_entries` falls back to
pypinyin, which yields exactly one. This module therefore sees a strictly
poorer tencent than Rime does and can only UNDERSTATE coverage. Report the
tencent-free figure alongside to show how much rests on that source.
"""
from __future__ import annotations

import re
from dataclasses import dataclass
from pathlib import Path
from typing import Callable, Iterable, Sequence

# Longest entry considered when spanning gold. The lexicon holds longer
# phrases, but every additional length multiplies the DP's inner loop while
# the phrases that actually match a user's sentence are short; 12 characters
# is past the point where matches stop appearing in this corpus.
MAX_WORD = 12

MISSING_CHAR = "missing_char"
MISSING_READING = "missing_reading"
NO_SYLLABLES = "no_syllables"


@dataclass(frozen=True)
class Gap:
    """One character that gold cannot be spelled with."""

    index: int
    char: str
    want: str  # the reading speller.syllables assigned
    have: tuple[str, ...]  # readings the lexicon has for this character
    cause: str


@dataclass(frozen=True)
class Coverage:
    reachable: bool
    min_words: int | None  # None exactly when not reachable
    singles: int | None
    gaps: tuple[Gap, ...]


class Lexicon:
    """Dictionary entries indexed for "does this exact span exist" lookups.

    Only two questions are ever asked of it, so only two things are stored:
    the set of (text, readings) pairs, and the readings available for each
    single character. Storing the entries themselves would multiply memory by
    the number of homophones without answering anything extra -- weights play
    no part in reachability.
    """

    def __init__(self) -> None:
        self._spans: set[tuple[str, tuple[str, ...]]] = set()
        self._char_readings: dict[str, set[str]] = {}
        self.misaligned = 0

    def add(self, text: str, syllables: Sequence[str]) -> None:
        syls = tuple(syllables)
        if not text or len(syls) != len(text):
            # One reading per character is what makes an entry alignable to a
            # span of gold. Entries that break it (a stray Latin word, a
            # malformed line) are counted rather than dropped silently --
            # a source that is mostly misaligned is a parsing bug, and a
            # silent `continue` is how it would stay invisible.
            self.misaligned += 1
            return
        self._spans.add((text, syls))
        if len(text) == 1:
            self._char_readings.setdefault(text, set()).add(syls[0])

    def has(self, text: str, syllables: tuple[str, ...]) -> bool:
        return (text, syllables) in self._spans

    def readings_of(self, char: str) -> tuple[str, ...]:
        return tuple(sorted(self._char_readings.get(char, ())))

    def __len__(self) -> int:
        return len(self._spans)


def _gaps(text: str, syllables: Sequence[str], lex: Lexicon) -> list[Gap]:
    gaps = []
    for i, (char, want) in enumerate(zip(text, syllables)):
        if lex.has(char, (want,)):
            continue
        have = lex.readings_of(char)
        gaps.append(
            Gap(
                index=i,
                char=char,
                want=want,
                have=have,
                cause=MISSING_READING if have else MISSING_CHAR,
            )
        )
    return gaps


def analyze(
    text: str, syllables: Sequence[str], lex: Lexicon, max_word: int = MAX_WORD
) -> Coverage:
    """Cheapest lexicon path spelling `text` with exactly `syllables`."""
    n = len(text)
    if len(syllables) != n:
        # `speller.syllables` drops what pypinyin cannot read (errors="ignore"),
        # so a length mismatch means the unit is not the pure Han run this
        # analysis assumes. Reporting it as its own cause keeps it out of the
        # lexicon's column.
        return Coverage(False, None, None, (Gap(-1, text, "", (), NO_SYLLABLES),))

    unreachable = (1 << 30, 0)
    # cost[j] = (words, singles) of the cheapest path spelling text[:j]
    cost: list[tuple[int, int]] = [unreachable] * (n + 1)
    cost[0] = (0, 0)
    for j in range(1, n + 1):
        best = unreachable
        for i in range(max(0, j - max_word), j):
            if cost[i] == unreachable:
                continue
            if not lex.has(text[i:j], tuple(syllables[i:j])):
                continue
            words, singles = cost[i]
            candidate = (words + 1, singles + (1 if j - i == 1 else 0))
            if candidate < best:
                best = candidate
        cost[j] = best

    if cost[n] == unreachable:
        return Coverage(False, None, None, tuple(_gaps(text, syllables, lex)))
    return Coverage(True, cost[n][0], cost[n][1], ())


_LIST_ITEM = re.compile(r"^\s+-\s+(\S+)")

DICT_SUFFIX = ".dict.yaml"


def table_name(path: Path) -> str:
    """`cn_dicts/tencent.dict.yaml` -> `tencent`.

    `Path.stem` strips one suffix and yields `tencent.dict`, which silently
    matches nothing when a caller selects tables by name.
    """
    return path.name[: -len(DICT_SUFFIX)] if path.name.endswith(DICT_SUFFIX) else path.stem


def import_tables(dict_path: Path) -> list[Path]:
    """The `.dict.yaml` files a Rime dictionary imports, in declared order.

    Parsed by line rather than with a YAML library: this package has no YAML
    dependency and adding one for a six-line list would be the tail wagging
    the dog. The parse is deliberately narrow -- an `import_tables:` key,
    followed by `  - name` items until the block ends -- and a dictionary that
    declares none is an error rather than an empty list, because "no imports"
    and "the parse did not recognise the block" are indistinguishable
    downstream and only one of them is survivable.
    """
    names: list[str] = []
    in_block = False
    for raw in dict_path.read_text(encoding="utf-8").splitlines():
        stripped = raw.strip()
        if stripped == "...":
            break
        if not in_block:
            if stripped.startswith("import_tables:"):
                in_block = True
            continue
        if not stripped or stripped.startswith("#"):
            # Commented-out imports (`# - cn_dicts/41448`) are how this file
            # records tables the user chose NOT to load. Reading them would
            # measure a lexicon nobody types with.
            continue
        item = _LIST_ITEM.match(raw)
        if not item:
            break  # dedent, or another key: the block is over
        names.append(item.group(1))

    if not names:
        raise ValueError(f"{dict_path}: no import_tables block found")
    return [dict_path.parent / f"{name}.dict.yaml" for name in names]


def load(paths: Iterable[Path], keep: Callable[[str], bool] | None = None) -> tuple[
    "Lexicon", dict[str, int]
]:
    """Build a Lexicon from `.dict.yaml` files, and report each one's share.

    The per-source counts are not decoration. Coverage that rests mostly on
    one dictionary is a different result from coverage spread across all of
    them -- particularly for `cn_dicts/tencent`, whose readings this harness
    approximates (see the module docstring).
    """
    # rime_copilot owns the `.dict.yaml` parser, and it encodes more about the
    # format than is obvious (the `---`/`...` header, three column shapes, the
    # missing-reading fallback). A second parser here would be a second place
    # for those to be got wrong.
    from rime_copilot.dictfile import read_entries

    lex = Lexicon()
    per_source: dict[str, int] = {}
    for path in paths:
        entries = read_entries(path, keep=keep)
        for entry in entries:
            lex.add(entry.word, entry.pinyin.split())
        per_source[table_name(path)] = len(entries)
    return lex, per_source


def substrings(text: str, max_word: int = MAX_WORD) -> Iterable[str]:
    """Every substring of `text` up to `max_word` characters.

    The pre-filter that makes loading affordable: an entry whose text never
    appears in the corpus can never span any gold, so it need not be read,
    given a reading by pypinyin, or held in memory. On this corpus that drops
    `cn_dicts/tencent` from 981k entries to a few thousand.
    """
    n = len(text)
    for i in range(n):
        for j in range(i + 1, min(n, i + max_word) + 1):
            yield text[i:j]
