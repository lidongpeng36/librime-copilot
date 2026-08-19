"""The corpora this pipeline pulls from, with why each one is here.

Every URL is a direct file, not a `datasets`-library identifier: the two
useful sources both resolve to plain HTTP downloads (verified 2026-08-19), and
depending on `datasets` would pull in arrow, pandas and a script-execution
trust prompt to fetch files `requests` already fetches. `requests` is already
a RUNTIME_REQUIREMENT of this repo; nothing new is added.

Registers are tagged, not merged. The eval set is 46% technical and 54%
colloquial, the installed grammar is trained on essay text, and its measured
weakness is register-shaped (67.8% colloquial against 71.1% technical). A
pipeline that threw the tag away could not answer which half of the corpus
bought which half of any improvement.
"""
from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True)
class Source:
    name: str
    url: str
    register: str
    # Where the text lives in each JSON line, or "" when the line IS the value.
    # Verified against the real files rather than the dataset cards: LCCC's
    # lines are bare JSON arrays of utterances, not objects with a `dialog`
    # key; SkyPile's are objects with a "text" field.
    field: str
    is_dialogue: bool = False
    # Ships word-separated rather than as written; see normalize.join_han_tokens
    # for why leaving it alone silently empties the collocation table.
    pre_tokenized: bool = False
    note: str = ""


SOURCES = {
    # 1.2M dialogues, ~350MB gzipped. The closest public match to chat, and
    # aimed squarely at the register the installed grammar is worst on.
    "lccc-base": Source(
        name="lccc-base",
        url="https://huggingface.co/datasets/silver/lccc/resolve/main/lccc_base_train.jsonl.gz",
        register="colloquial",
        field="",
        is_dialogue=True,
        pre_tokenized=True,
        note="LCCC base: cleaned Chinese conversation",
    ),
    # The larger cut of the same corpus, for when base is not enough.
    "lccc-large": Source(
        name="lccc-large",
        url="https://huggingface.co/datasets/silver/lccc/resolve/main/lccc_large.jsonl.gz",
        register="colloquial",
        field="",
        is_dialogue=True,
        pre_tokenized=True,
        note="LCCC large: same cleaning, more of it",
    ),
}

# SkyPile ships 437 shards of ~1.3GB each, named by crawl and rank. One is
# already past what a 3-and-4-character collocation table needs; a 43M-parameter
# model wants billions of tokens, and each shard yields roughly 263M characters
# after normalization, so training scale is measured in shards rather than in
# corpora. `head` shards come first: SkyPile ranks them by a quality model, so
# taking them in order takes the better text first.
_SKYPILE = "https://huggingface.co/datasets/Skywork/SkyPile-150B/resolve/main/data/"
_SKYPILE_SHARDS = [f"2020-40_zh_head_{i:04d}.jsonl" for i in range(18)]

for _i, _shard in enumerate(_SKYPILE_SHARDS):
    SOURCES[f"skypile-{_i}"] = Source(
        name=f"skypile-{_i}",
        url=_SKYPILE + _shard,
        register="general",
        field="text",
        note=f"SkyPile-150B {_shard}",
    )
