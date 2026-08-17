"""全拼 syllable -> the keys the user actually presses.

The mapping is NOT transcribed by hand. It executes the `speller/algebra`
block from the user's own schema, which is the authoritative definition and is
already in the right direction (dictionary 全拼 -> typed spelling). A
hand-written table would introduce errors the schema does not have, and would
silently diverge from a schema that carries custom fuzzy rules.

Rime's four operators:

  erase/PATTERN/        drop spellings matching PATTERN
  derive/PATTERN/REPL/  ADD an alternate spelling, keeping the original
  xform/PATTERN/REPL/   REPLACE in place
  xlit/FROM/TO/         transliterate character by character

`derive` branches, so a syllable can end with several accepted spellings --
`ai` yields both `ad` and `ai`. That is what `derive` means: alternate
spellings Rime also accepts, which therefore produce identical candidates.
Zero-initial syllables reach their canonical two-key form THROUGH the derive
branch (ai -> aai -> ad), so derive cannot be skipped.

Among two-key results we take sorted()[0]. `cli.py verify-speller` checks that
choice against real Rime rather than trusting it.
"""
from __future__ import annotations

import re
from pathlib import Path

FLYPY_RULES = Path(__file__).resolve().parent / "spellers" / "flypy.txt"


def load_rules(path: Path) -> list[str]:
    rules = []
    for line in Path(path).read_text(encoding="utf-8").splitlines():
        line = line.strip()
        if line and not line.startswith("#"):
            rules.append(line)
    return rules


class Speller:
    def __init__(self, rules: list[str]):
        self._ops = [self._parse(rule) for rule in rules]
        self._cache: dict[str, str | None] = {}

    @staticmethod
    def _parse(rule: str):
        kind, rest = rule.split("/", 1)
        parts = re.split(r"(?<!\\)/", rest)
        if kind == "xlit":
            return kind, parts[0], parts[1]
        pattern = re.compile(parts[0])
        # Rime writes group references as $1; Python wants \1.
        replacement = parts[1].replace("$", "\\") if len(parts) > 1 else ""
        return kind, pattern, replacement

    def keys(self, syllable: str) -> str | None:
        """The two keys for one 全拼 syllable, or None if the rules reject it."""
        if syllable not in self._cache:
            self._cache[syllable] = self._run(syllable)
        return self._cache[syllable]

    def _run(self, syllable: str) -> str | None:
        spellings = {syllable}
        for op in self._ops:
            kind = op[0]
            if kind == "erase":
                pattern = op[1]
                spellings = {s for s in spellings if not pattern.search(s)}
            elif kind == "derive":
                pattern, replacement = op[1], op[2]
                spellings |= {
                    pattern.sub(replacement, s) for s in spellings if pattern.search(s)
                }
            elif kind == "xform":
                pattern, replacement = op[1], op[2]
                spellings = {pattern.sub(replacement, s) for s in spellings}
            elif kind == "xlit":
                table = str.maketrans(op[1], op[2])
                spellings = {s.translate(table) for s in spellings}
        two = sorted(s for s in spellings if len(s) == 2 and s.isascii())
        return two[0] if two else None


def syllables(text: str) -> list[str]:
    """全拼 syllables for a run of Han characters, one per character.

    pypinyin is imported here rather than at module scope so that this package
    keeps its promise of importing on a stock interpreter -- the same rule
    tools/rime_copilot follows.
    """
    from pypinyin import Style, lazy_pinyin

    return lazy_pinyin(text, style=Style.NORMAL, errors="ignore")
