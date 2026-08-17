"""Paired on/off comparison of the DB re-ranker (`copilot/rerank/enable`).

Drives the SAME request list through two deployed Rime directories that are
identical except for one config line -- see `.superpowers/sdd/
2026-08-15-corpus-eval-harness-poc/extension-rerank-ab-report.md` for how
they were built -- and reports the four-way split (`bucket`) per arm plus a
PAIRED delta on bucket C, the only bucket re-ranking is meant to move.

Why this needs its own script rather than being folded into `oracle`
(cli.py): `metrics.bucket`/`metrics.summarize` answer "how much headroom
exists", a SINGLE-arm question. This script answers a different one -- "how
much of that headroom does the EXISTING re-ranker actually take" -- which
needs two arms run under identical conditions and PAIRED per-segment, not
independently tallied. `replay.py` and `metrics.py` (bucket, restore,
strip_timings, assert_deterministic) supply everything single-arm; only the
pairing (`index_segments`, `paired_delta`) is specific to this comparison.

Non-negotiable methodology (repeats task instructions verbatim because a
previous run of this harness was invalidated by getting them wrong):

  1. The user dictionary MUST be restored to a pristine snapshot before EACH
     arm, and again afterward. Without this the replay trains the dictionary
     on the very corpus it measures -- observed: top-1 accuracy over the same
     2320 requests went 32.8% -> 99.2% between two passes.
  2. Both arms MUST see the identical request list -- generated once here,
     not regenerated per arm (a corpus.iter_records() re-walk is not
     guaranteed stable across two calls if the corpus file changes mid-run).
  3. Segments are paired across arms by (request id, starting char position)
     -- NOT by segment index. FindLongestPrefixMatch (src/replay_align.h)
     matches a variable-length span per arm, so the SAME request can split
     into a different number of segments in each arm (observed directly: one
     arm returned a single 11-character segment where the other returned
     five). A segment's start position is anchored to the shared `text`/
     `keys`, independent of how either arm chose to segment it, so it is the
     only stable pairing key. A start position that is a boundary in only one
     arm has no partner and is excluded from the paired counts (see
     `unpaired`), not silently matched to the wrong segment.
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path
from typing import Iterable

from . import corpus as _corpus
from . import replay as _replay
from . import speller as _speller
from .metrics import bucket

DEFAULT_WINDOW = 32  # must equal copilot/rerank/window in the replay schema (rerank.h kDefaultWindow)
DEFAULT_REPLAYER = "/Users/lidongpeng/repo/librime/build/plugins/copilot/bin/replay_copilot"
DEFAULT_RIME_DIR = Path.home() / ".local/share/rime-corpus/rime-dir"
DEFAULT_RIME_DIR_NORERANK = Path.home() / ".local/share/rime-corpus/rime-dir-norerank"
DEFAULT_PRISTINE = _replay.DEFAULT_PRISTINE_DIR


def build_requests(records: list[dict]) -> tuple[list[dict], int]:
    """One request per maximal Han run in the corpus, generated ONCE so every
    arm sees an identical list (methodology requirement 2). Returns
    (requests, skipped) -- `skipped` counts runs speller.syllables/keys could
    not spell.

    Thin wrapper over replay.build_requests: the actual per-syllable spelling
    and request assembly logic lives there now (shared with `oracle` and
    `export-evalset`) so this module carries only what is specific to it --
    the skipped-run diagnostic this script prints."""
    sp = _speller.Speller(_speller.load_rules(_speller.FLYPY_RULES))
    total_runs = sum(len(_replay.han_runs(rec["text"])) for rec in records)
    requests = _replay.build_requests(records, sp)
    return requests, total_runs - len(requests)


def tally(responses: list[dict], window: int) -> dict:
    """Per-arm A/B/C/D counts. Requests with status:"error" are excluded
    entirely (their context could not be trusted -- see replay_copilot.cc);
    "diverged" requests still contribute their completed segments plus their
    final hit:-1 segment (bucket D), same as the preliminary run."""
    counts: Counter = Counter()
    total = 0
    for r in responses:
        if r.get("status") == "error":
            continue
        for seg in r.get("segments", []):
            counts[bucket(seg["hit"], seg.get("cands", []), window)] += 1
            total += 1
    return {"total": total, **{k: counts[k] for k in "ABCD"}}


def index_segments(responses: list[dict]) -> dict[str, dict[int, dict]]:
    """request id -> {start_char_pos: segment}. `span` is in KEY units at two
    keys per Han character (双拼, see replay_copilot.cc), hence // 2. Error
    responses are dropped so nothing pairs against an untrustworthy context."""
    out: dict[str, dict[int, dict]] = {}
    for r in responses:
        if r.get("status") == "error":
            continue
        out[r["id"]] = {seg["span"][0] // 2: seg for seg in r.get("segments", [])}
    return out


def paired_delta(responses_on: list[dict], responses_off: list[dict], window: int) -> dict:
    """The headline comparison: bucket C paired between arms, plus which
    segments moved to hit==0 in only one arm (methodology requirement 3 for
    how segments are paired)."""
    idx_on = index_segments(responses_on)
    idx_off = index_segments(responses_off)
    common_ids = set(idx_on) & set(idx_off)

    paired = 0
    unpaired_on = 0
    unpaired_off = 0
    c_on = c_off = c_both = 0
    gained: list[dict] = []  # off wasn't hit==0, on is  (rerank FIXES)
    lost: list[dict] = []  # off was hit==0, on isn't  (rerank BREAKS)

    for rid in set(idx_on) | set(idx_off):
        pos_on = set(idx_on.get(rid, {}))
        pos_off = set(idx_off.get(rid, {}))
        unpaired_on += len(pos_on - pos_off)
        unpaired_off += len(pos_off - pos_on)
        if rid not in common_ids:
            continue
        for pos in pos_on & pos_off:
            seg_on = idx_on[rid][pos]
            seg_off = idx_off[rid][pos]
            paired += 1
            b_on = bucket(seg_on["hit"], seg_on.get("cands", []), window)
            b_off = bucket(seg_off["hit"], seg_off.get("cands", []), window)
            if b_on == "C":
                c_on += 1
            if b_off == "C":
                c_off += 1
            if b_on == "C" and b_off == "C":
                c_both += 1
            on_first = seg_on["hit"] == 0
            off_first = seg_off["hit"] == 0
            if on_first and not off_first:
                gained.append({"id": rid, "pos": pos, "want": seg_on.get("want", "")})
            elif off_first and not on_first:
                lost.append({"id": rid, "pos": pos, "want": seg_off.get("want", "")})

    return {
        "paired": paired,
        "unpaired_on_only": unpaired_on,
        "unpaired_off_only": unpaired_off,
        "bucket_c_on": c_on,
        "bucket_c_off": c_off,
        "bucket_c_both": c_both,
        "gained_hit0": len(gained),
        "lost_hit0": len(lost),
        "gained_examples": gained[:10],
        "lost_examples": lost[:10],
    }


def _print_tally(label: str, t: dict) -> None:
    total = t["total"] or 1
    print(f"\n{label}: {t['total']} segments")
    print(f"  A first                : {t['A']:6}  ({t['A']/total:.1%})")
    print(f"  B raw-first-by-design   : {t['B']:6}  ({t['B']/total:.1%})")
    print(f"  C real opportunity      : {t['C']:6}  ({t['C']/total:.1%})")
    print(f"  D unreachable           : {t['D']:6}  ({t['D']/total:.1%})")


def llm_engagement(responses: list[dict]) -> dict:
    """How often the LLM re-rank path engaged versus fell back, per
    `llm_skip` (rerank_filter.cc's OWN fallback-chain verdict, read back from
    its RerankTrace -- see rerank_trace.h / replay_copilot.cc's ObserveLlm --
    not re-derived by this tool), and the REAL per-scoring latency
    (`us.llm_score`, a timer around the actual Scorer::Score() call inside
    RerankTranslation::Replenish(), rerank_filter.cc) for the segments that
    were actually scored.

    `us.llm_score` is NOT `us.menu`. An earlier version of this function used
    `us.menu` (the wall-clock around WalkCandidates()) as a latency proxy;
    that was wrong -- for most segments WalkCandidates() reads an
    already-materialized menu (the real Score() call happens earlier,
    synchronously inside a preceding select_candidate(), see
    replay_copilot.cc's PendingSegmentContext) -- so `us.menu` was timing a
    cache hit, not the scoring, and read 1-2 orders of magnitude too fast.
    See task-6-report.md for how that was caught.

    Only meaningful on a run made with --wait-for-warm: `llm_skip` is only
    present on segments from such a run (replay_copilot.cc gates the field on
    Options::wait_for_warm), so a plain run's responses tally as all-empty
    here, not a silent zero-engagement result.
    """
    reasons: Counter = Counter()
    total = 0
    engaged_latencies_us: list[int] = []
    for r in responses:
        if r.get("status") == "error":
            continue
        for seg in r.get("segments", []):
            skip = seg.get("llm_skip")
            if skip is None:
                continue
            total += 1
            reasons[skip] += 1
            if "llm_score" in seg.get("us", {}):
                engaged_latencies_us.append(seg["us"]["llm_score"])
    return {
        "total": total,
        "reasons": dict(reasons),
        "engaged": reasons.get("none", 0),
        "engaged_latency_us": sorted(engaged_latencies_us),
    }


def _percentile(sorted_values: list[int], p: float) -> float:
    if not sorted_values:
        return 0.0
    idx = min(len(sorted_values) - 1, int(len(sorted_values) * p))
    return sorted_values[idx]


def _print_llm_engagement(label: str, e: dict) -> None:
    total = e["total"] or 1
    print(f"\n{label}: {e['total']} segments had an llm_skip verdict (--wait-for-warm)")
    for reason, count in sorted(e["reasons"].items(), key=lambda kv: -kv[1]):
        print(f"  {reason:10}: {count:6}  ({count/total:.1%})")
    lat = e["engaged_latency_us"]
    if lat:
        print(
            f"  real per-scoring latency (us.llm_score, n={len(lat)}): "
            f"p50={_percentile(lat, 0.5):.0f} p90={_percentile(lat, 0.9):.0f} "
            f"max={lat[-1]}"
        )


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="compare_rerank", description=__doc__)
    parser.add_argument("--replayer", default=DEFAULT_REPLAYER)
    parser.add_argument("--rime-dir", default=str(DEFAULT_RIME_DIR), help="rerank ON")
    parser.add_argument(
        "--rime-dir-norerank", default=str(DEFAULT_RIME_DIR_NORERANK), help="rerank OFF"
    )
    parser.add_argument("--pristine", default=str(DEFAULT_PRISTINE))
    parser.add_argument("--corpus-dir", default=None)
    parser.add_argument("--window", type=int, default=DEFAULT_WINDOW)
    parser.add_argument(
        "--wait-for-warm",
        action="store_true",
        help=(
            "Forward --wait-for-warm to replay_copilot on BOTH arms (a no-op on an "
            "arm with no copilot/rerank/llm scorer configured -- see replay_copilot.cc). "
            "Forces every segment's LLM re-rank scorer hot before it is consulted, "
            "instead of the live fire-and-forget warm triggers replay's back-to-back, "
            "always-new-context requests never give time to finish. Measures scoring "
            "quality with warming forced, NOT live warm-hit rate (unmeasurable "
            "offline -- see the 'Warming' section of "
            "docs/superpowers/specs/2026-08-17-llm-rerank-design.md). Prints an "
            "engagement/latency breakdown for the ON arm when set."
        ),
    )
    args = parser.parse_args(argv)

    rime_dir_on = Path(args.rime_dir)
    rime_dir_off = Path(args.rime_dir_norerank)
    pristine = Path(args.pristine)

    directory = _corpus.corpus_dir(args.corpus_dir)
    records = [r for path in sorted(directory.glob("*.jsonl")) for r in _corpus.iter_records(path)]
    if not records:
        print(f"no corpus at {directory}", file=sys.stderr)
        return 1
    requests, skipped = build_requests(records)
    print(f"{len(records)} utterances -> {len(requests)} requests (skipped {skipped} unspellable)")

    # Determinism check FIRST (methodology requirement 5), on the ON arm --
    # this also produces that arm's actual result, so it is not a throwaway
    # pass. See replay.assert_deterministic's docstring for why reusing it is
    # valid; the pristine-userdb restore happens INSIDE it, before and after
    # each of its two passes.
    if args.wait_for_warm:
        print(
            "\n--wait-for-warm is ON: every request forces its LLM scorer hot before "
            "reading candidates. This measures scoring quality with warming forced, "
            "NOT live warm-hit rate -- see the flag's own --help text."
        )
    print("\nrunning the ON arm twice for the determinism check...")
    try:
        responses_on = _replay.assert_deterministic(
            requests,
            args.replayer,
            rime_dir_on,
            pristine,
            window=args.window,
            wait_for_warm=args.wait_for_warm,
        )
    except AssertionError as exc:
        print(f"determinism (ON arm, timings stripped): MISMATCH\n{exc}", file=sys.stderr)
        return 3
    print("determinism (ON arm, timings stripped): OK, identical")

    print("\nrunning the OFF arm...")
    responses_off = _replay.run_arm(
        requests,
        args.replayer,
        rime_dir_off,
        pristine,
        window=args.window,
        wait_for_warm=args.wait_for_warm,
    )

    tally_on = tally(responses_on, args.window)
    tally_off = tally(responses_off, args.window)
    _print_tally("ON  (rerank enabled)", tally_on)
    _print_tally("OFF (rerank disabled)", tally_off)

    if args.wait_for_warm:
        _print_llm_engagement("ON  (rerank enabled)", llm_engagement(responses_on))

    delta = paired_delta(responses_on, responses_off, args.window)
    print(f"\nPAIRED (by request id + starting char position):")
    print(f"  paired segments        : {delta['paired']}")
    print(f"  unpaired (ON only)      : {delta['unpaired_on_only']}")
    print(f"  unpaired (OFF only)     : {delta['unpaired_off_only']}")
    print(f"  bucket C, ON            : {delta['bucket_c_on']}")
    print(f"  bucket C, OFF           : {delta['bucket_c_off']}")
    print(f"  bucket C in BOTH arms   : {delta['bucket_c_both']}")
    print(f"  moved to hit==0 by ON, not OFF (GAIN): {delta['gained_hit0']}")
    print(f"  moved to hit==0 by OFF, not ON (LOSS): {delta['lost_hit0']}")
    if delta["gained_examples"]:
        print("  gain examples:", delta["gained_examples"])
    if delta["lost_examples"]:
        print("  loss examples:", delta["lost_examples"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
