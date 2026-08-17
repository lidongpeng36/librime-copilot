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
  rr INT,            -- 1 when db re-ranking promoted something
  rr_key TEXT, rr_key_len INT, rr_n INT, rr_text TEXT,
  rr_from INT, rr_rank INT, rr_level INT,
  verdict TEXT,      -- accepted | rejected | nomove | misrank (db path only --
                     -- unchanged by v2; see llm_verdict for the LLM path)
  head_altered INT,  -- 1 when the displayed head is not what db re-ranking put
                     -- first, so `sel` cannot be compared with `rr.text`
  -- v2 only, from the event's optional `llm` object (schema.h:LlmRecord).
  -- A v1 line, or a v2 line the LLM path never touched, leaves every column
  -- below NULL/0 -- never required on the v1 path, per kSchemaVersion's
  -- compatibility rule.
  llm INT,           -- 1 when the LLM path was engaged (scored) at all
  llm_text TEXT, llm_incumbent TEXT, llm_from INT, llm_margin REAL,
  llm_n_scored INT, llm_us INT,
  llm_skip TEXT,     -- none | nohan | margin -- Decide()'s own verdict; the
                     -- other five SkipReasonName() values never reach an
                     -- event at all (telemetry_commit.cc only fills `llm`
                     -- when llm_skip == kNone at the ENGAGEMENT level, a
                     -- different, outer check from this column)
  llm_promoted INT,  -- 1 when llm_skip = 'none', i.e. the LLM actually moved
                     -- a candidate (mirrors `rr` for the db path)
  llm_verdict TEXT,  -- accepted | rejected | declined | NULL (llm never
                     -- engaged for this event)
  llm_head_altered INT  -- same concept as head_altered, evaluated against
                        -- llm_text instead of rr_text
);

-- One row per `type":"stats"` line (telemetry_event.h:StatsLine). Disjoint
-- from `ev`: a stats line has no `sel`/`rr`/`llm` of its own, it is a
-- flush-interval aggregate over segments `ev` never sees the majority of
-- (ShouldRecord's "hard cases only" gate). This is the "everything" side of
-- the split telemetry_event.h documents; warm-hit rate can only be computed
-- from here.
CREATE TABLE stats (
  ts TEXT, segments INT, llm_acted INT, us_p50 REAL, us_p95 REAL
);

-- One row per (stats line, skip reason) pair, flattened out of that line's
-- `skip_counts` object so it can be summed with plain SQL.
CREATE TABLE skip (
  ts TEXT, reason TEXT, count INT
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
                    # `type` is the v2 discriminator (telemetry_event.h): an
                    # Event line has no `type` at all -- v1 files predate the
                    # concept and must keep loading on this same branch
                    # unchanged -- while a stats line always carries
                    # `type == "stats"` and has none of an event's fields.
                    if e.get("type") == "stats":
                        _load_stats_line(db, e)
                    else:
                        _load_event_line(db, e)
                except (ValueError, TypeError, AttributeError, sqlite3.InterfaceError):
                    skipped += 1
                    continue
    db.commit()
    return db, skipped


def _load_event_line(db, e):
    rr = e.get("rr")
    if rr is not None and not isinstance(rr, dict):
        raise ValueError("rr is not a JSON object")
    top = e.get("top")
    if not isinstance(top, list):
        top = []
    # `.get("from", 0)` would still yield None for an explicit null, and
    # None > 0 raises.
    if rr and (rr.get("from") or 0) > 0:
        verdict = "accepted" if e.get("sel") == rr.get("text") else "rejected"
    elif rr:
        verdict = "nomove"
    else:
        verdict = "misrank"
    # Whether a downstream filter invalidated this event. When re-ranking
    # fired it left `rr.text` at position 0 of the list it handed on — both
    # when it moved a candidate there (from > 0) and when it agreed the head
    # was already right (from == 0, the control group). `top` is that same
    # list as finally displayed. So `top[0] != rr.text` means a filter
    # installed after copilot_rerank_filter rewrote the text or reordered the
    # head, and `sel` is no longer comparable with `rr.text`. nomove events
    # get the identical test on purpose: a contaminated control group would
    # be just as misleading as a contaminated rejection rate.
    #
    # A missing or empty `top` is not evidence of alteration — it is an
    # event written with top_n candidates unavailable — so it is not
    # flagged.
    head_altered = 1 if (rr and top and top[0] != rr.get("text")) else 0

    # v2's `llm` object -- absent on every v1 line and on a v2 line the LLM
    # path never touched (rerank_filter.cc runs the db loop OR the LLM path
    # per segment, never both, so `rr` and `llm` are never both set on one
    # event). Same validate-then-default shape as `rr` above, so a line that
    # parses but is not shaped like an event still lands in `skipped` rather
    # than corrupting a row.
    llm = e.get("llm")
    if llm is not None and not isinstance(llm, dict):
        raise ValueError("llm is not a JSON object")
    llm_skip = (llm or {}).get("skip")
    # skip == "none" is Decide()'s own "something was promoted" (rerank_llm.h)
    # -- distinct from the outer "was the model even consulted" check that
    # gates whether `llm` is present at all (telemetry_commit.cc's
    # `llm_engaged`). Both must hold for a promotion to have actually
    # happened, but only the inner one varies once `llm` is present.
    llm_promoted = 1 if (llm and llm_skip == "none") else 0
    if llm_promoted:
        llm_verdict = "accepted" if e.get("sel") == llm.get("text") else "rejected"
    elif llm:
        llm_verdict = "declined"  # consulted (nohan/margin), promoted nothing
    else:
        llm_verdict = None  # never engaged for this event
    # Same reasoning as head_altered above, evaluated against llm_text: only
    # meaningful when the LLM actually promoted something, since a decline
    # left nothing at the front to have been overwritten.
    llm_head_altered = 1 if (llm_promoted and top and top[0] != llm.get("text")) else 0

    db.execute(
        "INSERT INTO ev VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
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
            1 if llm else 0,
            (llm or {}).get("text"), (llm or {}).get("incumbent"),
            (llm or {}).get("from"), (llm or {}).get("margin"),
            (llm or {}).get("n_scored"), (llm or {}).get("us"),
            llm_skip,
            llm_promoted,
            llm_verdict,
            llm_head_altered,
        ),
    )


def _load_stats_line(db, e):
    segments = e.get("segments")
    llm_acted = e.get("llm_acted")
    # bool is an int subclass; a stats line never legitimately has `true` for
    # a count, but this loader treats booleans as malformed on the same
    # principle the `ev` branch does for `rr`/`llm` -- "parses but is not
    # shaped like the record" belongs in `skipped`, not in a row.
    if isinstance(segments, bool) or not isinstance(segments, (int, float)):
        raise ValueError("stats line: segments is not numeric")
    if isinstance(llm_acted, bool) or not isinstance(llm_acted, (int, float)):
        raise ValueError("stats line: llm_acted is not numeric")
    skip_counts = e.get("skip_counts")
    if skip_counts is not None and not isinstance(skip_counts, dict):
        raise ValueError("skip_counts is not a JSON object")
    for reason, count in (skip_counts or {}).items():
        if not isinstance(reason, str) or isinstance(count, bool) or not isinstance(count, (int, float)):
            raise ValueError("skip_counts entry is not a str -> number mapping")
    us_p50 = e.get("us_p50")
    us_p95 = e.get("us_p95")
    # Validated in full above before the first INSERT: a malformed entry
    # midway through skip_counts must not leave this line's `stats` row
    # written while its `skip` rows are half-missing -- the exception the
    # caller catches has no transaction to roll back that back out of.
    db.execute("INSERT INTO stats VALUES (?,?,?,?,?)",
              (e.get("ts"), segments, llm_acted, us_p50, us_p95))
    for reason, count in (skip_counts or {}).items():
        db.execute("INSERT INTO skip VALUES (?,?,?)", (e.get("ts"), reason, count))


def rate_table(db, title, expr, where="rr_from > 0", limit=None,
              promoted_col="rr", verdict_col="verdict", altered_col="head_altered"):
    """Rejection rate grouped by `expr`. A flat column is the refutation.

    `ORDER BY bucket` is a BINARY string sort, so every bucket label a CASE in
    this file produces must be zero-padded to a common width ('01-05', not
    '1-5'). Every section here is read as a trend across its buckets; rows out
    of order make a monotonic trend look like noise, which is exactly the
    misreading that would falsely refute a claim.

    `head_altered = 0` is not optional: an event whose head a downstream filter
    rewrote has an accept/reject verdict that means nothing, and one such event
    must never be able to move a rejection rate.

    `promoted_col`/`verdict_col`/`altered_col` let the LLM decision-quality
    section (Task 8) reuse this exact shape against `llm_promoted`/
    `llm_verdict`/`llm_head_altered` instead of the db path's `rr`/`verdict`/
    `head_altered` -- every existing call site keeps the defaults, so this is
    purely additive.
    """
    sql = f"""
      SELECT {expr} AS bucket,
             COUNT(*) AS n,
             SUM({verdict_col} = 'rejected') AS rejected
      FROM ev WHERE {promoted_col} = 1 AND {altered_col} = 0 AND {where}
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
    stats_total = db.execute("SELECT COUNT(*) FROM stats").fetchone()[0]
    # A file can legitimately hold stats lines with no (recordable) events in
    # the same window -- ShouldRecord keeps only "hard cases", stats.Observe
    # counts everything. `not total` alone would call that "No events" and
    # skip the one section (skip-reason distribution) that has data to show.
    if not total and not stats_total:
        if skipped:
            print(f"No events: all {skipped} line(s) were unparseable. That is a format "
                  "problem, not an empty telemetry file — check that the paths given are "
                  "copilot telemetry JSONL and not something else.")
        else:
            print("No events. Type for a while with telemetry on, then run this again.")
        return 0
    if not total:
        print("No per-event lines (only stats lines) in the given files.")
        _print_skip_distribution(db)
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
        # Independent of `usable`: it comes from the stats table, which
        # head_altered (a db-`rr` concept) has no bearing on.
        _print_skip_distribution(db)
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
    print("LLM DECISION QUALITY  acceptance rate by margin")
    print("=" * 64)
    print("  The live version of the offline threshold sweep that set the current")
    print("  copilot/rerank/llm/margin (2.0) -- see")
    print("  docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md. Every")
    print("  promotion here already cleared whatever margin the writing machine was")
    print("  configured with, so a bucket below that value is empty by construction,")
    print("  not evidence of anything -- it says what RAISING the threshold further")
    print("  would cost or buy, not what a lower one would have done.")
    rate_table(db, "by margin",
              "CASE WHEN llm_margin IS NULL THEN 'n/a'"
              " WHEN llm_margin < 2 THEN '00-02'"
              " WHEN llm_margin < 3 THEN '02-03'"
              " WHEN llm_margin < 5 THEN '03-05'"
              " WHEN llm_margin < 8 THEN '05-08'"
              " ELSE '08+' END",
              where="llm_from > 0", promoted_col="llm_promoted",
              verdict_col="llm_verdict", altered_col="llm_head_altered")
    acted = db.execute("SELECT COUNT(*) FROM ev WHERE llm = 1").fetchone()[0]
    declined = db.execute(
        "SELECT COUNT(*) FROM ev WHERE llm_verdict = 'declined'").fetchone()[0]
    if acted:
        print(f"\n  LLM engaged (scored) on {acted} recorded event(s); declined to "
             f"promote on {declined} ({declined / acted:.1%}).")
    else:
        print("\n  (LLM path never engaged on a recorded event -- rerank/llm/enable is "
             "off, or every event here came from the db path)")

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

    _print_skip_distribution(db)
    print()
    return 0


def _print_skip_distribution(db):
    """Where the LLM path's benefit leaks, from stats lines alone.

    Independent of the `ev` table and everything gating it (usable/altered):
    a stats line aggregates every segment StatsAccumulator saw, not just the
    ones ShouldRecord kept, so this has data even in a run where every event
    was excluded above, or where nothing was ever `ShouldRecord`-worthy.
    """
    print("\n" + "=" * 64)
    print("SKIP-REASON DISTRIBUTION  where the LLM path's benefit leaks")
    print("=" * 64)
    segments, acted = db.execute(
        "SELECT COALESCE(SUM(segments), 0), COALESCE(SUM(llm_acted), 0) FROM stats"
    ).fetchone()
    if not segments:
        print("  (no stats lines yet -- v1 files, or a v2 file from before the first "
             "flush interval closed, have none)")
        return
    cold = db.execute(
        "SELECT COALESCE(SUM(count), 0) FROM skip WHERE reason = 'cold'"
    ).fetchone()[0]
    # No `warm_hit` field on the wire (telemetry_event.h): under the fallback
    # chain as implemented, kCold is assigned before Score() can ever run and
    # is mutually exclusive with kNone, so a segment is never scored while not
    # warm. `acted` (the numerator) and `cold` (the other side of "was it
    # warm when we needed it") are therefore the two counts that carry
    # independent information; every other skip reason is orthogonal to
    # warmth and left out of this ratio on purpose.
    warm_denom = acted + cold
    print(f"  segments observed:  {segments}")
    print(f"  llm engaged:        {acted}  ({acted / segments:.1%} of segments)")
    if warm_denom:
        print(f"  warm-hit rate:      {acted / warm_denom:.1%}  "
             "(llm_acted / (llm_acted + skip[cold]))")
    else:
        print("  warm-hit rate:      n/a (no engaged and no cold segments recorded)")

    rows = db.execute(
        "SELECT reason, SUM(count) AS n FROM skip GROUP BY reason ORDER BY n DESC"
    ).fetchall()
    print(f"\n  {'reason':<12}{'n':>10}{'share':>9}")
    for reason, n in rows:
        print(f"  {reason:<12}{n:>10}{n / segments:>8.1%}")
    if not rows:
        print("  (no skips recorded -- the LLM path engaged on every segment observed)")
    print("\n  If `cold` dominates, the problem is the warming trigger, not the model:")
    print("  see src/warm_cache.h and copilot.cc's WarmUp calls.")


if __name__ == "__main__":
    sys.exit(main())
