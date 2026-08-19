"""Exact-duplicate removal, streaming.

Web corpora are largely boilerplate -- navigation, licence footers, cookie
notices -- repeated across every page of a site. Left in, those are the
highest-count collocations in the table, so the grammar learns furniture
instead of language. Exact line hashing removes the bulk of it for one set
membership test per line; near-duplicate detection is a larger tool and is not
reached for until the counts show it is needed.
"""
from __future__ import annotations

import hashlib
from typing import Iterable, Iterator


def unique(lines: Iterable[str]) -> Iterator[str]:
    """Yield each distinct line once, in first-seen order.

    Hashes rather than the lines themselves: a multi-gigabyte corpus does not
    fit in a set of its own strings, and a 16-byte digest per line does.
    """
    seen: set[bytes] = set()
    for line in lines:
        digest = hashlib.blake2b(line.encode("utf-8"), digest_size=16).digest()
        if digest not in seen:
            seen.add(digest)
            yield line
