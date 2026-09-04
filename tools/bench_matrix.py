#!/usr/bin/env python3
"""Run tools/bench_scorer over a matrix of configurations, and diff two runs.

WHY THIS IS COMMITTED rather than a shell loop someone retypes. This repository
is largely a list of measurements, several of which later turned out to be
wrong, and an unreproducible one cannot be re-examined when the next one
contradicts it -- the same argument that made rime_corpus/compare_warmed.py a
module. Two properties matter more here than the table it prints:

  * It records what the numbers are ABOUT. A latency figure without the
    llama.cpp tag, the model's content hash and this repo's commit is not
    comparable with anything, and the question this tool exists for ("did the
    bump help?") is exactly a comparison across those.
  * It runs the conditions that matter, not the convenient one. Every figure in
    bench_scorer.cc's own header was measured back-to-back (--idle-ms 0), which
    is the single condition under which the dominant cost cannot appear: an
    idle of 50 ms costs ~6x, saturates immediately, and is what the deployed
    scorer lives in. A matrix that omits it reproduces the original mistake.

Usage:
    tools/bench_matrix.py run --model ~/Library/Rime/private/rime40m-v2-q8.gguf \\
                              --out before.json
    # ... change something, rebuild ...
    tools/bench_matrix.py run --model ... --out after.json
    tools/bench_matrix.py compare before.json after.json
"""

import argparse
import hashlib
import json
import os
import platform
import re
import shutil
import subprocess
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(HERE)

# One row per (condition, workload) pair worth telling apart, chosen from what
# the live log actually contains rather than from what is quick to run:
#
#   idle 0    the historical condition, kept ONLY so old numbers stay
#             comparable. Nothing deployed ever runs here.
#   idle 100  the deployed condition. 50 ms already saturates the penalty and
#             a keystroke gap is never shorter, so this stands for all of it.
#   cand 1    the decode-free mode -- every candidate a single token, scored
#             off the prefill's own last logits. 44% of live scorings, and
#             ~0.2 ms even cold.
#   cand 2    the decode-bearing mode, the other 56%.
#
# `iters` is per row because a row with an idle costs iters * idle_ms of wall
# time and would otherwise dominate the run.
DEFAULT_MATRIX = (
    {"name": "hot/decode",     "idle_ms": 0,   "candidate_chars": 2, "iters": 200},
    {"name": "hot/nodecode",   "idle_ms": 0,   "candidate_chars": 1, "iters": 200},
    {"name": "idle/decode",    "idle_ms": 100, "candidate_chars": 2, "iters": 80},
    {"name": "idle/nodecode",  "idle_ms": 100, "candidate_chars": 1, "iters": 80},
    {"name": "idle/decode/64", "idle_ms": 100, "candidate_chars": 2, "iters": 80,
     "context_chars": 64},
)

# What a comparison prints, and which direction is an improvement. Everything
# here is a cost, so lower is better -- stated rather than assumed, because a
# future counter (tokens/second, say) would invert and a hard-coded sign would
# then quietly report a regression as a win.
METRICS = (
    ("score_p50_ms", "score p50"),
    ("score_p99_ms", "score p99"),
    ("prefill_p50_ms", "prefill p50"),
    ("cpu_ms_per_iter", "cpu/iter"),
)

# A floor, and ONLY a floor. It is not the error bar -- the error bar is
# measured per row, per run, from `--repeats`, because it is not a constant:
#
# The first version of this file marked anything past a fixed 8%. Run against
# two executions of the IDENTICAL build, minutes apart, it marked 14 of 20
# metrics as changes, the largest at -40.7% -- the `idle/*` rows swing that far
# on their own because what they measure is the machine's power state, not the
# code. A tool that reports a llama.cpp bump as a 30% win from that is worse
# than no tool, and this is the class of measurement this repository is
# otherwise a catalogue of.
#
# So a difference is called a change only when it clears BOTH this floor and
# the spread actually observed across repeats of the two runs being compared.
NOISE_PCT = 8.0

# Repeats per matrix row. Three is the minimum that yields a spread at all;
# the idle rows want more, and `compare` says so when the spread it measured
# is wide enough to swallow the difference.
DEFAULT_REPEATS = 3


def median(values):
    xs = sorted(values)
    n = len(xs)
    if n == 0:
        return None
    mid = n // 2
    return xs[mid] if n % 2 else (xs[mid - 1] + xs[mid]) / 2.0


def spread_pct(values):
    """(max - min) / median, as a percentage. 0.0 for a single sample.

    Full range rather than a standard deviation: with three samples an sd is
    not meaningful, and the question here is "could this difference be nothing",
    for which the worst case observed is the honest answer.
    """
    if len(values) < 2:
        return 0.0
    m = median(values)
    if not m:
        return 0.0
    return (max(values) - min(values)) / m * 100.0


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def llama_tag(repo=REPO):
    """The GIT_TAG this checkout's CMakeLists pins llama.cpp to.

    Read from the file rather than from the built _deps tree: the tree is a
    build artifact that may predate an edit to CMakeLists, and the question a
    comparison asks is which version the binary being measured was built
    against. A mismatch between the two is exactly the "you did not rebuild"
    case, and reporting the file's value makes it visible in the diff.
    """
    try:
        with open(os.path.join(repo, "CMakeLists.txt"), encoding="utf-8") as f:
            text = f.read()
    except OSError:
        return None
    # `[\w.-]+`, not `\S+`: FetchContent_Declare closes on the same line as the
    # tag more often than not (`GIT_TAG b10456)`), and a greedy non-space run
    # swallows the paren -- which then reads as a different llama.cpp version
    # from the identical one on a checkout that happened to format it with the
    # paren on its own line.
    m = re.search(r"llama.*?GIT_TAG\s+([\w.-]+)", text, re.S)
    return m.group(1) if m else None


def git_head(repo=REPO):
    try:
        out = subprocess.run(["git", "-C", repo, "rev-parse", "--short", "HEAD"],
                             capture_output=True, text=True, timeout=10)
        return out.stdout.strip() or None
    except (OSError, subprocess.SubprocessError):
        return None


def git_dirty(repo=REPO):
    """Whether the tree had uncommitted changes when the run was made.

    Recorded because a dirty measurement is not reproducible from its commit,
    and a comparison that silently pairs a clean `before` with a dirty `after`
    attributes the difference to the commit rather than to whatever was in the
    working tree.
    """
    try:
        out = subprocess.run(["git", "-C", repo, "status", "--porcelain"],
                             capture_output=True, text=True, timeout=10)
        return bool(out.stdout.strip())
    except (OSError, subprocess.SubprocessError):
        return None


def find_bench(explicit=None):
    if explicit:
        return explicit
    for rel in ("../../build/plugins/copilot/bin/bench_scorer",
                "../build/plugins/copilot/bin/bench_scorer"):
        p = os.path.normpath(os.path.join(REPO, rel))
        if os.path.isfile(p) and os.access(p, os.X_OK):
            return p
    return shutil.which("bench_scorer")


def run_one(bench, model, row, extra_args=()):
    """One bench_scorer invocation, returning its parsed JSON object.

    --json rather than parsing the prose: the prose is written for a human
    reading one run and its wording is not a contract, while every field below
    is keyed by name.
    """
    cmd = [bench, "--model", model, "--json",
           "--iters", str(row["iters"]),
           "--idle-ms", str(row["idle_ms"]),
           "--candidate-chars", str(row["candidate_chars"])]
    if "context_chars" in row:
        cmd += ["--context-chars", str(row["context_chars"])]
    if "candidates" in row:
        cmd += ["--candidates", str(row["candidates"])]
    if "threads" in row:
        cmd += ["--threads", str(row["threads"])]
    if "gpu_layers" in row:
        cmd += ["--gpu-layers", str(row["gpu_layers"])]
    cmd += list(extra_args)
    proc = subprocess.run(cmd, capture_output=True, text=True)
    if proc.returncode != 0:
        raise RuntimeError(f"bench_scorer failed ({proc.returncode}): {proc.stderr.strip()[:400]}")
    return parse_bench_output(proc.stdout)


def parse_bench_output(stdout):
    """The one JSON object in bench_scorer's stdout.

    Scans for it rather than calling json.loads on the whole stream: llama.cpp
    installs its own logger before this tool can silence it, and a stray line
    from a future version would otherwise turn a good run into a parse error.
    """
    for line in stdout.splitlines():
        line = line.strip()
        if line.startswith("{") and line.endswith("}"):
            return json.loads(line)
    raise RuntimeError("no JSON object in bench_scorer output")


def cmd_run(args):
    bench = find_bench(args.bench)
    if not bench:
        print("bench_scorer not found; pass --bench or build it "
              "(cmake --build build --target bench_scorer)", file=sys.stderr)
        return 1
    if not os.path.isfile(args.model):
        print(f"no model at {args.model}", file=sys.stderr)
        return 1
    rows = []
    for row in DEFAULT_MATRIX:
        print(f"  {row['name']:16s} iters={row['iters']:<4d} idle={row['idle_ms']:<4d} "
              f"cand_chars={row['candidate_chars']} ...", end="", flush=True)
        samples = [run_one(bench, args.model, row, args.bench_arg)
                   for _ in range(args.repeats)]
        # The median repeat's own object carries the configuration echo
        # (decoded_per_iter and friends); the per-metric medians and spreads
        # are folded in beside it, so a result file is readable either way.
        result = dict(samples[0])
        result["name"] = row["name"]
        result["repeats"] = args.repeats
        result["samples"] = [{k: s_[k] for _, k in ((None, m) for m, _ in METRICS)}
                             for s_ in samples]
        for key, _ in METRICS:
            values = [s_[key] for s_ in samples]
            result[key] = median(values)
            result[key + "_spread_pct"] = spread_pct(values)
        rows.append(result)
        print(f" score p50 {result['score_p50_ms']:.2f} ms "
              f"+/-{result['score_p50_ms_spread_pct']:.0f}% "
              f"({result['decoded_per_iter']:.0f} tok decoded)")
    out = {
        "created": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "label": args.label,
        "machine": platform.node(),
        "platform": f"{platform.system()} {platform.release()} {platform.machine()}",
        "bench": bench,
        "model": args.model,
        "model_sha256": sha256_of(args.model),
        "llama_tag": llama_tag(),
        "git_head": git_head(),
        "git_dirty": git_dirty(),
        "repeats": args.repeats,
        "rows": rows,
    }
    with open(args.out, "w", encoding="utf-8") as f:
        json.dump(out, f, indent=2)
        f.write("\n")
    print(f"wrote {args.out}")
    return 0


def provenance_diff(before, after):
    """[(field, before, after)] for everything that must match for a comparison
    to mean what it looks like. Returned rather than printed so the caller can
    decide how loud to be, and so a test can assert on it."""
    out = []
    for field in ("machine", "platform", "model_sha256", "llama_tag", "git_head", "git_dirty"):
        b, a = before.get(field), after.get(field)
        if b != a:
            out.append((field, b, a))
    return out


def is_change(pct, spread_before, spread_after):
    """Whether a difference is big enough to be called one.

    BOTH conditions, and the second is the one that matters: a fixed floor
    alone marked 14 of 20 metrics as changes across two runs of the identical
    build (see NOISE_PCT). `idle/*` rows measure the machine's power state as
    much as the code, and their own repeat-to-repeat range is routinely wider
    than any effect worth shipping.
    """
    if pct is None:
        return False
    return abs(pct) >= NOISE_PCT and abs(pct) > (spread_before + spread_after)


def compare_rows(before, after):
    """[(name, label, before, after, pct, spread_before, spread_after)] by row name.

    Rows present in only one file are skipped rather than compared against
    zero, and the caller is told which -- a matrix that changed between the two
    runs is a reason to re-run, not to diff what happens to overlap.

    The spreads come from `--repeats` and are 0.0 in a file written before
    repeats existed, or by `--repeats 1`. That is not "no noise": it is "no
    measurement of the noise", and is_change then falls back to the floor
    alone, which is known to be too generous. `compare` says so.
    """
    b_by_name = {r["name"]: r for r in before.get("rows", [])}
    a_by_name = {r["name"]: r for r in after.get("rows", [])}
    out = []
    for name in [r["name"] for r in before.get("rows", [])]:
        if name not in a_by_name:
            continue
        for key, label in METRICS:
            b = b_by_name[name].get(key)
            a = a_by_name[name].get(key)
            if b is None or a is None:
                continue
            pct = ((a - b) / b * 100.0) if b else None
            out.append((name, label, b, a, pct,
                        b_by_name[name].get(key + "_spread_pct", 0.0) or 0.0,
                        a_by_name[name].get(key + "_spread_pct", 0.0) or 0.0))
    return out


def cmd_compare(args):
    with open(args.before, encoding="utf-8") as f:
        before = json.load(f)
    with open(args.after, encoding="utf-8") as f:
        after = json.load(f)

    print(f"before  {before.get('label') or args.before}   {before.get('created')}")
    print(f"after   {after.get('label') or args.after}   {after.get('created')}")
    drift = provenance_diff(before, after)
    dirty = bool(before.get("git_dirty")) or bool(after.get("git_dirty"))
    if drift:
        print("\n  what differs between the two runs besides the numbers:")
        for field, b, a in drift:
            print(f"    {field:14s} {b}  ->  {a}")
    elif dirty:
        # `provenance_diff` compares the recorded commit, and two runs of a
        # DIRTY tree record the same one while measuring different source.
        # That is the normal way this tool is used -- edit, rebuild, re-run,
        # all before committing -- so saying "same commit, therefore noise"
        # here would call every real result an artifact. It said exactly that
        # on the first change it was used for.
        print("\n  same machine, model and llama.cpp tag, and the SAME COMMIT --")
        print("  but both runs were made from a dirty tree, so that says nothing")
        print("  about whether the source was the same. Name what changed in --label.")
    else:
        print("\n  same machine, model, llama.cpp tag and commit, both trees clean --")
        print("  so there is nothing here that should have changed the numbers.")
    if any(f == "git_dirty" for f, _, _ in drift):
        print("    (a dirty tree is not reproducible from its commit)")

    only_before = ({r["name"] for r in before.get("rows", [])}
                   - {r["name"] for r in after.get("rows", [])})
    only_after = ({r["name"] for r in after.get("rows", [])}
                  - {r["name"] for r in before.get("rows", [])})
    if only_before or only_after:
        print(f"\n  !! the matrix changed; rows compared are the overlap only.")
        for n in sorted(only_before):
            print(f"     only in before: {n}")
        for n in sorted(only_after):
            print(f"     only in after:  {n}")

    rows = compare_rows(before, after)
    measured_spread = any(sb or sa for *_, sb, sa in rows)
    print()
    print(f"  {'row':16s} {'metric':13s} {'before':>9s} {'after':>9s} "
         f"{'change':>9s}  {'noise':>7s}")
    last = None
    for name, label, b, a, pct, sb, sa in rows:
        shown = name if name != last else ""
        last = name
        mark = ""
        if is_change(pct, sb, sa):
            mark = "  <-- better" if pct < 0 else "  <-- WORSE"
        pct_s = "     --" if pct is None else f"{pct:+8.1f}%"
        print(f"  {shown:16s} {label:13s} {b:9.2f} {a:9.2f} {pct_s}  "
             f"{sb + sa:6.0f}%{mark}")
    print()
    print("  Lower is better for every metric. `noise` is the two runs' own")
    print("  repeat-to-repeat ranges added together, and a difference is called a")
    print(f"  change only when it clears BOTH that and a {NOISE_PCT:.0f}% floor.")
    if not measured_spread:
        print()
        print("  !! neither run measured its own noise (--repeats 1, or a file written")
        print("     before repeats existed), so only the floor applied. That floor alone")
        print("     marked 14 of 20 metrics across two runs of the IDENTICAL build when")
        print("     this tool was first used. Re-run both with --repeats before believing")
        print("     anything marked above.")
    return 0


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)

    r = sub.add_parser("run", help="run the matrix and write a JSON result file")
    r.add_argument("--model", required=True)
    r.add_argument("--out", required=True)
    r.add_argument("--bench", help="path to bench_scorer (default: look in the build tree)")
    r.add_argument("--label", help="what this run is OF, e.g. 'llama b10796'")
    r.add_argument("--repeats", type=int, default=DEFAULT_REPEATS,
                   help="executions per matrix row (default %(default)s). The spread "
                        "across them is what `compare` uses to decide whether a "
                        "difference is a change; with 1 there is none and nothing "
                        "can be called.")
    r.add_argument("--bench-arg", action="append", default=[],
                   help="extra flag passed through to every bench_scorer invocation")
    r.set_defaults(func=cmd_run)

    c = sub.add_parser("compare", help="diff two result files")
    c.add_argument("before")
    c.add_argument("after")
    c.set_defaults(func=cmd_compare)

    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
