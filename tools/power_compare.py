#!/usr/bin/env python3
"""Energy per scoring, llama.cpp against MLX, with the sampler in the loop.

Latency and energy are different questions and this tree has already been wrong
about the second by reasoning from the first: `cpu_ms_per_iter` cannot answer it
because both backends run at cpu/wall ~0.4, i.e. most of the work is on the GPU
where process CPU time sees nothing. bench_scorer.cc has said to pair it with
powermetrics since it was written; this is that pairing, automated so the
numbers are not aligned by hand.

WHY IT STARTS A SAMPLER PER PHASE rather than one long run. Matching wall-clock
timestamps between powermetrics' output and a benchmark's phases is exactly the
kind of manual join that produces a confident wrong answer -- and powermetrics'
own start latency is unknown, so an offset would have to be guessed. One
invocation per phase means each output file contains only that phase's samples
and there is nothing to align.

WHY IT RUNS THE ARMS TWICE, IN BOTH ORDERS. This machine drifts: the identical
llama.cpp binary measured 9.17 ms and then 10.30 ms half an hour apart. A
thermal ramp during the run would otherwise be charged entirely to whichever
arm went second.

WHY IT MEASURES AN IDLE BASELINE. Absolute package power is mostly the display,
the window server and whatever else is running. Only the increment over idle is
attributable, and the arms differ by less than the baseline is worth.

Run it with sudo -- powermetrics requires root. The load itself is dropped back
to the invoking user, so the thing being measured runs at the scheduling
priority it would have in real use rather than as root:

    sudo tools/power_compare.py --model ~/Library/Rime/private/rime40m-v2-q8.gguf
"""

import argparse
import json
import os
import re
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BIN = REPO.parents[1] / "build/plugins/copilot/bin/bench_backends"

# powermetrics prints one block per sample. "Combined Power" is not on every
# machine, so CPU + GPU is the fallback -- and the two are reported separately
# on purpose, because the backends put different shares of the work on each and
# a CPU-only reading would flatter whichever uses the GPU more.
PAT = {
    "cpu_mw": re.compile(r"^CPU Power:\s+([0-9.]+)\s*mW", re.M),
    "gpu_mw": re.compile(r"^GPU Power:\s+([0-9.]+)\s*mW", re.M),
    "combined_mw": re.compile(r"^Combined Power \([^)]*\):\s+([0-9.]+)\s*mW", re.M),
}


def parse_samples(text):
    """[{cpu_mw, gpu_mw, combined_mw}] -- one entry per sample block."""
    cpu = [float(x) for x in PAT["cpu_mw"].findall(text)]
    gpu = [float(x) for x in PAT["gpu_mw"].findall(text)]
    comb = [float(x) for x in PAT["combined_mw"].findall(text)]
    out = []
    for i in range(min(len(cpu), len(gpu))):
        c = comb[i] if i < len(comb) else cpu[i] + gpu[i]
        out.append({"cpu_mw": cpu[i], "gpu_mw": gpu[i], "combined_mw": c})
    return out


def summarize(samples, drop_first=2):
    """Median over the samples, with the first few dropped.

    Dropped rather than averaged in: powermetrics' first sample covers the
    interval since it started, which contains its own startup and, for a load
    phase, the part of the window before the load began.
    """
    use = samples[drop_first:] or samples
    if not use:
        return None
    return {k: statistics.median([s[k] for s in use]) for k in ("cpu_mw", "gpu_mw", "combined_mw")}


def run_phase(name, seconds, interval_ms, load_cmd, as_user):
    """Sample power for `seconds` while `load_cmd` runs (None for the idle baseline).

    powermetrics is started FIRST and asked for enough samples to outlast the
    load, so the load is entirely inside the sampling window; it is then killed
    rather than waited for.
    """
    n = int(seconds * 1000 / interval_ms) + 4
    with tempfile.NamedTemporaryFile("w+", suffix=".txt", delete=False) as f:
        path = f.name
    sampler = subprocess.Popen(
        ["powermetrics", "--samplers", "cpu_power,gpu_power", "-i", str(interval_ms), "-n", str(n)],
        stdout=open(path, "w"), stderr=subprocess.DEVNULL)
    # Let the first sample land before the load starts, so the load never
    # straddles the sampler's own startup.
    time.sleep(interval_ms / 1000.0 * 2)
    load_out, wall = "", 0.0
    if load_cmd:
        cmd = load_cmd
        if as_user:
            cmd = ["sudo", "-u", as_user] + load_cmd
        t0 = time.time()
        p = subprocess.run(cmd, capture_output=True, text=True)
        wall = time.time() - t0
        load_out = p.stdout
        if p.returncode != 0:
            sampler.kill()
            raise RuntimeError(f"{name}: load failed ({p.returncode}): {p.stderr[-600:]}")
    else:
        time.sleep(seconds)
    sampler.terminate()
    try:
        sampler.wait(timeout=10)
    except subprocess.TimeoutExpired:
        sampler.kill()
    text = Path(path).read_text(errors="replace")
    os.unlink(path)
    samples = parse_samples(text)
    if not samples:
        raise RuntimeError(f"{name}: powermetrics produced no samples -- is this running as root?")
    return {"name": name, "samples": len(samples), "power": summarize(samples),
            "wall_s": wall, "stdout": load_out}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--model", default=str(Path.home() / "Library/Rime/private/rime40m-v2-q8.gguf"))
    ap.add_argument("--bin", default=str(DEFAULT_BIN))
    ap.add_argument("--iters", type=int, default=150, help="scorings per arm per round")
    ap.add_argument("--idle-ms", type=int, default=100,
                    help="gap before each scoring. NOT zero: an IME scores once per keystroke, "
                         "and a back-to-back loop measures a duty cycle no user is ever in")
    ap.add_argument("--interval-ms", type=int, default=200, help="powermetrics sampling interval")
    ap.add_argument("--rounds", type=int, default=2,
                    help="each round runs both arms; odd rounds reverse the order")
    ap.add_argument("--json", help="also write the raw result here")
    args = ap.parse_args()

    if os.geteuid() != 0:
        print("powermetrics needs root; re-run with sudo", file=sys.stderr)
        return 1
    if not Path(args.bin).is_file():
        print(f"no {args.bin} -- build with -DCOPILOT_WITH_MLX=ON", file=sys.stderr)
        return 1
    if not Path(args.model).is_file():
        print(f"no model at {args.model}", file=sys.stderr)
        return 1
    as_user = os.environ.get("SUDO_USER")

    base = [args.bin, "--model", args.model, "--iters", str(args.iters),
            "--idle-ms", str(args.idle_ms), "--rounds", "1"]

    # Verify before measuring, the same rule bench_backends applies to latency:
    # an energy comparison between two things computing different numbers is
    # not a comparison either.
    print("checking the two backends agree before measuring anything ...")
    check = subprocess.run(
        ([ "sudo", "-u", as_user] if as_user else []) +
        [args.bin, "--model", args.model, "--iters", "3", "--rounds", "1", "--idle-ms", "0",
         "--json"], capture_output=True, text=True)
    if check.returncode != 0:
        print(check.stderr[-800:], file=sys.stderr)
        return 2
    agreement = json.loads([l for l in check.stdout.splitlines() if l.startswith("{")][0])
    print(f"  worst |diff| = {agreement['agreement']:.5f} nats\n")

    # A load phase lasts about iters * (idle_ms + score_ms); the score term is
    # small next to the idle, so this is close enough to size the sampler.
    seconds = args.iters * (args.idle_ms + 12) / 1000.0

    phases = [run_phase("idle", 8, args.interval_ms, None, as_user)]
    for r in range(args.rounds):
        order = ["llama", "mlx"] if r % 2 == 0 else ["mlx", "llama"]
        for arm in order:
            print(f"round {r + 1}: {arm} ({args.iters} scorings, ~{seconds:.0f}s) ...", flush=True)
            phases.append(run_phase(arm, seconds, args.interval_ms, base + ["--only", arm],
                                    as_user))

    idle = phases[0]["power"]
    print(f"\nidle baseline over {phases[0]['samples']} samples: "
          f"cpu {idle['cpu_mw']:.0f} mW, gpu {idle['gpu_mw']:.0f} mW, "
          f"combined {idle['combined_mw']:.0f} mW")
    print(f"\n{'arm':>8} {'round':>6} {'cpu mW':>9} {'gpu mW':>9} {'combined':>9} "
          f"{'over idle':>10} {'wall s':>8} {'mJ/scoring':>11}")
    per_arm = {}
    for i, p in enumerate(phases[1:]):
        w = p["power"]
        over = w["combined_mw"] - idle["combined_mw"]
        # Energy for the WORK, not for the wall clock: the machine would have
        # burned the idle baseline anyway, and both arms do the same number of
        # scorings, so this is the number that decides the question.
        mj = over * p["wall_s"] / args.iters
        print(f"{p['name']:>8} {i // 2 + 1:>6} {w['cpu_mw']:>9.0f} {w['gpu_mw']:>9.0f} "
              f"{w['combined_mw']:>9.0f} {over:>10.0f} {p['wall_s']:>8.1f} {mj:>11.2f}")
        per_arm.setdefault(p["name"], []).append(mj)
    print()
    if len(per_arm) == 2 and all(len(v) >= 1 for v in per_arm.values()):
        a, b = statistics.median(per_arm["llama"]), statistics.median(per_arm["mlx"])
        spread = {k: (max(v) - min(v)) / statistics.median(v) * 100 if len(v) > 1 else 0.0
                  for k, v in per_arm.items()}
        print(f"  energy per scoring, over idle:  llama.cpp {a:.2f} mJ   mlx {b:.2f} mJ"
              f"   ({(b - a) / a * 100:+.1f}%)")
        print(f"  round-to-round spread:          llama.cpp {spread['llama']:.0f}%"
              f"   mlx {spread['mlx']:.0f}%")
        if abs((b - a) / a * 100) <= spread["llama"] + spread["mlx"]:
            print("\n  !! the difference does not clear the spread. More rounds, or the two")
            print("     backends cost the same energy -- do not report this as a win either way.")
    if args.json:
        Path(args.json).write_text(json.dumps(
            {"idle": idle, "phases": [{k: v for k, v in p.items() if k != "stdout"}
                                      for p in phases], "agreement": agreement["agreement"]},
            indent=2))
        print(f"\nwrote {args.json}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
