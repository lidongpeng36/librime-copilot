"""Regex redaction, applied centrally.

Adapters never redact. Keeping this in one place is what stops a third-party
adapter from becoming the weak link -- see the design doc's "Adapter contract".

Regex redaction is necessarily incomplete: internal project names, personal
names and customer names have no pattern. `cli.py stats` surfaces the residue
rather than pretending it is gone.
"""
from __future__ import annotations

import re

# Duplicated from corpus.HAN_CLASS rather than imported: corpus.py imports
# redact, so importing back would be circular. Keep the two in sync; the
# HanClassTest in corpus_test.py and test_han_ranges_match below both pin it.
_HAN_RANGES = "一-鿿㐀-䶿豈-﫿𠀀-𯨟"

# Order matters. URL runs before KEY and IP so that a token inside a query
# string is swallowed by the URL rule rather than half-matched by the others,
# and PATH runs last so it cannot eat a path-looking fragment of a URL.
#
# NO \b ANYWHERE. Python's \w includes Han, so there is no word boundary
# between 汉 and an ASCII letter: \b-anchored rules silently failed to fire on
# "见https://..." and swallowed whole sentences on "我的邮箱a@b.com给你".
# Every anchor here is an explicit ASCII lookaround instead.
_RULES: list[tuple[str, re.Pattern[str], str]] = [
    ("url", re.compile(r"https?://\S+?(?=[\s　-〿＀-￯" + _HAN_RANGES + r"]|$)"), "⟦URL⟧"),
    ("email", re.compile(r"(?<![A-Za-z0-9._+-])[A-Za-z0-9._+-]+@[A-Za-z0-9-]+\.[A-Za-z0-9.-]*[A-Za-z0-9]"), "⟦EMAIL⟧"),
    ("id", re.compile(r"(?<!\d)\d{17}[\dXx](?!\d)"), "⟦ID⟧"),
    ("phone", re.compile(r"(?<!\d)1[3-9]\d{9}(?!\d)"), "⟦PHONE⟧"),
    # AliCloud access key ids start LTAI and are shorter than the generic
    # long-run rule below catches. Found live in a real harvest: the
    # residue report is what surfaced them, which is the whole reason
    # `stats` lists what regexes did NOT match.
    ("key", re.compile(r"(?<![A-Za-z0-9])(?:sk-|ghp_|gho_|github_pat_|AKIA|LTAI)[A-Za-z0-9_-]{8,}"), "⟦KEY⟧"),
    # A mixed-case alphanumeric run of 24+ with at least one digit is a
    # secret far more often than it is prose. The generic rule below
    # requires 32+, which let a 30-character AliCloud secret through.
    ("key", re.compile(r"(?<![A-Za-z0-9])(?=[A-Za-z0-9]*[a-z])(?=[A-Za-z0-9]*[A-Z])(?=[A-Za-z0-9]*\d)[A-Za-z0-9]{24,}(?![A-Za-z0-9])"), "⟦KEY⟧"),
    ("key", re.compile(r"(?<![A-Za-z0-9+/])(?=[A-Za-z0-9+/]*[A-Za-z])(?=[A-Za-z0-9+/]*\d)[A-Za-z0-9+/]{32,}={0,2}(?![A-Za-z0-9+/])"), "⟦KEY⟧"),
    ("ip", re.compile(r"(?<![\d.])(?:\d{1,3}\.){3}\d{1,3}(?![\d.])"), "⟦IP⟧"),
    ("path", re.compile(r"/(?:Users|home)/[A-Za-z0-9._-]+"), "⟦PATH⟧"),
]


def redact(text: str) -> tuple[str, list[str]]:
    """Return the redacted text and the sorted set of categories substituted."""
    found: set[str] = set()
    for name, pattern, placeholder in _RULES:
        text, count = pattern.subn(placeholder, text)
        if count:
            found.add(name)
    return text, sorted(found)
