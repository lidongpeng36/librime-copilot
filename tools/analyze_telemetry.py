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
import os
import re
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
  ,
  -- v3. NULL on a v1/v2 line: "not recorded" and "recorded as none" are
  -- different evidence, so nothing here defaults to 0.
  llm_best TEXT, llm_best_from INT,
  llm_dropped_n INT,     -- how many candidates the same-span gate removed
  sel_in_dropped INT,    -- 1 when the user chose one of them: the live cost
                         -- of copilot/rerank/same_span_only being true
  decline_kind TEXT,     -- agreed | blocked | NULL. `agreed` is best ==
                         -- incumbent (a model-quality signal; lowering the
                         -- margin cannot help); `blocked` is the model
                         -- preferring something the threshold refused.
  best_is_sel INT,       -- 1 when the model's pick is what the user chose --
                         -- with decline_kind='blocked', the recoverable case
  engage_skip TEXT,      -- the event's TOP-LEVEL llm_skip: why the model did
                         -- not engage. Distinct from the `llm_skip` column
                         -- above, which is Decide()'s own verdict.
  -- The two halves of `head_altered`. A filter that INSERTS above re-ranking's
  -- pick (pin_cand_filter) leaves that pick intact further down the list; one
  -- that REWRITES it (simplifier@traditionalize) or removes it (uniquifier)
  -- takes it out of the list entirely. Both look like `top[0] != rr.text`, and
  -- only the second makes `sel` incomparable with `rr.text`.
  head_displaced INT,    -- 1 when the pick survived, below position 0
  rr_pos_in_top INT      -- where it ended up, NULL when it is not in the list
);

-- One row per `type":"stats"` line (telemetry_event.h:StatsLine). Disjoint
-- from `ev`: a stats line has no `sel`/`rr`/`llm` of its own, it is a
-- flush-interval aggregate over segments `ev` never sees the majority of
-- (ShouldRecord's "hard cases only" gate). This is the "everything" side of
-- the split telemetry_event.h documents; warm-hit rate can only be computed
-- from here.
--
-- `depth_p50`/`depth_p95` are NULL on a v4 line, and also on a v5 line whose
-- window saw no environment-truncated fetch. Those two are the same fact for
-- every question here -- "no depth measurement to report" -- and neither is
-- a zero: the plugin omits the field rather than writing 0.0 precisely so a
-- real measured 0 (the source reached no characters at all) stays legible.
CREATE TABLE stats (
  ts TEXT, segments INT, llm_acted INT, us_p50 REAL, us_p95 REAL,
  depth_p50 REAL, depth_p95 REAL
);

-- One row per (stats line, skip reason) pair, flattened out of that line's
-- `skip_counts` object so it can be summed with plain SQL.
CREATE TABLE skip (
  ts TEXT, reason TEXT, count INT
);

-- One row per (stats line, truncation kind) pair, flattened out of that
-- line's `trunc_counts` object -- full|config|app|screen|unknown. Same shape
-- and same reason as `skip` above.
--
-- Deliberately NOT read off the `ev` table, which also carries `trunc` per
-- event: the event stream is sampled (ShouldRecord keeps every miss and
-- promotion but only 1 in `sample_ok` plain successes), so a kByScreen share
-- computed there is biased toward the hard cases -- the short requests
-- carrying least context, i.e. exactly the ones most likely to be truncated.
-- This table is the unbiased one, and it is the only one whose numbers should
-- be quoted when deciding whether to reach deeper.
CREATE TABLE trunc (
  ts TEXT, kind TEXT, count INT
);

-- One row per (machine, schema version) actually seen in the files, counted
-- over BOTH line types. Its only job is to answer "is some machine still
-- writing an older schema than the code in this checkout" -- i.e. is its
-- librime-copilot.dylib stale. Nothing else in this report can say so: a
-- stale recorder simply omits the newer columns, which reads identically to
-- "that situation did not arise". `ts` is the newest line seen for the pair,
-- so a machine that was upgraded mid-file is judged on its latest line and
-- not on its history.
CREATE TABLE recorder (
  machine TEXT, v INT, ts TEXT, n INT,
  PRIMARY KEY (machine, v)
);
"""

# The schema this checkout's plugin writes (src/telemetry_event.h:kSchemaVersion).
# Kept here rather than parsed out of the header so the report needs no source
# tree at hand; analyze_telemetry_test.py fails if the two drift, which is the
# same generated-file-plus-drift-test arrangement tools/requirements.txt uses.
#
# v4 adds `before_depth` (characters the surrounding source actually returned
# before the caret) and `trunc` (why it stopped: full|config|app|screen) to
# Event. Both are omitted on a line whose trace carried no measurement.
#
# v5 adds `trunc_counts` and `depth_p50`/`depth_p95` to the stats line: the
# same two facts over EVERY segment rather than the sampled subset the event
# stream keeps. The depth pair covers only the fetches the environment cut
# short (screen/app) and is omitted when the window saw none.
SCHEMA_VERSION = 5

# Below this many samples a percentage is noise, and the first percentage this
# report prints becomes the baseline every later change is quoted against. The
# counts are still shown -- they are real; it is the rate derived from them
# that is not yet evidence.
MIN_N = 30


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
        # A stats line carries no `machine` of its own (telemetry_event.h's
        # StatsLine has no such field), and the writer names the file after it
        # -- telemetry.cc:41, `machine + ".jsonl"`, plus a `.N` suffix once
        # rotation has run. So the file name is the only attribution a stats
        # line has, and the machine name itself may contain dots.
        file_machine = re.sub(r"\.jsonl(\.\d+)?$", "", os.path.basename(path))
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
                    # After the line has loaded, not before: a line the loader
                    # rejects is not evidence about which schema the recorder
                    # writes, and counting it would let one torn line make a
                    # current machine look stale.
                    _record_version(db, e, file_machine)
                except (ValueError, TypeError, AttributeError, sqlite3.InterfaceError):
                    skipped += 1
                    continue
    db.commit()
    return db, skipped


def _record_version(db, e, file_machine):
    """Count this line under (machine, schema version), keeping the newest ts."""
    v = e.get("v")
    # A `true` here is not version 1. Anything that is not a plain int is
    # recorded as 0, which prints as `v?` and counts as stale -- unknown and
    # old are the same call to action (rebuild that machine's dylib).
    v = v if isinstance(v, int) and not isinstance(v, bool) else 0
    machine = e.get("machine") or file_machine
    ts = e.get("ts") if isinstance(e.get("ts"), str) else ""
    db.execute(
        "INSERT INTO recorder VALUES (?,?,?,1) ON CONFLICT(machine, v) DO UPDATE SET "
        "n = n + 1, ts = MAX(ts, excluded.ts)",
        (machine, v, ts),
    )


def recorder_report(db, current=None):
    """[(machine, latest_v, lines, stale, behind)], one row per machine, by name.

    `stale` means that machine's most recent line was written by a plugin
    older than this checkout's -- its dylib has not been rebuilt and installed,
    so every column added since is absent from its lines while looking exactly
    like a column whose situation never arose.

    Judged on the LATEST line rather than on the highest version seen, so that
    a rollback reads as a rollback instead of being hidden by the newer lines
    that came before it.

    `behind` is how many of that machine's lines predate `current`. A machine
    upgraded mid-file is not stale, but the lines it wrote before the upgrade
    still have every newer column NULL -- so this is what says how much of the
    report is blank for a reason that has nothing to do with the typing.
    """
    current = SCHEMA_VERSION if current is None else current
    rows = db.execute(
        """
        SELECT machine, SUM(n), SUM(CASE WHEN v < ? THEN n ELSE 0 END),
               (SELECT v FROM recorder inner_r
                 WHERE inner_r.machine = outer_r.machine
                 ORDER BY inner_r.ts DESC, inner_r.v DESC LIMIT 1)
        FROM recorder outer_r GROUP BY machine ORDER BY machine
        """,
        (current,),
    ).fetchall()
    return [(machine, latest_v, lines, latest_v < current, behind)
            for machine, lines, behind, latest_v in rows]


def pct(n, total, width=6, min_total=None):
    """`n / total` as a right-aligned percentage, or `n/a` when total is too small.

    See MIN_N. Returning a string rather than a number is deliberate: a caller
    that got None back would have to decide what to print, and every caller
    would decide it differently.
    """
    min_total = MIN_N if min_total is None else min_total
    if not total or total < min_total:
        return "n/a".rjust(width)
    return f"{n / total:.1%}".rjust(width)


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
    rr_text = rr.get("text") if rr else None
    rr_pos_in_top = top.index(rr_text) if (rr and rr_text in top) else None
    head_altered = 1 if (rr and top and top[0] != rr_text) else 0
    # Recorded, but head_altered stays 1 for these and the tables still exclude
    # them. A displacement only makes `sel == rr.text` sound -- the user took
    # the pick although it was not first, which is unambiguous. `sel != rr.text`
    # stays ambiguous: they were shown a head re-ranking did not produce, and
    # may have taken it only because it was first. Counting those as rejections
    # would bias the very rate the exclusion exists to protect.
    head_displaced = 1 if (head_altered and rr_pos_in_top is not None) else 0

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

    # v3 fields. Absent on v1/v2 -> None throughout, never 0: a reader must be
    # able to tell "this line predates the field" from "the field was zero".
    llm_best = (llm or {}).get("best")
    llm_best_from = (llm or {}).get("best_from")
    dropped = (llm or {}).get("dropped")
    if dropped is not None and not isinstance(dropped, list):
        raise ValueError("llm.dropped is not a JSON array")
    llm_dropped_n = len(dropped) if dropped is not None else None
    sel_in_dropped = (1 if e.get("sel") in dropped else 0) if dropped is not None else None
    # Decide()'s verdict was `margin` AND we know what it preferred. Without
    # `best` the two kinds are indistinguishable, which is the whole reason
    # the field was added -- so a v2 line gets None, not a guess.
    if llm and llm_skip == "margin" and llm_best is not None:
        if llm_best != llm.get("incumbent"):
            decline_kind = "blocked"
        elif (llm.get("n_scored") or 0) <= 1 or sel_in_dropped:
            # `agreed` carries the advice "the model is the limit". That holds
            # only where the model had a choice. With one scored candidate it
            # agreed with the only thing it was shown; with the user's pick
            # among `dropped` it never evaluated the alternative at all. Either
            # way the lever is copilot/rerank/same_span_only, not retraining --
            # opposite fixes, which is the same collapse Defect 1 was about.
            decline_kind = "gated"
        else:
            decline_kind = "agreed"
    else:
        decline_kind = None
    best_is_sel = (1 if llm_best == e.get("sel") else 0) if llm_best is not None else None
    engage_skip = e.get("llm_skip")

    db.execute(
        "INSERT INTO ev VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
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
            llm_best, llm_best_from, llm_dropped_n, sel_in_dropped,
            decline_kind, best_is_sel, engage_skip,
            head_displaced, rr_pos_in_top,
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
    trunc_counts = e.get("trunc_counts")
    if trunc_counts is not None and not isinstance(trunc_counts, dict):
        raise ValueError("trunc_counts is not a JSON object")
    for kind, count in (trunc_counts or {}).items():
        if not isinstance(kind, str) or isinstance(count, bool) or not isinstance(count, (int, float)):
            raise ValueError("trunc_counts entry is not a str -> number mapping")
    us_p50 = e.get("us_p50")
    us_p95 = e.get("us_p95")
    # Absent on v4, and on a v5 window that saw no environment-truncated
    # fetch. NULL rather than 0.0 for both: see the `stats` table comment.
    depth_p50 = e.get("depth_p50")
    depth_p95 = e.get("depth_p95")
    # Validated in full above before the first INSERT: a malformed entry
    # midway through skip_counts or trunc_counts must not leave this line's
    # `stats` row written while its child rows are half-missing -- the
    # exception the caller catches has no transaction to roll that back out.
    db.execute("INSERT INTO stats VALUES (?,?,?,?,?,?,?)",
              (e.get("ts"), segments, llm_acted, us_p50, us_p95, depth_p50, depth_p95))
    for reason, count in (skip_counts or {}).items():
        db.execute("INSERT INTO skip VALUES (?,?,?)", (e.get("ts"), reason, count))
    for kind, count in (trunc_counts or {}).items():
        db.execute("INSERT INTO trunc VALUES (?,?,?)", (e.get("ts"), kind, count))


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


# Margins the report asks "what would this threshold have recovered?" about.
# All BELOW the shipped default of 2.0, and that is not a style choice: a
# `blocked` decline is by definition one whose margin fell short of whatever
# the writing machine was configured with, so a threshold at or above that
# value recovers nothing and would print a column of zeros forever. The old
# report could only sweep upward; this is the other direction.
RECOVERY_THRESHOLDS = (0.5, 1.0, 1.5)


def accuracy_line(db):
    """(hits, segments, rate) — first-candidate accuracy, or None.

    `segments` comes from the stats lines, which count every segment
    BuildCommitEvents walked; `misses` are the recorded events with
    sel_idx != 0, which ShouldRecord keeps unconditionally. So the numerator
    needs no sampling and no scaling.

    Known bias, stated by the caller rather than hidden: `segments` includes
    AutoSpacer's bail-out commits, for which no event is ever produced
    (telemetry_commit.h), so the denominator is slightly larger than the
    population the numerator is drawn from. Correcting it needs a second
    counter on the stats line; deferred until it is shown to matter.

    None when there are no stats lines at all: a v1 file, or a session that
    ended before the first flush interval closed. A rate computed off the
    events alone would be a rate over hard cases only, which is exactly the
    misreading this function exists to replace.
    """
    segments = db.execute("SELECT COALESCE(SUM(segments), 0) FROM stats").fetchone()[0]
    if not segments:
        return None
    misses = db.execute("SELECT COUNT(*) FROM ev WHERE sel_idx != 0").fetchone()[0]
    hits = max(0, segments - misses)
    return hits, segments, hits / segments


def decline_split(db):
    """How the LLM's declines break down, and what a lower margin would buy.

    Two kinds with opposite fixes:
      agreed   best == incumbent, over a field of more than one scored
               candidate -- the model endorsed the head and the user
               disagreed. Lowering `margin` cannot help; this is the share
               that says whether the MODEL is the limit.
      gated    best == incumbent, but the model had nothing to compare: one
               scored candidate, or the user's pick removed by the span gate
               before scoring. Reads as agreement and is not one. The lever
               here is copilot/rerank/same_span_only.
      blocked  the model preferred something else and the threshold refused
               it. When that something is what the user then chose
               (`best_is_sel`), a lower threshold would have fixed the
               segment outright.
    """
    rows = db.execute(
        "SELECT decline_kind, best_is_sel, llm_margin FROM ev WHERE decline_kind IS NOT NULL"
    ).fetchall()
    split = {
        "agreed": 0,
        "gated": 0,
        "blocked": 0,
        "blocked_and_wanted": 0,
        "recovered_at": {t: 0 for t in RECOVERY_THRESHOLDS},
    }
    for kind, best_is_sel, margin in rows:
        if kind in ("agreed", "gated"):
            split[kind] += 1
            continue
        split["blocked"] += 1
        if not best_is_sel:
            continue
        split["blocked_and_wanted"] += 1
        for threshold in RECOVERY_THRESHOLDS:
            if margin is not None and margin >= threshold:
                split["recovered_at"][threshold] += 1
    return split


def displaced_heads(db, limit=10):
    """[(top0, events, pick_still_chosen)] for displaced events, most first.

    `top0` is what ended up at the head instead. One text dominating this list
    is a PIN (the schema's `pin_cand_filter`), not general corruption -- a
    rewriting filter would not concentrate on a single word.

    `pick_still_chosen` counts the events where the user took re-ranking's
    pick anyway, from further down the list. That is an unambiguous accept the
    tables do not count, and it is the live cost of the pin: the times
    re-ranking was right and the user had to reach past the pinned head.
    """
    return [tuple(r) for r in db.execute(
        """
        SELECT top0, COUNT(*), COALESCE(SUM(sel = rr_text), 0)
        FROM ev WHERE head_displaced = 1
        GROUP BY top0 ORDER BY COUNT(*) DESC, top0 LIMIT ?
        """,
        (limit,),
    ).fetchall()]


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
        _print_fetch_truncation(db)
        return 0

    altered = db.execute("SELECT COUNT(*) FROM ev WHERE head_altered = 1").fetchone()[0]
    usable = total - altered

    print("=" * 64)
    print("OVERALL")
    print("=" * 64)
    _print_recorders(db)
    acc = accuracy_line(db)
    if acc:
        hits, segments, rate = acc
        print(f"  {'first-candidate accuracy':<44}{pct(hits, segments, 7)}  "
              f"({hits} / {segments} segments)")
        print("  Denominator counts every segment observed, including AutoSpacer's")
        print("  bail-out commits, which produce no event; the numerator is therefore")
        print("  slightly pessimistic. Misses are recorded in full, so this needs no")
        print("  sampling correction.")
        if segments < MIN_N:
            print(f"  The counts are real; the rate is withheld under {MIN_N} segments,")
            print("  because the first one printed becomes the baseline every later")
            print("  change gets quoted against.")
    else:
        print("  first-candidate accuracy: n/a (no stats lines -- v1 file, or the "
             "first flush interval has not closed)")
    print(f"  {'events read':<44}{total:>8}")
    displaced = db.execute("SELECT COUNT(*) FROM ev WHERE head_displaced = 1").fetchone()[0]
    if altered:
        print(f"  {'EXCLUDED, head altered downstream':<44}{altered:>8}  {altered / total:>6.1%}")
        print(f"    {'displaced -- the pick survived lower down':<42}{displaced:>8}")
        print(f"    {'rewritten or removed -- the pick is gone':<42}{altered - displaced:>8}")
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
        _print_displaced_heads(db)
    if not usable:
        # Independent of `usable`: it comes from the stats table, which
        # head_altered (a db-`rr` concept) has no bearing on.
        _print_skip_distribution(db)
        _print_fetch_truncation(db)
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
    split = decline_split(db)
    total_declines = split["agreed"] + split["gated"] + split["blocked"]
    if total_declines:
        print(f"\n  LLM declined on {total_declines} segment(s)")
        print(f"    {'model agreed with the head':<36}{split['agreed']:>6}"
             f"  {pct(split['agreed'], total_declines)}   <- model quality")
        print(f"    {'span gate left nothing to compare':<36}{split['gated']:>6}"
             f"  {pct(split['gated'], total_declines)}   <- same_span_only")
        print(f"    {'threshold blocked the models pick':<36}{split['blocked']:>6}"
             f"  {pct(split['blocked'], total_declines)}   <- margin")
        if total_declines < MIN_N:
            print(f"    (shares withheld: {total_declines} decline(s) is below the "
                 f"{MIN_N} this report will divide by)")
        if split["blocked"]:
            print(f"      of those, the model's pick is what the user chose: "
                 f"{split['blocked_and_wanted']}")
            for threshold in RECOVERY_THRESHOLDS:
                print(f"      margin {threshold} would have recovered: "
                     f"{split['recovered_at'][threshold]}")
        print("\n  `agreed` is the share a lower margin cannot fix, and it is the only")
        print("  one that says the MODEL is the limit -- when it dominates, the next")
        print("  lever is tools/rime_train/. `gated` looks identical in the log and")
        print("  is not the same thing: the model agreed with the only candidate it")
        print("  was shown, or never saw the one the user picked. That one is")
        print("  copilot/rerank/same_span_only, and reading it as `agreed` would")
        print("  point at a retrain that could not have helped.")

    span = db.execute(
        "SELECT COUNT(*), COALESCE(SUM(sel_in_dropped), 0) FROM ev WHERE llm_dropped_n > 0"
    ).fetchone()
    if span[0]:
        print(f"\n  same_span_only removed candidates on {span[0]} recorded segment(s); "
             f"the user chose one of them {span[1]} time(s).")
        print("  That second number is what setting copilot/rerank/same_span_only to")
        print("  false could address, and nothing else measures it live.")

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
    _print_fetch_truncation(db)
    print()
    return 0


def _print_recorders(db):
    """Which schema each machine is writing, and which are behind this checkout.

    Printed ahead of every number derived from the lines, because a stale
    recorder's missing columns are indistinguishable from columns whose
    situation never arose -- every section below would then read as evidence
    when it is an artefact of a dylib that was never reinstalled. Printed even
    when everything is current: a check that only speaks up on failure cannot
    be told apart from a check that did not run.
    """
    rows = recorder_report(db)
    if not rows:
        return
    print(f"  {'recorders':<30}{'latest':<8}{'lines':>7}")
    for machine, latest_v, lines, stale, behind in rows:
        version = f"v{latest_v}" if latest_v else "v?"
        note = f"   !!  STALE" if stale else (
            f"   {behind} predate v{SCHEMA_VERSION}" if behind else "")
        print(f"    {machine:<28}{version:<8}{lines:>7}{note}")
    stale_names = [r[0] for r in rows if r[3]]
    if stale_names:
        print(f"\n  !!  Not writing schema v{SCHEMA_VERSION}, which is what this checkout's")
        print(f"      src/telemetry_event.h writes: {', '.join(stale_names)}.")
        print("      The librime-copilot.dylib there predates the source, so every")
        print("      column added since is simply absent from those lines -- and absent")
        print("      reads exactly like `this never happened`. Rebuild and reinstall")
        print("      (see CLAUDE.md, the Squirrel.app copy step) before treating the")
        print("      sections below as evidence about that machine.")
    print()


def _print_displaced_heads(db):
    """What displaced re-ranking's pick, and what that cost."""
    rows = displaced_heads(db)
    if not rows:
        return
    print("\n  What took the head instead, most frequent first:")
    for text, n, kept in rows:
        print(f"    {text:<12}{n:>6} event(s)   the pick was still chosen {kept}")
    print("\n  One text dominating this list is a PIN -- the schema's")
    print("  `pin_cand_filter` -- not general corruption; a rewriting filter")
    print("  would not concentrate on a single word. The last number is what")
    print("  that pin costs: the times re-ranking was right and the user had")
    print("  to reach past the pinned head. Those are unambiguous accepts, and")
    print("  the tables below still do not count them: `sel != pick` under a")
    print("  displacement stays ambiguous, because the head the user was shown")
    print("  is not the head re-ranking produced.")


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


def _print_fetch_truncation(db):
    """How often the surrounding fetch is cut short, and how deep it gets.

    Which bucket dominates decides which lever is the right one, and the two
    are not interchangeable:

      config  the source HAD more and `prefix_chars` cut it. Raising that cap
              recovers real text, and 8 -> 64 characters is worth +2.47 points
              (p = 4.6e-07; the context-length results record). This is the
              bucket with a measured payoff behind it.
      screen  the terminal cannot see further. Raising the cap buys nothing;
              only chrome stripping or scrollback would, and neither has any
              measured gain -- that record says explicitly that it "says
              nothing about chrome".
      full    the input region ended on its own. Nothing to win either way.

    From the `trunc` table, never from `ev.trunc`: the event stream is
    sampled and its sample is biased toward short requests, which are
    exactly the ones most likely to be truncated. A share computed there
    would be wrong by an unknown amount in a knowable direction.
    """
    print("\n" + "=" * 64)
    print("SURROUNDING FETCH  how often it is cut short, and how deep it gets")
    print("=" * 64)
    rows = db.execute(
        "SELECT kind, SUM(count) AS n FROM trunc GROUP BY kind ORDER BY n DESC"
    ).fetchall()
    if not rows:
        print("  (no `trunc_counts` on any stats line -- every machine here is writing")
        print(f"   a schema older than v{SCHEMA_VERSION}; see the RECORDERS section above)")
        return
    total = sum(n for _, n in rows)
    print(f"  {'stopped because':<18}{'n':>10}{'share':>9}")
    for kind, n in rows:
        print(f"  {kind:<18}{n:>10}{n / total:>8.1%}")
    print(f"  {'(total)':<18}{total:>10}")

    unknown = dict(rows).get("unknown", 0)
    if unknown / total >= 0.5:
        print("\n  !!  Most fetches cannot say why they stopped. That is the IME Bridge's")
        print("      honest answer (src/imk_client.h) -- it receives a string over a")
        print("      socket and has no way to learn whether the client had more. The")
        print("      shares above describe the minority that can say, not typing overall.")

    # Per-WINDOW percentiles. A percentile of the pooled segments is not
    # recoverable from them -- averaging percentiles is not a percentile --
    # so this reports their spread and says so, rather than printing a
    # plausible single number that is not the statistic it looks like.
    depths = db.execute(
        "SELECT depth_p50, depth_p95 FROM stats WHERE depth_p50 IS NOT NULL"
    ).fetchall()
    if not depths:
        print("\n  depth: not reported -- no window saw a fetch the environment cut")
        print("         short (screen/app). Nothing here is limited by how far the")
        print("         source can see.")
        _print_fetch_lever(rows)
        return
    # By position, not by name: `load()` leaves the connection's default
    # row_factory (plain tuples), while the unit tests build theirs with
    # sqlite3.Row. Indexing works on both; naming works only on the second,
    # and that difference once let a green test suite ship a report that
    # crashed on the first real file.
    p50s = sorted(r[0] for r in depths)
    p95s = sorted(r[1] for r in depths)
    mid = lambda v: v[len(v) // 2]
    print(f"\n  depth reached when the environment ran out (screen/app), "
         f"over {len(depths)} window(s):")
    print(f"    p50   typical {mid(p50s):.1f}   range {p50s[0]:.1f} - {p50s[-1]:.1f}")
    print(f"    p95   typical {mid(p95s):.1f}   range {p95s[0]:.1f} - {p95s[-1]:.1f}")
    print("    (per-window percentiles; a pooled one is not recoverable by averaging")
    print("     them. For the pooled shape use `ev.before_depth`, accepting its bias.)")
    _print_fetch_lever(rows)


def _print_fetch_lever(rows):
    """Which lever the dominant bucket points at.

    Called on BOTH exits from _print_fetch_truncation, including the one where
    no depth was measured -- that path is a dataset with no screen/app fetch at
    all, i.e. the strongest possible "raise the cap" case, and an early return
    there printed no guidance whatsoever. Caught by the per-branch tests, which
    exist because an untested branch is how the inverted guidance survived.
    """
    dominant = rows[0][0]
    if dominant == "config":
        print("\n  `config` dominates: the source had MORE text and prefix_chars cut it.")
        print("  That is the bucket with a measured payoff -- 8 -> 64 characters is worth")
        print("  +2.47 points (p=4.6e-07). copilot/rerank/llm/context_chars is a consumer")
        print("  declaration only; it is not yet a term in copilot.cc's prefix_chars max,")
        print("  so the fetch stops at 8 whatever it says. Raising that is the lever.")
    elif dominant in ("screen", "app"):
        print("\n  `screen`/`app` dominates: the source cannot see further, so raising")
        print("  prefix_chars buys nothing here. The levers are chrome stripping and")
        print("  scrollback -- NEITHER of which has a measured gain. The +2.47 result is")
        print("  about context LENGTH (8 -> 64) and says nothing about chrome.")
    else:
        print("\n  Neither cap is binding: the input region mostly ends on its own, so")
        print("  reaching deeper has nothing to reach for.")


if __name__ == "__main__":
    sys.exit(main())
