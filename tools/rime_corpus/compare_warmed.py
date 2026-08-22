"""Paired warmed/cold comparison of Rime's user-dictionary learning.

Drives ONE shared request list -- built once from the eval corpus half, so
every arm sees the identical list -- through two arms of the SAME rime-dir
family:

  cold : `replay.run_arm` -- pristine userdb, restored before and after.
  warm : `replay.run_warmed_arm` -- pristine userdb, then warmed on a
         SEPARATE request list (the train corpus half) before the same
         eval requests are measured, restored again afterward.

This is the module `docs/superpowers/specs/2026-08-22-lexicon-phase2-results.md`
names as having been produced by an ad-hoc, uncommitted `/tmp` script; see
that document's Reproduction section for the exact invocation.

Non-negotiable methodology (mirrors `compare_rerank.py`'s module docstring,
which states these for the same reason -- a previous run of this harness was
invalidated by getting them wrong):

  1. Both arms MUST see the identical MEASURED request list -- generated once
     here, from the eval corpus, not regenerated per arm.
  2. The user dictionary MUST be restored to a pristine snapshot before EACH
     arm (and again after) -- done here by delegating entirely to
     `replay.run_arm` / `replay.run_warmed_arm`, which own that guarantee.
  3. Segments are paired across arms by (request id, starting char position),
     not by segment index -- `compare_rerank.index_segments` /
     `compare_rerank.paired_delta` (reused unchanged, not reimplemented).

Reused from `compare_rerank` rather than reimplemented, per the above:
`build_requests` (shared request-list generation), `index_segments` and
`paired_delta` (the pairing and the GAIN/LOSS counts), `tally` and
`_print_tally` (per-arm accuracy reporting). The one thing that differs from
`compare_rerank`: one arm is warmed on a separate request list and the other
is not, rather than both arms differing only by a config toggle.

Adds McNemar's test (continuity-corrected) over `paired_delta`'s
`gained_hit0` / `lost_hit0` discordant-pair counts, since the results
document this reproduces reports a significance figure that `paired_delta`
itself does not compute. `mcnemar()` uses `math.erfc` for the p-value rather
than taking a `scipy` dependency for one number -- see its own docstring.
"""
from __future__ import annotations

import argparse
import math
import sys
from pathlib import Path

from . import corpus as _corpus
from . import replay as _replay
from .compare_rerank import (
    DEFAULT_PRISTINE,
    DEFAULT_REPLAYER,
    DEFAULT_WINDOW,
    _print_tally,
    build_requests,
    paired_delta,
    tally,
)

# Distinct from compare_rerank's DEFAULT_RIME_DIR / DEFAULT_RIME_DIR_NORERANK:
# those name an on/off config toggle on the SAME corpus; these name the two
# rime-dirs 2026-08-22-lexicon-phase2-results.md actually used (p1-both is
# Phase 1's shipped, unmodified arm; p2-warm is a scratch copy of it that gets
# warmed before each measurement).
DEFAULT_RIME_DIR_COLD = Path.home() / ".local" / "share" / "rime-corpus" / "p1-both"
DEFAULT_RIME_DIR_WARM = Path.home() / ".local" / "share" / "rime-corpus" / "p2-warm"


def mcnemar(b: int, c: int) -> tuple[float, float]:
    """Continuity-corrected (Yates) McNemar's test on discordant pair counts
    b and c (here: `gained_hit0` and `lost_hit0` from `paired_delta`).

    chi2 = (|b - c| - 1)^2 / (b + c) for b + c > 0. The p-value is the upper
    tail of the chi-square distribution with 1 degree of freedom, computed as
    `math.erfc(sqrt(chi2 / 2))` -- equivalent to `2 * (1 - Phi(sqrt(chi2)))`
    for the standard normal CDF Phi, since a chi-square_1 variate is a
    squared standard normal. Deliberately avoids a `scipy` dependency for
    this one number; this package's dependencies are kept minimal on
    purpose (see CLAUDE.md).

    b + c == 0 means no discordant pairs at all -- both arms agreed on every
    paired segment -- and returns (0.0, 1.0) rather than dividing by zero.
    """
    if b + c == 0:
        return 0.0, 1.0
    chi2 = (abs(b - c) - 1) ** 2 / (b + c)
    p = math.erfc(math.sqrt(chi2 / 2))
    return chi2, p


def _load_requests(corpus_dir: Path, label: str) -> tuple[list[dict], int] | None:
    records = [
        r for path in sorted(corpus_dir.glob("*.jsonl")) for r in _corpus.iter_records(path)
    ]
    if not records:
        print(f"no corpus at {corpus_dir}", file=sys.stderr)
        return None
    requests, skipped = build_requests(records)
    print(f"{label}: {len(records)} utterances -> {len(requests)} requests (skipped {skipped} unspellable)")
    return requests, skipped


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="compare_warmed", description=__doc__)
    parser.add_argument("--replayer", default=DEFAULT_REPLAYER)
    parser.add_argument(
        "--rime-dir-cold",
        default=str(DEFAULT_RIME_DIR_COLD),
        help="the cold arm's rime-dir (pristine userdb, restored around it)",
    )
    parser.add_argument(
        "--rime-dir-warm",
        default=str(DEFAULT_RIME_DIR_WARM),
        help="the warm arm's rime-dir (warmed on --warm-corpus-dir before each measurement)",
    )
    parser.add_argument("--pristine", default=str(DEFAULT_PRISTINE))
    parser.add_argument(
        "--eval-corpus-dir",
        required=True,
        help="the corpus half BOTH arms are measured on (e.g. an `eval` split from `rime-corpus split-time`)",
    )
    parser.add_argument(
        "--warm-corpus-dir",
        required=True,
        help="the corpus half the warm arm alone is warmed on (e.g. the `train` split)",
    )
    parser.add_argument("--window", type=int, default=DEFAULT_WINDOW)
    args = parser.parse_args(argv)

    rime_dir_cold = Path(args.rime_dir_cold)
    rime_dir_warm = Path(args.rime_dir_warm)
    pristine = Path(args.pristine)

    eval_dir = _corpus.corpus_dir(args.eval_corpus_dir)
    loaded = _load_requests(eval_dir, "eval")
    if loaded is None:
        return 1
    requests, _ = loaded

    warm_dir = _corpus.corpus_dir(args.warm_corpus_dir)
    warm_loaded = _load_requests(warm_dir, "warm")
    if warm_loaded is None:
        return 1
    warm_requests, _ = warm_loaded

    print("\nrunning the cold arm...")
    responses_cold = _replay.run_arm(
        requests, args.replayer, rime_dir_cold, pristine, window=args.window
    )

    print("\nrunning the warmed arm...")
    responses_warm = _replay.run_warmed_arm(
        warm_requests, requests, args.replayer, rime_dir_warm, pristine, window=args.window
    )

    tally_cold = tally(responses_cold, args.window)
    tally_warm = tally(responses_warm, args.window)
    _print_tally("COLD  (pristine userdb)", tally_cold)
    _print_tally("WARM  (warmed on train half)", tally_warm)

    # warm as "on", cold as "off" -- matches the results document's GAIN
    # (moved to hit==0 by warming, not cold) / LOSS (the reverse) framing.
    delta = paired_delta(responses_warm, responses_cold, args.window)
    gained, lost = delta["gained_hit0"], delta["lost_hit0"]
    chi2, p = mcnemar(gained, lost)

    print("\nPAIRED (by request id + starting char position):")
    print(f"  paired segments        : {delta['paired']}")
    print(f"  unpaired (WARM only)    : {delta['unpaired_on_only']}")
    print(f"  unpaired (COLD only)    : {delta['unpaired_off_only']}")
    print(f"  bucket C, WARM          : {delta['bucket_c_on']}")
    print(f"  bucket C, COLD          : {delta['bucket_c_off']}")
    print(f"  bucket C in BOTH arms   : {delta['bucket_c_both']}")
    print(f"  moved to hit==0 by WARM, not COLD (GAIN): {gained}")
    print(f"  moved to hit==0 by COLD, not WARM (LOSS): {lost}")
    print(f"  McNemar (continuity-corrected): chi2={chi2:.2f}  p={p:.4f}")
    if delta["gained_examples"]:
        print("  gain examples:", delta["gained_examples"])
    if delta["lost_examples"]:
        print("  loss examples:", delta["lost_examples"])

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
