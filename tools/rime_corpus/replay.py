"""Turn corpus utterances into replay requests, drive `replay_copilot`, and
police the two invariants that make its output trustworthy.

Both invariants were violated by the plan this package started from, and
both were caught only by actually running the harness -- see
`.superpowers/sdd/2026-08-15-corpus-eval-harness-poc/progress.md`:

  1. Determinism (Sentinel 4) can never be checked byte-for-byte: every
     response carries `us` wall-clock timings that differ between any two
     runs by definition. `strip_timings` removes them before comparison;
     with that done, the replayer is genuinely deterministic (verified over
     the full 2320-request corpus).

  2. Replay COMMITS candidates, and committing trains Rime's user
     dictionary. Left unguarded, top-1 accuracy over the identical 2320
     requests went 32.8% -> 99.2% between two consecutive passes, because
     Rime had memorised the corpus's own sentences -- the harness training
     on its own test set. `restore_pristine_userdb` must run before EVERY
     arm of EVERY measurement (and again after, so no run leaves contaminated
     state for whatever runs next against the same directory). This is not
     the same as `enable_user_dict: false`: that disables READING the
     dictionary too, and measuring without the user's own learned phrases
     produced a wildly different, meaningless number.

Sentence splitting (`han_runs`) lives here, not in corpus.py, precisely so
it can be changed without rebuilding the corpus.
"""
from __future__ import annotations

import copy
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path
from typing import Iterable, Sequence

from . import corpus

# Built from corpus.HAN_CLASS so the corpus, the replayer's own segmentation,
# and metrics.bucket's B/C split can never disagree about what counts as
# Chinese.
_HAN_RUN = re.compile(f"[{corpus.HAN_CLASS}]+")

# The two LevelDB directories that carry learned state, mirroring the
# pristine snapshot's own contents. If a third userdb is ever added to the
# schema it must be added to the snapshot AND here, or restore_pristine_userdb
# will silently leave it contaminated.
USERDB_NAMES = ("private.userdb", "melt_eng.userdb")

DEFAULT_PRISTINE_DIR = Path.home() / ".local" / "share" / "rime-corpus" / "userdb-pristine"


def han_runs(text: str) -> list[tuple[str, str]]:
    """(context_before, run) for every maximal run of Han characters.

    The context is everything already committed, INCLUDING non-Chinese -- that
    is what gets pushed as surrounding text, and it is what the re-ranking
    filter reads (rerank_filter.cc calls TrailingCjkRun on it, which trims to
    the Han tail itself).
    """
    return [(text[: m.start()], m.group()) for m in _HAN_RUN.finditer(text)]


def build_requests(records: Iterable[dict], sp) -> list[dict]:
    """One replay request per maximal Han run in the corpus.

    Callers should materialize `records` once and pass the SAME list to every
    arm of a comparison: a second `corpus.iter_records()` walk is not
    guaranteed stable if the corpus file changes mid-run, and two arms seeing
    different request lists would make any delta between them meaningless.
    """
    from . import speller as _speller

    requests = []
    for record in records:
        for index, (ctx, run) in enumerate(han_runs(record["text"])):
            syllables = _speller.syllables(run)
            if len(syllables) != len(run):
                continue  # pypinyin could not read every character
            keys = [sp.keys(s) for s in syllables]
            if any(k is None for k in keys):
                continue  # a run we cannot spell must not be replayed short
            requests.append(
                {
                    "id": f"{record['id']}#{index}",
                    "ctx": ctx,
                    "keys": "".join(keys),
                    "text": run,
                }
            )
    return requests


def restore_pristine_userdb(rime_dir: Path, pristine_dir: Path) -> None:
    """Overwrite rime_dir's user dictionaries with the pristine snapshot.

    MUST be called before every arm of every measurement, and again
    afterward -- see the module docstring for why. The "afterward" restore
    matters as much as the "before" one: without it, a run left dirty
    silently contaminates whatever call comes next against this same
    rime_dir, including a second call to this same function (see
    `assert_deterministic`, which does exactly that).
    """
    rime_dir = Path(rime_dir)
    pristine_dir = Path(pristine_dir)
    for name in USERDB_NAMES:
        src = pristine_dir / name
        if not src.is_dir():
            raise FileNotFoundError(f"no pristine snapshot at {src}")
        dst = rime_dir / name
        if dst.exists():
            shutil.rmtree(dst)
        shutil.copytree(src, dst)


def run(
    requests: Sequence[dict],
    replayer: str,
    rime_dir: str,
    window: int | None = None,
    wait_for_warm: bool = False,
) -> list[dict]:
    """Drive replay_copilot over `requests`, once.

    Raises with the child's stderr attached on a non-zero exit, rather than
    letting CalledProcessError swallow it into "returned non-zero exit status
    N" -- a stalled or refused run (replay_copilot's own contamination /
    push-failure guards exit non-zero on purpose) must be LOUD, not a
    silently smaller sample.

    `wait_for_warm` forwards --wait-for-warm (replay_copilot.cc) -- see
    compare_rerank.py's own flag for what that measures and why it exists.
    Harmless to pass on an arm with no `copilot/rerank/llm` scorer at all
    (every wait becomes a no-op there), so callers may pass it uniformly
    across arms rather than tracking which one has the model configured.
    """
    payload = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in requests)
    argv = [str(replayer), "--rime-dir", str(rime_dir)]
    if window is not None:
        argv += ["--window", str(window)]
    if wait_for_warm:
        argv += ["--wait-for-warm"]
    result = subprocess.run(argv, input=payload, capture_output=True, text=True)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise RuntimeError(f"{replayer} exited {result.returncode}; see stderr above")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def run_arm(
    requests: Sequence[dict],
    replayer: str,
    rime_dir: str,
    pristine_dir: str,
    window: int | None = None,
    wait_for_warm: bool = False,
) -> list[dict]:
    """restore -> run -> restore: one full, uncontaminated measurement pass.

    This is the ONE place "restore before, restore after" is implemented;
    every caller that wants a trustworthy pass (the oracle bound, the
    determinism check, compare_rerank's on/off arms) goes through here rather
    than restoring by hand, so the guard cannot be forgotten at a new call
    site.
    """
    restore_pristine_userdb(rime_dir, pristine_dir)
    try:
        return run(requests, replayer, rime_dir, window, wait_for_warm)
    finally:
        restore_pristine_userdb(rime_dir, pristine_dir)


def strip_timings(responses: Iterable[dict]) -> list[dict]:
    """Responses with the per-segment `us` wall-clock fields removed.

    Every response carries `us` timings that differ between any two runs by
    definition, so a byte-for-byte comparison would fail on EVERY run, not
    just a genuinely non-deterministic one -- the plan this package started
    from specified exactly that broken comparison. Strip first, then compare.
    """
    out = []
    for r in responses:
        r = copy.deepcopy(r)
        for seg in r.get("segments", []):
            seg.pop("us", None)
        out.append(r)
    return out


def assert_deterministic(
    requests: Sequence[dict],
    replayer: str,
    rime_dir: str,
    pristine_dir: str,
    window: int | None = None,
    wait_for_warm: bool = False,
) -> list[dict]:
    """Sentinel 4: the same configuration, run twice, must produce identical
    (timing-stripped) output. Raises AssertionError naming the first
    divergence if not; returns the first pass's responses if so.

    Each of the two passes is its own full `run_arm` (restore-run-restore) --
    Correction 3 (see module docstring) applies to this check exactly as much
    as to the "real" run, because it IS a real run. Reusing the first pass's
    responses as the caller's actual measurement (rather than paying for a
    third full pass) is valid precisely because a positive determinism result
    is what licenses treating either run as representative.

    Replay drives ONE Rime instance across thousands of segments; any state
    that fails to clear between them -- a leftover composition, commit
    history, stale bridge context -- makes results drift. The danger is
    directional: leaked context from the previous segment is often RELEVANT
    to the next, so re-ranking guesses right and the harness cheats in its
    own favour. A quietly cheating evaluator reports a positive delta, which
    is the result nobody questions. Irreproducibility is the only symptom
    such a bug reliably shows.
    """
    first = run_arm(requests, replayer, rime_dir, pristine_dir, window, wait_for_warm)
    second = run_arm(requests, replayer, rime_dir, pristine_dir, window, wait_for_warm)
    stripped_first = strip_timings(first)
    stripped_second = strip_timings(second)
    if stripped_first != stripped_second:
        by_id = {r.get("id"): r for r in stripped_second}
        for a in stripped_first:
            b = by_id.get(a.get("id"))
            if a != b:
                raise AssertionError(
                    f"replay is not deterministic; first divergence at {a.get('id')}:\n"
                    f"  run 1: {json.dumps(a, ensure_ascii=False)}\n"
                    f"  run 2: {json.dumps(b, ensure_ascii=False)}"
                )
        raise AssertionError("replay is not deterministic: different result counts or ids")
    return first
