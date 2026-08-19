"""Counting the collocations octagram stores, and emitting its build input.

Pinned to the consumer rather than chosen: `Octagram` looks up Han collocations
of `collocation_min_length` to `collocation_max_length` characters (3 and 4 by
default, octagram.cc), and `build_grammar` reads whitespace-separated
`key value` pairs, storing `max(0, int(log(value) * 10000))` (gram_db.cc:68).
So the value is a count, a count of 1 stores as 0, and anything below 1 would
store as 0 too -- which is still better than absent, since a missing key scores
`non_collocation_penalty` (-12) instead.
"""
from __future__ import annotations

from collections import Counter
from typing import Iterable

MIN_LENGTH = 3
MAX_LENGTH = 4


def count(sentences: Iterable[str], min_length: int = MIN_LENGTH,
          max_length: int = MAX_LENGTH, shard: int = 0, shards: int = 1) -> Counter:
    """Every window of `min_length`..`max_length` characters, within sentences.

    Never across them: a window spanning a sentence boundary is a collocation
    no one ever types, and the caller's splitter is what guarantees each input
    string is one Han run.

    `shards` bounds memory by counting only the keys whose first character
    falls in this shard, at the cost of one pass over the corpus per shard.
    Measured on LCCC: 1M sentences produce 5.9M distinct keys at 1.06 GB
    resident, and the full 17.5M would reach an estimated 10-15 GB in one
    pass. Sharding on the FIRST character rather than a hash of the whole key
    lets the test hoist out of the length loop -- every window starting at a
    position shares it -- so the extra passes cost I/O, not comparisons.

    Sharding on the first character is also what makes `emit_conditional`
    possible under sharding at all: a key and its prefix `key[:-1]` share a
    first character, so they always land in the same shard and a shard's
    counts are self-sufficient for the division.
    """
    counts: Counter = Counter()
    sharded = shards > 1
    for sentence in sentences:
        n = len(sentence)
        for i in range(n - min_length + 1):
            if sharded and ord(sentence[i]) % shards != shard:
                continue
            for size in range(min_length, min(max_length, n - i) + 1):
                counts[sentence[i:i + size]] += 1
    return counts


# Multiplier that lets a conditional probability survive build_grammar's
# `max(0, int(log(value) * 10000))`. Without it every probability -- all of
# them below 1, so all with negative logs -- would clamp to a stored 0 and the
# table would rank nothing. 1e6 keeps probabilities down to one in a million
# distinguishable, which is far past where a 4-gram estimate means anything.
PROB_SCALE = 1_000_000


def emit(counts: Counter, min_count: int = 2) -> Iterable[str]:
    """`key value` lines for build_grammar, commonest first.

    `min_count` defaults above 1 because a collocation seen once is as likely
    to be a typo or a scrape artifact as a phrase, and singletons are the bulk
    of any n-gram table -- keeping them would trade most of the file size for
    the least trustworthy entries.
    """
    for key, value in counts.most_common():
        if value >= min_count:
            yield f"{key} {value}"


def emit_conditional(counts: Counter, min_count: int = 2,
                     min_length: int = MIN_LENGTH,
                     max_length: int = MAX_LENGTH) -> Iterable[str]:
    """`key value` lines where value is P(last char | the rest), scaled.

    Raw counts are what `emit` writes and what octagram's own arithmetic then
    treats as the score: `Query` returns `log(count) + collocation_penalty`,
    monotone in raw frequency with no conditioning at all. That hands a large
    bonus to whatever is merely frequent -- on a social-media corpus, 哈哈哈
    and its relatives -- regardless of whether it fits the context, which is
    the opposite of what a language model should do.

    A conditional probability says what the collocation is actually evidence
    for: given the characters before it, how expected is this one. It is
    expressible here only because the scale factor lifts it above the format's
    `max(0, ...)` clamp; the clamp is why raw counts look like the intended
    input at first reading.

    Requires `counts` to hold every prefix length as well, i.e. counted from
    `min_length - 1`.
    """
    for key, value in counts.most_common():
        if value < min_count or not (min_length <= len(key) <= max_length):
            continue
        prefix = counts.get(key[:-1], 0)
        if prefix <= 0:
            continue
        scaled = int(value / prefix * PROB_SCALE)
        if scaled >= 1:
            yield f"{key} {scaled}"
