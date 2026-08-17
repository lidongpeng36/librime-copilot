"""Export the LLM scoring set -- the input contract `tools/score_candidates.cc`
reads: one JSON object per line, `{id, bucket, ctx, cands, gold, gold_idx}`.

Only segments whose context ends in Han qualify. `replay_copilot` only
re-pushes real surrounding context for segment 2+ of a multi-segment request
(see `replay_copilot.cc`'s `committed_so_far`); segment 1 of every request
is, by construction, preceded by non-Han text -- it starts right where a
maximal Han run begins -- so its trailing context is empty by definition.
Before the mid-request re-push fix this was essentially every segment
(measured: 3818/3819). An LLM scored against an empty context is not testing
anything about Chinese continuation; it is scoring blind. Excluding those
segments is not a nicety -- it is what makes the exported set mean anything.

Only two buckets are exported, and both are needed:

  A - the false-promotion set: the first candidate is already gold. Would
      the LLM have promoted something else ahead of the correct,
      already-first answer?
  C - the hit-rate set: gold exists in the window but isn't first. Can the
      LLM find it?

A hit rate on C without a false-promotion rate on A is not a result -- a
scorer that "finds" gold by promoting everything looks perfect on C and
terrible in practice.
"""
from __future__ import annotations

import re
from typing import Iterable

from . import corpus
from .metrics import bucket as _bucket

_TRAILING_HAN = re.compile(f"[{corpus.HAN_CLASS}]+$")


def _has_trailing_han(text: str) -> bool:
    return bool(_TRAILING_HAN.search(text))


def build_evalset(
    requests: Iterable[dict], responses: Iterable[dict], window: int = 32
) -> list[dict]:
    """One record per qualifying segment.

    `requests` and `responses` are paired by `id` (a request's own id, e.g.
    `"<corpus-id>#<run-index>"`), not by list position -- responses need not
    arrive in the same order, and a request whose push failed produces a
    `status: "error"` response with no matching request lookup needed at all.

    Per-segment context is reconstructed the same way `replay_copilot` builds
    it internally: `request["ctx"]` for the first segment, plus whatever
    EARLIER segments in this SAME response actually committed
    (`segment["want"]`, only when that segment was a hit) for every segment
    after it. Nothing in the response carries this back directly -- the
    response only records `want`/`hit`/`cands` per segment -- so it must be
    accumulated here exactly as `replay_copilot.cc`'s own `committed_so_far`
    does.
    """
    by_id = {r["id"]: r for r in requests}
    out: list[dict] = []
    for response in responses:
        if response.get("status") == "error":
            continue  # untrustworthy context -- same exclusion as metrics.summarize
        request = by_id.get(response.get("id"))
        if request is None:
            continue
        committed_so_far = ""
        for index, segment in enumerate(response.get("segments", [])):
            ctx = request["ctx"] + committed_so_far
            hit = segment["hit"]
            cands = segment.get("cands", [])
            want = segment["want"]
            # Mirrors replay_copilot.cc: committed_so_far only grows on a
            # real hit (select_candidate happened); the final hit==-1
            # sentinel segment of a diverged request commits nothing.
            if hit >= 0:
                committed_so_far += want

            label = _bucket(hit, cands, window)
            if label not in ("A", "C"):
                continue
            if not _has_trailing_han(ctx):
                continue

            out.append(
                {
                    "id": f"{response['id']}:{index}",
                    "bucket": label,
                    "ctx": ctx,
                    "cands": cands,
                    "gold": want,
                    # A: hit == 0. C: 0 < hit < window, so gold is already
                    # inside the window-truncated `cands` list at index hit.
                    "gold_idx": hit,
                }
            )
    return out
