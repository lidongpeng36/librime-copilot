#!/usr/bin/env python3
"""Merge copilot telemetry from every machine and report on re-ranking quality.

Each section answers one claim from
docs/superpowers/specs/2026-08-14-prediction-telemetry-design.md. A flat
rejection rate across a section's buckets refutes that section's claim.

Usage (one machine, straight from the live directory):
    tools/analyze_telemetry.py ~/Library/Rime/private/copilot_telemetry/*.jsonl

Across machines, run tools/sync_telemetry.sh on each first; it prints the exact
command for the merged directory, which is the shared sync dir named in
~/Library/Rime/installation.yaml, e.g.:
    tools/analyze_telemetry.py ~/Library/Mobile\\ Documents/.../copilot_telemetry/*.jsonl
"""

import argparse
import json
import sqlite3
import sys

SCHEMA = """
CREATE TABLE ev (
  ts TEXT, machine TEXT, schema TEXT, src TEXT,
  input TEXT, ctx TEXT, sel_idx INT, sel TEXT, top0 TEXT,
  rr INT,            -- 1 when re-ranking promoted something
  rr_key TEXT, rr_key_len INT, rr_n INT, rr_text TEXT,
  rr_from INT, rr_rank INT, rr_level INT,
  verdict TEXT,      -- accepted | rejected | nomove | misrank
  head_altered INT   -- 1 when the displayed head is not what re-ranking put
                     -- first, so `sel` cannot be compared with `rr.text`
);
"""

HEAD_ALTERED_NOTE = """\
  What this means: `rr.text` is captured inside copilot_rerank_filter, at the
  moment it promotes a candidate. `sel` and `top` are read at commit, from the
  list as displayed. In these events the displayed list does not start with what
  re-ranking put first, so something between those two points rewrote or
  reordered the head — and whether the user accepted or rejected the promotion
  can no longer be read off `sel`. They are excluded from every table below.

  The cause is a filter installed AFTER copilot_rerank_filter in
  engine/filters: one that rewrites candidate text (simplifier@traditionalize —
  the moment the 繁 switch is on, every event is altered) or one that reorders
  or removes candidates (pin_cand_filter, uniquifier). This check names none of
  them; any such filter, including one added later, lands here.

  To get a clean read: collect with those filters off, or move
  copilot_rerank_filter after them — noting that the order is deliberate (the
  README puts re-ranking ahead of pinning so pinned candidates win).
"""


def load(paths):
    db = sqlite3.connect(":memory:")
    db.executescript(SCHEMA)
    skipped = 0
    for path in paths:
        with open(path, encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line:
                    continue
                # A malformed line is skipped exactly like a torn one. The
                # O_APPEND writer can genuinely tear a line, and one bad line
                # must never abort a report over weeks of data. json.JSONDecodeError
                # is a ValueError; the rest guard a line that parses but is not
                # shaped like an event (`rr` null, `rr` a string, "from" null or
                # a string, a value SQLite cannot bind).
                try:
                    e = json.loads(line)
                    if not isinstance(e, dict):
                        raise ValueError("not a JSON object")
                    rr = e.get("rr")
                    if rr is not None and not isinstance(rr, dict):
                        raise ValueError("rr is not a JSON object")
                    top = e.get("top")
                    if not isinstance(top, list):
                        top = []
                    # `.get("from", 0)` would still yield None for an explicit
                    # null, and None > 0 raises.
                    if rr and (rr.get("from") or 0) > 0:
                        verdict = "accepted" if e.get("sel") == rr.get("text") else "rejected"
                    elif rr:
                        verdict = "nomove"
                    else:
                        verdict = "misrank"
                    # Whether a downstream filter invalidated this event. When
                    # re-ranking fired it left `rr.text` at position 0 of the
                    # list it handed on — both when it moved a candidate there
                    # (from > 0) and when it agreed the head was already right
                    # (from == 0, the control group). `top` is that same list as
                    # finally displayed. So `top[0] != rr.text` means a filter
                    # installed after copilot_rerank_filter rewrote the text or
                    # reordered the head, and `sel` is no longer comparable with
                    # `rr.text`. nomove events get the identical test on purpose:
                    # a contaminated control group would be just as misleading as
                    # a contaminated rejection rate.
                    #
                    # A missing or empty `top` is not evidence of alteration —
                    # it is an event written with top_n candidates unavailable —
                    # so it is not flagged.
                    head_altered = 1 if (rr and top and top[0] != rr.get("text")) else 0
                    db.execute(
                        "INSERT INTO ev VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
                        (
                            e.get("ts"), e.get("machine"), e.get("schema"), e.get("src"),
                            e.get("input"), e.get("ctx"), e.get("sel_idx"), e.get("sel"),
                            top[0] if top else None,
                            1 if rr else 0,
                            (rr or {}).get("key"), (rr or {}).get("key_len"),
                            (rr or {}).get("n"), (rr or {}).get("text"),
                            (rr or {}).get("from"), (rr or {}).get("rank"),
                            (rr or {}).get("level"),
                            verdict,
                            head_altered,
                        ),
                    )
                except (ValueError, TypeError, AttributeError, sqlite3.InterfaceError):
                    skipped += 1
                    continue
    db.commit()
    return db, skipped


def rate_table(db, title, expr, where="rr_from > 0", limit=None):
    """Rejection rate grouped by `expr`. A flat column is the refutation.

    `ORDER BY bucket` is a BINARY string sort, so every bucket label a CASE in
    this file produces must be zero-padded to a common width ('01-05', not
    '1-5'). Every section here is read as a trend across its buckets; rows out
    of order make a monotonic trend look like noise, which is exactly the
    misreading that would falsely refute a claim.

    `head_altered = 0` is not optional: an event whose head a downstream filter
    rewrote has an accept/reject verdict that means nothing, and one such event
    must never be able to move a rejection rate.
    """
    sql = f"""
      SELECT {expr} AS bucket,
             COUNT(*) AS n,
             SUM(verdict = 'rejected') AS rejected
      FROM ev WHERE rr = 1 AND head_altered = 0 AND {where}
      GROUP BY bucket ORDER BY bucket
    """
    rows = db.execute(sql).fetchall()
    if limit:
        rows = rows[:limit]
    print(f"\n{title}")
    print(f"  {'bucket':<16}{'n':>8}{'rejected':>10}{'rate':>8}")
    for bucket, n, rejected in rows:
        rate = (rejected / n) if n else 0.0
        print(f"  {str(bucket):<16}{n:>8}{rejected:>10}{rate:>7.1%}")
    if not rows:
        print("  (no data yet)")


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("paths", nargs="+", help="JSONL files from any number of machines")
    ap.add_argument("--top", type=int, default=30, help="rows in the per-character tables")
    args = ap.parse_args()

    db, skipped = load(args.paths)
    # Before the `total` check, not after: a file whose every line was skipped
    # has total == 0, and reporting only "No events" would read as "telemetry is
    # not recording" when the truth is "the lines could not be parsed".
    if skipped:
        print(f"warning: skipped {skipped} unparseable line(s)", file=sys.stderr)
    total = db.execute("SELECT COUNT(*) FROM ev").fetchone()[0]
    if not total:
        if skipped:
            print(f"No events: all {skipped} line(s) were unparseable. That is a format "
                  "problem, not an empty telemetry file — check that the paths given are "
                  "copilot telemetry JSONL and not something else.")
        else:
            print("No events. Type for a while with telemetry on, then run this again.")
        return 0

    altered = db.execute("SELECT COUNT(*) FROM ev WHERE head_altered = 1").fetchone()[0]
    usable = total - altered

    print("=" * 64)
    print("OVERALL")
    print("=" * 64)
    print(f"  {'events read':<44}{total:>8}")
    if altered:
        print(f"  {'EXCLUDED, head altered downstream':<44}{altered:>8}  {altered / total:>6.1%}")
    print(f"  {'usable':<44}{usable:>8}")
    if altered:
        if not usable:
            print("\n  !!  EVERY event was thrown away. There is nothing left to report on.")
        elif altered / total >= 0.10:
            print(f"\n  !!  {altered / total:.1%} of events were thrown away. Every table below "
                  f"rests on\n      the remaining {usable}; treat them as a sample, not as your "
                  "typing.")
        print()
        print(HEAD_ALTERED_NOTE, end="")
    if not usable:
        return 0

    print(f"\n  Of the {usable} usable event(s):")
    for verdict, label in [
        ("accepted", "promotion accepted"),
        ("rejected", "promotion REJECTED"),
        ("nomove", "re-ranking agreed, moved nothing (control)"),
        ("misrank", "no re-ranking, user took a later candidate"),
    ]:
        n = db.execute(
            "SELECT COUNT(*) FROM ev WHERE verdict = ? AND head_altered = 0", (verdict,)
        ).fetchone()[0]
        print(f"  {label:<44}{n:>8}  {n / usable:>6.1%}")

    moved = db.execute(
        "SELECT COUNT(*) FROM ev WHERE rr = 1 AND rr_from > 0 AND head_altered = 0"
    ).fetchone()[0]
    if moved:
        rej = db.execute(
            "SELECT COUNT(*) FROM ev WHERE verdict = 'rejected' AND head_altered = 0"
        ).fetchone()[0]
        print(f"\n  Promotions that moved a candidate: {moved}, rejected {rej / moved:.1%}")
        print("  This is the number that says whether re-ranking is a net gain.")

    print("\n" + "=" * 64)
    print("CLAIM 1  rank carries signal, so using it only as a threshold is wrong")
    print("=" * 64)
    rate_table(db, "by rank", "CASE WHEN rr_rank IS NULL THEN 'n/a'"
                              " WHEN rr_rank <= 5 THEN '01-05'"
                              " WHEN rr_rank <= 10 THEN '06-10'"
                              " WHEN rr_rank <= 20 THEN '11-20'"
                              " ELSE '21+' END")
    # A NULL rr_n or rr_rank has to be caught first: it fails every comparison
    # below and would otherwise land in '>10%', inventing evidence against the
    # claim out of missing data.
    rate_table(db, "by rank/n ratio", "CASE WHEN rr_n IS NULL OR rr_rank IS NULL OR rr_n <= 0"
                                      " THEN 'n/a'"
                                      " WHEN 1.0 * rr_rank / rr_n <= 0.001 THEN '<=0.1%'"
                                      " WHEN 1.0 * rr_rank / rr_n <= 0.01 THEN '<=1%'"
                                      " WHEN 1.0 * rr_rank / rr_n <= 0.1 THEN '<=10%'"
                                      " ELSE '>10%' END")

    print("\n" + "=" * 64)
    print("CLAIM 2  the translator's own ranking carries signal")
    print("=" * 64)
    rate_table(db, "by original position", "CASE WHEN rr_from <= 2 THEN '01-02'"
                                           " WHEN rr_from <= 5 THEN '03-05'"
                                           " WHEN rr_from <= 10 THEN '06-10'"
                                           " ELSE '11+' END")

    print("\n" + "=" * 64)
    print("CLAIM 3  exact matches dominating is harmful")
    print("=" * 64)
    rate_table(db, "by match level", "rr_level")

    print("\n" + "=" * 64)
    print("CLAIM 4  function words are over-promoted systematically")
    print("=" * 64)
    rows = db.execute(
        """
        SELECT rr_text, COUNT(*) AS n, SUM(verdict = 'rejected') AS rejected
        FROM ev WHERE rr = 1 AND rr_from > 0 AND head_altered = 0
        GROUP BY rr_text HAVING n >= 5
        ORDER BY rejected DESC, n DESC LIMIT ?
        """,
        (args.top,),
    ).fetchall()
    print(f"\n  {'promoted':<12}{'n':>8}{'rejected':>10}{'rate':>8}")
    for text, n, rejected in rows:
        print(f"  {text:<12}{n:>8}{rejected:>10}{rejected / n:>7.1%}")
    if not rows:
        print("  (not enough data: needs 5+ promotions of the same text)")

    print("\n" + "=" * 64)
    print("CLAIM 5  longer context keys are less reliable")
    print("=" * 64)
    rate_table(db, "by key length", "rr_key_len")

    print("\n" + "=" * 64)
    print("WHAT THE USER WANTED INSTEAD")
    print("=" * 64)
    rows = db.execute(
        """
        SELECT ctx, rr_text, sel, COUNT(*) AS n
        FROM ev WHERE verdict = 'rejected' AND head_altered = 0
        GROUP BY ctx, rr_text, sel ORDER BY n DESC LIMIT ?
        """,
        (args.top,),
    ).fetchall()
    print(f"\n  {'context':<16}{'promoted':<12}{'wanted':<12}{'n':>6}")
    for ctx, promoted, sel, n in rows:
        print(f"  {(ctx or ''):<16}{(promoted or ''):<12}{(sel or ''):<12}{n:>6}")
    if not rows:
        print("  (no rejections recorded)")

    print()
    return 0


if __name__ == "__main__":
    sys.exit(main())
