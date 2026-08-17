"""The four-way bucket split (A/B/C/D) and the oracle bound.

**Why four buckets, not three.** The plan this package started from split
segments into `first` / `opportunity` / `unreachable` -- treating every
segment where the correct candidate exists but isn't first as "opportunity".
That merges two things that must never be merged: `RawInputFilter`
deliberately inserts the raw keystroke input ahead of a "sentence" candidate
on purpose (`src/filters.cc:139`), which is policy, not headroom. Counting it
as opportunity produced a bound of 67.1% where the truth -- once that bucket
was split out -- was 27.5%. See
`.superpowers/sdd/2026-08-15-corpus-eval-harness-poc/progress.md` for the
full before/after. The four buckets:

  A - hit == 0                          already correct; re-ranking can only harm
  B - 0 < hit < window, cands[0] not Han  RawInputFilter's own policy, not headroom
  C - 0 < hit < window, cands[0] IS Han   the ONLY real re-ranking opportunity
  D - otherwise                          out of reach (absent, or past the window)

`corpus.HAN_CLASS` is the shared definition of "Han" between the corpus, the
replayer's own segmentation, and this bucketing -- one regex, not three that
must agree.

Absolute rates here carry reconstruction bias: the corpus contains text the
user WROTE, not input the user PERFORMED, so the baseline is systematically
overstated. That does not affect the oracle bound's role as a go/no-go
threshold, but it does mean these numbers are not reportable as live accuracy.
"""
from __future__ import annotations

import re
from collections import Counter
from typing import Iterable

from . import corpus

_ALL_HAN = re.compile(f"^[{corpus.HAN_CLASS}]+$")


def bucket(hit: int, cands: list[str], window: int) -> str:
    """A/B/C/D for one segment.

    B vs C is the single most consequential branch in this file: both
    require `0 < hit < window`, and differ ONLY on whether `cands[0]` is
    entirely Han. `cands[0]` being ASCII (or empty, or mixed) means
    RawInputFilter inserted the raw input ahead of a sentence candidate ON
    PURPOSE (`src/filters.cc:139`) -- policy the plugin chose, not headroom a
    re-ranker could take. Folding it into "opportunity" is what produced a
    bound of 67.1% where the real number is 27.5%.
    """
    if hit == 0:
        return "A"
    if 0 < hit < window:
        first = cands[0] if cands else ""
        return "C" if _ALL_HAN.fullmatch(first) else "B"
    return "D"


def severity(picked: str, gold: str) -> str:
    """Classify a re-ranker's disagreement with gold as "correct", "prefix",
    "extension", or "wrong-word".

    Only "wrong-word" is a harmful regression. Measured: 28.3% of apparent
    "false promotions" (bucket A cases where a candidate other than gold was
    promoted to first) were merely PREFIXES of gold -- e.g. promoting "运行"
    when gold was "运行了". A user who typed on past that point would still
    end up with the right sentence, so counting a prefix as equally bad as a
    genuinely wrong word understated the net effect of a re-ranker by a
    third. An "extension" (gold plus more) is the mirror case and is equally
    benign for the same reason.
    """
    if picked == gold:
        return "correct"
    if picked and gold.startswith(picked):
        return "prefix"
    if gold and picked.startswith(gold):
        return "extension"
    return "wrong-word"


def _tally(counts: Counter) -> dict:
    total = sum(counts.values())
    out: dict = {"total": total}
    for label in "ABCD":
        out[label] = counts[label]
    # Bucket C is the ONLY bucket re-ranking can legitimately claim as
    # headroom (see module docstring) -- B is excluded on purpose, not an
    # oversight.
    out["oracle_bound"] = counts["C"] / total if total else 0.0
    return out


def _gold_rank_breakdown(responses: Iterable[dict], window: int) -> dict[int, int]:
    """Within bucket C, where does the correct candidate actually rank?

    `hit` IS the rank (0-based, in the FULL walked candidate list, not the
    window-truncated one) -- reported here as a distribution rather than
    folded into a single number, because "27.5% opportunity, all at rank 1"
    and "27.5% opportunity, half beyond rank 10" are very different
    re-ranking problems with very different fixes.
    """
    ranks: Counter = Counter()
    for response in responses:
        if response.get("status") == "error":
            continue
        for segment in response.get("segments", []):
            hit = segment["hit"]
            cands = segment.get("cands", [])
            if bucket(hit, cands, window) == "C":
                ranks[hit] += 1
    return dict(sorted(ranks.items()))


def summarize(responses: Iterable[dict], requests: Iterable[dict] | None = None, window: int = 32) -> dict:
    """The four-way split, the oracle bound, the divergence rate, and the
    rank-of-gold breakdown inside bucket C.

    Requests with `status == "error"` are excluded entirely -- their context
    could not be trusted (see replay_copilot.cc's push-failure contract), so
    their `hit`/`cands` are not evidence of anything. `"diverged"` requests
    still contribute their completed segments plus their final `hit == -1`
    sentinel segment (bucket D).

    `requests` is accepted for interface symmetry with the rest of the
    harness -- a caller assembling a report typically has both the requests
    and the responses in hand -- but nothing here currently needs to join
    back to it; every value `summarize` computes already lives on each
    response. Kept as a parameter rather than dropped so a future per-source
    or per-context-length breakdown does not have to change every call site.
    """
    del requests  # see docstring: accepted, not yet consulted
    responses = list(responses)
    counts: Counter = Counter()
    total_responses = 0
    errors = 0
    diverged = 0
    for response in responses:
        total_responses += 1
        status = response.get("status")
        if status == "error":
            errors += 1
            continue
        if status == "diverged":
            diverged += 1
        for segment in response.get("segments", []):
            counts[bucket(segment["hit"], segment.get("cands", []), window)] += 1

    trustworthy = total_responses - errors
    return {
        "buckets": _tally(counts),
        "responses": total_responses,
        "errors": errors,
        "diverged": diverged,
        "divergence_rate": diverged / trustworthy if trustworthy else 0.0,
        "gold_rank_in_c": _gold_rank_breakdown(responses, window),
    }
