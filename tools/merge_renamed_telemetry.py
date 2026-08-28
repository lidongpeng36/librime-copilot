#!/usr/bin/env python3
"""Fold a telemetry file written under a stale machine name into the current one.

Until 6fd7b49 the telemetry filename went stale on a rename. The name reaches
the log twice and only one site re-read it: `GetTelemetryWriter` read
`deployer.user_id` once, when it built the process-wide `Writer` whose path is
derived from it, while `EmitCommitTelemetry` re-read it per commit for the
line's own `machine` field. So a machine whose `installation_id` changed
without a Squirrel restart went on appending to the OLD file while stamping
every line with the NEW name, until the process was restarted. Any machine
renamed before that fix has one of these files.

WHICH COPY TO REPAIR. The shared `<sync_dir>/copilot_telemetry/` copy is a
PROJECTION of the machine-local one: every sync -- `sync_telemetry.sh` or the
plugin's `auto_sync` -- overwrites it wholesale. Repairing the shared copy
therefore lasts exactly until the next sync. This tool defaults to the local
directory for that reason, and refuses a destination that looks like a sync
directory unless you insist.

WHY SQUIRREL MUST NOT BE RUNNING. `telemetry::Writer` holds an open fd and
appends through it. Replacing the file by rename leaves that fd pointing at
the orphaned inode, so every subsequent commit is written somewhere nothing
will ever read -- silently, until the process restarts. This tool refuses to
run while Squirrel is up rather than trusting the operator to remember.

Idempotent: a line already carrying `machine_was` has been folded before and
the run is refused rather than doubling the file.
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


def rewrite(lines, stale_name, current_name):
    """[(json line, ...)] -> lines with `machine` corrected, provenance kept.

    `machine_was` is what makes the correction legible and reversible from the
    merged file alone -- the alternative, a silent rewrite, leaves a reader no
    way to tell corrected data from data that was always right. The analyser
    reads named keys and ignores the rest, so it costs nothing there.

    Lines already carrying the CURRENT name are passed through untouched: a
    stale file holds both, because the flip happened mid-file.
    """
    out = []
    corrected = 0
    for line in lines:
        e = json.loads(line)
        if e.get("machine") == stale_name:
            e["machine"] = current_name
            e["machine_was"] = stale_name
            out.append(json.dumps(e, ensure_ascii=False))
            corrected += 1
        else:
            out.append(line)
    return out, corrected


def read_lines(path):
    return [ln.rstrip("\n") for ln in path.read_text(encoding="utf-8").splitlines() if ln.strip()]


def last_ts(lines):
    for line in reversed(lines):
        ts = json.loads(line).get("ts")
        if isinstance(ts, str):
            return ts
    return None


def first_ts(lines):
    for line in lines:
        ts = json.loads(line).get("ts")
        if isinstance(ts, str):
            return ts
    return None


def plan(stale_lines, current_lines, stale_name, current_name):
    """Refusals first, then the merged content. Returns (lines, corrected).

    Raises ValueError on anything that would lose or duplicate data. The
    ordering check is not pedantry: these two files are one machine's log split
    by a restart, so the stale one ends where the current one begins, and an
    overlap means the input is not what this tool is for.
    """
    if any("machine_was" in json.loads(ln) for ln in current_lines):
        raise ValueError(f"{current_name} already carries `machine_was` -- already merged")
    both = set(stale_lines) & set(current_lines)
    if both:
        raise ValueError(f"{len(both)} identical line(s) in both files -- refusing to duplicate")
    end, start = last_ts(stale_lines), first_ts(current_lines)
    if end and start and end > start:
        raise ValueError(
            f"time ranges overlap: {stale_name} runs to {end}, {current_name} starts {start}")
    merged, corrected = rewrite(stale_lines, stale_name, current_name)
    # Nothing relabelled while the stale file plainly names some OTHER machine
    # means `stale_name` is wrong -- most often because the file was renamed
    # out of the way and no longer ends in `.jsonl`, so deriving the name from
    # the filename yields "X.jsonl" and matches nothing. Merging anyway would
    # produce a file that looks right and still attributes those lines to the
    # machine that never typed them, which is the exact failure being repaired.
    names = {json.loads(ln).get("machine") for ln in stale_lines} - {None}
    if corrected == 0 and names - {current_name}:
        raise ValueError(
            f"nothing matched `{stale_name}`; the stale file names "
            f"{sorted(names)} -- pass --stale-name with the right one")
    return merged + current_lines, corrected


def squirrel_running():
    return subprocess.run(["pgrep", "-x", "Squirrel"],
                          capture_output=True).returncode == 0


def main(argv=None):
    p = argparse.ArgumentParser(description=__doc__,
                                formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("stale", type=Path, help="the file written under the old name")
    p.add_argument("current", type=Path, help="this machine's current file")
    p.add_argument("--dry-run", action="store_true")
    p.add_argument("--stale-name", help="the old machine name, when it cannot be "
                                        "read off the filename (e.g. a file renamed "
                                        "out of the way, so it no longer ends .jsonl)")
    p.add_argument("--allow-running-squirrel", action="store_true",
                   help="skip the fd check; see this module's docstring for what it costs")
    a = p.parse_args(argv)

    if not a.dry_run and not a.allow_running_squirrel and squirrel_running():
        print("Squirrel is running. Its Writer holds an open fd on the current file,\n"
              "and replacing that file would leave every later commit written to an\n"
              "orphaned inode -- silently, until the process restarts.\n"
              "  killall Squirrel   # then run this, then reopen it", file=sys.stderr)
        return 1
    for f in (a.stale, a.current):
        if not f.is_file():
            print(f"no {f}", file=sys.stderr)
            return 1
    if "copilot_telemetry" in str(a.current) and "sync" in str(a.current.parent.parent):
        print(f"{a.current} looks like the SHARED copy. That is a projection of the\n"
              "machine-local file and the next sync overwrites it. Repair\n"
              "~/Library/Rime/private/copilot_telemetry/ instead.", file=sys.stderr)
        return 1

    stale_name = a.stale_name or (
        a.stale.name[:-len(".jsonl")] if a.stale.name.endswith(".jsonl") else a.stale.stem)
    current_name = a.current.stem
    try:
        merged, corrected = plan(read_lines(a.stale), read_lines(a.current),
                                 stale_name, current_name)
    except ValueError as e:
        print(f"refusing: {e}", file=sys.stderr)
        return 1

    print(f"{a.stale.name} -> {a.current.name}")
    print(f"  {len(merged)} lines after merge, {corrected} relabelled {stale_name} -> {current_name}")
    if a.dry_run:
        print("  (dry run, nothing written)")
        return 0

    # Written to a sibling and renamed: the destination may be iCloud-backed,
    # and a half-written telemetry log is worse than an unmerged one.
    tmp = a.current.with_suffix(".jsonl.tmp")
    tmp.write_text("".join(ln + "\n" for ln in merged), encoding="utf-8")
    os.chmod(tmp, 0o600)  # a transcript of the user's own typing
    os.replace(tmp, a.current)
    backup = a.stale.with_name(a.stale.name + ".merged")
    os.rename(a.stale, backup)
    print(f"  wrote {a.current}")
    print(f"  kept  {backup}")
    print("\nNow restart Squirrel (its fd points at the replaced file) and sync:")
    print("  open -a Squirrel && <repo>/tools/sync_telemetry.sh")
    return 0


if __name__ == "__main__":
    sys.exit(main())
