"""The analyser's derived columns.

`analyze_telemetry.py` is the component that turns the log into a decision,
and it had no tests until this file. Every case here is a line shape the live
log actually produces.
"""
from __future__ import annotations

import contextlib
import importlib.util
import io
import json
import re
import sqlite3
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

# analyze_telemetry.py is a script, not a package module: load it by path.
_SPEC = importlib.util.spec_from_file_location(
    "analyze_telemetry", Path(__file__).resolve().parents[1] / "analyze_telemetry.py")
analyze = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(analyze)


def load_one(event: dict) -> sqlite3.Row:
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(analyze.SCHEMA)
    analyze._load_event_line(db, event)
    return db.execute("SELECT * FROM ev").fetchone()


class DeclineKind(unittest.TestCase):
    """margin 0.0 means the model AGREED with the head, not that a challenger
    lost by a hair. Lowering the margin fixes the second and does nothing for
    the first."""

    def test_agreement_is_labelled_agreed(self):
        row = load_one({
            "v": 3, "sel": "月度", "sel_idx": 1, "top": ["阅读", "月度"],
            "llm": {"incumbent": "阅读", "best": "阅读", "best_from": 0,
                    "margin": 0.0, "skip": "margin", "n_scored": 4, "dropped": []},
        })
        self.assertEqual(row["decline_kind"], "agreed")
        self.assertEqual(row["best_is_sel"], 0)

    def test_a_blocked_pick_the_user_then_chose_is_the_recoverable_case(self):
        row = load_one({
            "v": 3, "sel": "那里", "sel_idx": 1, "top": ["哪里", "那里"],
            "llm": {"incumbent": "哪里", "best": "那里", "best_from": 1,
                    "margin": 0.8799, "skip": "margin", "dropped": []},
        })
        self.assertEqual(row["decline_kind"], "blocked")
        self.assertEqual(row["best_is_sel"], 1)
        self.assertAlmostEqual(row["llm_margin"], 0.8799)

    def test_a_promotion_is_not_a_decline(self):
        row = load_one({
            "v": 3, "sel": "想", "sel_idx": 0, "top": ["想", "先"],
            "llm": {"text": "想", "incumbent": "先", "best": "想", "best_from": 2,
                    "from": 2, "margin": 2.13, "skip": "none", "dropped": []},
        })
        self.assertIsNone(row["decline_kind"])


class SpanGate(unittest.TestCase):
    def test_the_users_choice_among_the_dropped_is_flagged(self):
        row = load_one({
            "v": 3, "sel": "管理", "sel_idx": 2, "top": ["管理业", "grliye", "管理"],
            "llm": {"incumbent": "管理业", "best": "管理业", "margin": 0.0,
                    "skip": "margin", "dropped": ["管理", "惯例"]},
        })
        self.assertEqual(row["llm_dropped_n"], 2)
        self.assertEqual(row["sel_in_dropped"], 1)

    def test_an_empty_gate_flags_nothing(self):
        row = load_one({
            "v": 3, "sel": "故意", "sel_idx": 1, "top": ["顾忌", "故意"],
            "llm": {"incumbent": "顾忌", "best": "故意", "margin": 3.4,
                    "skip": "none", "text": "故意", "from": 1, "dropped": []},
        })
        self.assertEqual(row["llm_dropped_n"], 0)
        self.assertEqual(row["sel_in_dropped"], 0)


class EngageSkip(unittest.TestCase):
    def test_the_top_level_reason_lands_in_its_own_column(self):
        # NOT in `llm_skip`: that column holds Decide()'s verdict and the two
        # are different quantities.
        row = load_one({"v": 3, "sel": "页", "sel_idx": 1, "top": ["也", "页"],
                        "llm_skip": "noctx"})
        self.assertEqual(row["engage_skip"], "noctx")
        self.assertIsNone(row["llm_skip"])
        self.assertEqual(row["llm"], 0)


class Version2Compatibility(unittest.TestCase):
    """v2 lines outlive the code that wrote them; a v3 reader must report the
    new columns as unavailable, never as zero."""

    def test_a_v2_line_loads_with_the_new_columns_null(self):
        row = load_one({
            "v": 2, "sel": "那里", "sel_idx": 1, "top": ["哪里", "那里"],
            "llm": {"incumbent": "哪里", "margin": 0.8799, "skip": "margin",
                    "n_scored": 3, "us": 21508},
        })
        self.assertIsNone(row["llm_best"])
        self.assertIsNone(row["engage_skip"])
        # decline_kind cannot be derived without `best`, and must not guess.
        self.assertIsNone(row["decline_kind"])


def load_all(events: list[dict], stats: list[dict]) -> sqlite3.Connection:
    db = sqlite3.connect(":memory:")
    db.row_factory = sqlite3.Row
    db.executescript(analyze.SCHEMA)
    for e in events:
        analyze._load_event_line(db, e)
    for s in stats:
        analyze._load_stats_line(db, s)
    return db


class AccuracyLine(unittest.TestCase):
    """The number the whole plugin is optimized for, which the report never
    stated. Misses are recorded in full, so it needs no sampling and no
    scaling -- successes - misses over segments, exactly."""

    def test_accuracy_is_segments_minus_misses_over_segments(self):
        db = load_all(
            events=[
                {"v": 3, "sel": "那里", "sel_idx": 1, "top": ["哪里", "那里"]},
                {"v": 3, "sel": "管理", "sel_idx": 2, "top": ["管理业", "管理"]},
                # a promotion the user accepted: a success, and recorded
                {"v": 3, "sel": "想", "sel_idx": 0, "top": ["想", "先"],
                 "llm": {"text": "想", "from": 2, "skip": "none", "best": "想",
                         "incumbent": "先", "dropped": []}},
            ],
            stats=[{"v": 3, "type": "stats", "ts": "t", "segments": 10,
                    "llm_acted": 8, "skip_counts": {"cold": 2},
                    "us_p50": 7723.0, "us_p95": 21472.0}],
        )
        hits, segments, rate = analyze.accuracy_line(db)
        self.assertEqual(segments, 10)
        self.assertEqual(hits, 8)  # 10 segments - 2 misses
        self.assertAlmostEqual(rate, 0.8)

    def test_no_stats_lines_means_no_number_rather_than_a_wrong_one(self):
        db = load_all(events=[{"v": 3, "sel": "那里", "sel_idx": 1, "top": []}],
                      stats=[])
        self.assertIsNone(analyze.accuracy_line(db))


class DeclineSplit(unittest.TestCase):
    def test_the_split_counts_both_kinds_and_the_recoverable_subset(self):
        db = load_all(
            events=[
                {"v": 3, "sel": "月度", "sel_idx": 1, "top": ["阅读", "月度"],
                 "llm": {"incumbent": "阅读", "best": "阅读", "margin": 0.0,
                         "skip": "margin", "n_scored": 4, "dropped": []}},
                {"v": 3, "sel": "那里", "sel_idx": 1, "top": ["哪里", "那里"],
                 "llm": {"incumbent": "哪里", "best": "那里", "margin": 0.88,
                         "skip": "margin", "dropped": []}},
                {"v": 3, "sel": "别的", "sel_idx": 3, "top": ["哪里", "那里"],
                 "llm": {"incumbent": "哪里", "best": "那里", "margin": 1.6,
                         "skip": "margin", "dropped": []}},
            ],
            stats=[],
        )
        split = analyze.decline_split(db)
        self.assertEqual(split["agreed"], 1)
        self.assertEqual(split["blocked"], 2)
        self.assertEqual(split["blocked_and_wanted"], 1)
        # The one the user wanted scored 0.88, so it comes back at a
        # threshold of 0.5 and not at 1.0. Getting this backwards is easy and
        # would make the report recommend a threshold that changes nothing.
        self.assertEqual(split["recovered_at"][0.5], 1)
        self.assertEqual(split["recovered_at"][1.0], 0)


if __name__ == "__main__":
    unittest.main()


def load_files(files: dict) -> sqlite3.Connection:
    """Run the real `load()` over temp files named as the machines are named.

    Not `load_one`: version accounting has to happen where the file name is
    known, because a stats line carries no `machine` of its own.
    """
    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for name, lines in files.items():
            p = Path(tmp) / name
            p.write_text("".join(json.dumps(line) + "\n" for line in lines), encoding="utf-8")
            paths.append(str(p))
        db, _ = analyze.load(paths)
    return db


class RecorderVersions(unittest.TestCase):
    """A machine whose dylib predates the current schema keeps recording, and
    every v3 column it writes is silently absent. Nothing else in the report
    says so -- the engagement rate merely looks wrong, which is an inference,
    not a statement."""

    def test_a_machine_writing_the_previous_schema_is_named_stale(self):
        current = analyze.SCHEMA_VERSION
        db = load_files({
            "New.jsonl": [{"v": current, "ts": "2026-08-21T10:00:00+0800", "machine": "New",
                           "sel": "想", "sel_idx": 0, "top": ["想"]}],
            "Old.jsonl": [{"v": current - 1, "ts": "2026-08-21T10:00:00+0800", "machine": "Old",
                           "sel": "想", "sel_idx": 0, "top": ["想"]}],
        })
        report = {r[0]: r for r in analyze.recorder_report(db)}
        self.assertEqual(report["Old"][1], current - 1)
        self.assertTrue(report["Old"][3])
        self.assertEqual(report["New"][1], current)
        self.assertFalse(report["New"][3])

    def test_a_machine_that_upgraded_is_judged_on_its_latest_line(self):
        current = analyze.SCHEMA_VERSION
        db = load_files({
            "Mac-Mini.jsonl": [
                {"v": current - 1, "ts": "2026-08-20T10:00:00+0800", "machine": "Mac-Mini",
                 "sel": "想", "sel_idx": 0, "top": ["想"]},
                {"v": current, "ts": "2026-08-21T10:00:00+0800", "machine": "Mac-Mini",
                 "sel": "想", "sel_idx": 0, "top": ["想"]},
            ],
        })
        machine, latest_v, lines, stale, behind = analyze.recorder_report(db)[0]
        self.assertEqual((machine, latest_v, lines, behind), ("Mac-Mini", current, 2, 1))
        self.assertFalse(stale)

    def test_a_stats_line_is_attributed_to_the_file_it_came_from(self):
        current = analyze.SCHEMA_VERSION
        db = load_files({
            "Mac-Mini.jsonl": [{"v": current - 1, "type": "stats", "ts": "2026-08-20T15:29:03+0800",
                                "segments": 6, "llm_acted": 5}],
        })
        self.assertEqual(analyze.recorder_report(db), [("Mac-Mini", current - 1, 1, True, 1)])

    def test_an_unparseable_line_is_not_counted_as_a_recorded_line(self):
        db = load_files({
            "Mac-Mini.jsonl": [{"v": analyze.SCHEMA_VERSION, "type": "stats",
                                "ts": "2026-08-21T10:00:00+0800", "segments": "not a number"}],
        })
        self.assertEqual(analyze.recorder_report(db), [])


class FetchTruncation(unittest.TestCase):
    """v5's `trunc_counts` / `depth_p50` -- the unbiased half of the
    truncation question. The same two facts are on every Event, but the event
    stream is sampled (ShouldRecord keeps misses and promotions and only 1 in
    `sample_ok` plain successes), so a share computed from it is biased toward
    the hard cases -- the ones carrying least context."""

    def test_trunc_counts_are_flattened_into_their_own_table(self):
        db = load_all(events=[], stats=[
            {"v": 5, "type": "stats", "ts": "t1", "segments": 100, "llm_acted": 80,
             "trunc_counts": {"screen": 60, "full": 30, "unknown": 10},
             "depth_p50": 3.0, "depth_p95": 6.0},
            {"v": 5, "type": "stats", "ts": "t2", "segments": 50, "llm_acted": 40,
             "trunc_counts": {"screen": 40, "full": 10},
             "depth_p50": 2.0, "depth_p95": 5.0},
        ])
        rows = dict(db.execute(
            "SELECT kind, SUM(count) FROM trunc GROUP BY kind").fetchall())
        self.assertEqual(rows, {"screen": 100, "full": 40, "unknown": 10})

    def test_the_depth_travels_on_the_stats_row(self):
        db = load_all(events=[], stats=[
            {"v": 5, "type": "stats", "ts": "t", "segments": 10, "llm_acted": 8,
             "trunc_counts": {"screen": 10}, "depth_p50": 3.0, "depth_p95": 6.0},
        ])
        row = db.execute("SELECT depth_p50, depth_p95 FROM stats").fetchone()
        self.assertEqual((row["depth_p50"], row["depth_p95"]), (3.0, 6.0))

    def test_a_v4_line_loads_unchanged_and_reports_no_depth(self):
        # Files outlive the code that wrote them: a machine on an older dylib
        # keeps writing v4, and its lines must still count toward `segments`.
        db = load_all(events=[], stats=[
            {"v": 4, "type": "stats", "ts": "t", "segments": 10, "llm_acted": 8,
             "skip_counts": {"cold": 2}, "us_p50": 4.0, "us_p95": 11.0},
        ])
        row = db.execute("SELECT segments, depth_p50 FROM stats").fetchone()
        self.assertEqual(row["segments"], 10)
        self.assertIsNone(row["depth_p50"])
        self.assertEqual(db.execute("SELECT COUNT(*) FROM trunc").fetchone()[0], 0)

    def test_a_malformed_trunc_counts_is_rejected_whole(self):
        db = sqlite3.connect(":memory:")
        db.row_factory = sqlite3.Row
        db.executescript(analyze.SCHEMA)
        with self.assertRaises(ValueError):
            analyze._load_stats_line(db, {
                "v": 5, "type": "stats", "ts": "t", "segments": 10, "llm_acted": 8,
                "trunc_counts": {"screen": 10, "full": "lots"}})
        # Nothing half-written: the `stats` row must not survive a
        # trunc_counts entry that failed validation, exactly as skip_counts
        # already guarantees -- the caller catches the exception and has no
        # transaction to roll back.
        self.assertEqual(db.execute("SELECT COUNT(*) FROM stats").fetchone()[0], 0)
        self.assertEqual(db.execute("SELECT COUNT(*) FROM trunc").fetchone()[0], 0)


    def test_the_section_prints_against_a_plain_tuple_connection(self):
        """`load()` leaves the default row_factory; load_all() above sets
        sqlite3.Row. A report that indexes rows by name passes every test here
        and crashes on the first real file, which is how this test came to
        exist -- caught by running the tool, not by running the suite."""
        db = sqlite3.connect(":memory:")  # deliberately NO row_factory
        db.executescript(analyze.SCHEMA)
        analyze._load_stats_line(db, {
            "v": 5, "type": "stats", "ts": "t", "segments": 100, "llm_acted": 80,
            "trunc_counts": {"screen": 60, "full": 40},
            "depth_p50": 3.0, "depth_p95": 6.0})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            analyze._print_fetch_truncation(db)
        out = buf.getvalue()
        self.assertIn("screen", out)
        self.assertIn("3.0", out)

    def test_no_depth_says_so_rather_than_printing_a_zero(self):
        db = sqlite3.connect(":memory:")
        db.executescript(analyze.SCHEMA)
        analyze._load_stats_line(db, {
            "v": 5, "type": "stats", "ts": "t", "segments": 10, "llm_acted": 8,
            "trunc_counts": {"full": 10}})
        buf = io.StringIO()
        with contextlib.redirect_stdout(buf):
            analyze._print_fetch_truncation(db)
        self.assertIn("not reported", buf.getvalue())


class SchemaVersionDrift(unittest.TestCase):
    """The analyser's idea of `current` has to be the plugin's. A hand-kept
    copy that drifts would report every machine as stale, or none."""

    def test_the_analysers_version_matches_telemetry_event_h(self):
        header = (Path(__file__).resolve().parents[2] / "src" / "telemetry_event.h").read_text(
            encoding="utf-8")
        m = re.search(r"kSchemaVersion\s*=\s*(\d+)", header)
        self.assertIsNotNone(m, "kSchemaVersion not found in src/telemetry_event.h")
        self.assertEqual(analyze.SCHEMA_VERSION, int(m.group(1)))


class SmallSampleGuard(unittest.TestCase):
    """A percentage over a handful of segments is the number that gets written
    down as the baseline every later change is measured against."""

    def test_a_rate_over_too_few_samples_is_not_printed(self):
        self.assertEqual(analyze.pct(1, 3).strip(), "n/a")

    def test_a_rate_over_enough_samples_is_printed(self):
        self.assertEqual(analyze.pct(30, 40, min_total=40).strip(), "75.0%")

    def test_an_empty_denominator_is_not_a_division(self):
        self.assertEqual(analyze.pct(0, 0).strip(), "n/a")


class ReportNamesAStaleRecorder(unittest.TestCase):
    """The check is only worth having if it is loud. Task 9 of the plan had a
    human infer staleness from the engagement rate looking wrong."""

    def _report_over(self, files):
        with tempfile.TemporaryDirectory() as tmp:
            paths = []
            for name, lines in files.items():
                p = Path(tmp) / name
                p.write_text("".join(json.dumps(x) + "\n" for x in lines), encoding="utf-8")
                paths.append(str(p))
            out = io.StringIO()
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
                argv = sys.argv
                sys.argv = ["analyze_telemetry.py", *paths]
                try:
                    analyze.main()
                finally:
                    sys.argv = argv
        return out.getvalue()

    def test_a_machine_on_the_old_schema_is_named_in_the_report(self):
        text = self._report_over({
            "Old.jsonl": [
                {"v": 2, "ts": "2026-08-21T10:00:00+0800", "machine": "Old",
                 "sel": "想", "sel_idx": 1, "top": ["先", "想"]},
                {"v": 2, "type": "stats", "ts": "2026-08-21T10:01:00+0800",
                 "segments": 6, "llm_acted": 5},
            ],
        })
        self.assertIn("Old", text)
        self.assertIn("STALE", text)

    def test_a_current_machine_is_not_flagged(self):
        text = self._report_over({
            "New.jsonl": [
                {"v": analyze.SCHEMA_VERSION, "ts": "2026-08-21T10:00:00+0800",
                 "machine": "New", "sel": "想", "sel_idx": 1, "top": ["先", "想"]},
                {"v": analyze.SCHEMA_VERSION, "type": "stats",
                 "ts": "2026-08-21T10:01:00+0800", "segments": 6, "llm_acted": 5},
            ],
        })
        self.assertNotIn("STALE", text)

    def test_an_accuracy_over_a_handful_of_segments_is_not_given_as_a_rate(self):
        text = self._report_over({
            "New.jsonl": [
                {"v": analyze.SCHEMA_VERSION, "ts": "2026-08-21T10:00:00+0800",
                 "machine": "New", "sel": "想", "sel_idx": 1, "top": ["先", "想"]},
                {"v": analyze.SCHEMA_VERSION, "type": "stats",
                 "ts": "2026-08-21T10:01:00+0800", "segments": 6, "llm_acted": 5},
            ],
        })
        line = next(x for x in text.splitlines() if "first-candidate accuracy" in x)
        self.assertIn("n/a", line)
        self.assertIn("5 / 6 segments", line)


class MixedVersionMachine(unittest.TestCase):
    """A machine that was upgraded mid-file holds lines of both schemas, and
    the older ones have every new column NULL. How many is what says how much
    of the report is blank for reasons that are not about the typing."""

    def test_the_lines_predating_the_current_schema_are_counted(self):
        current = analyze.SCHEMA_VERSION
        db = load_files({
            "Mac-Mini.jsonl": [
                {"v": current - 1, "ts": "2026-08-20T10:00:00+0800", "machine": "Mac-Mini",
                 "sel": "想", "sel_idx": 0, "top": ["想"]},
                {"v": current - 1, "ts": "2026-08-20T11:00:00+0800", "machine": "Mac-Mini",
                 "sel": "想", "sel_idx": 0, "top": ["想"]},
                {"v": current, "ts": "2026-08-21T10:00:00+0800", "machine": "Mac-Mini",
                 "sel": "想", "sel_idx": 0, "top": ["想"]},
            ],
        })
        self.assertEqual(analyze.recorder_report(db), [("Mac-Mini", current, 3, False, 2)])

    def test_a_machine_wholly_on_the_current_schema_has_nothing_behind(self):
        current = analyze.SCHEMA_VERSION
        db = load_files({
            "New.jsonl": [{"v": current, "ts": "2026-08-21T10:00:00+0800", "machine": "New",
                           "sel": "想", "sel_idx": 0, "top": ["想"]}],
        })
        self.assertEqual(analyze.recorder_report(db), [("New", current, 1, False, 0)])


class DeclineWithNothingToCompare(unittest.TestCase):
    """`agreed` carries the advice "the model is the limit, go retrain". That
    is only true when the model had a choice. When the span gate left it one
    candidate, or took away the one the user then picked, it agreed with the
    only thing it was shown -- and the lever is same_span_only, not training."""

    def test_agreement_over_a_real_field_is_a_model_signal(self):
        row = load_one({
            "v": 3, "sel": "月度", "sel_idx": 1, "top": ["阅读", "月度"],
            "llm": {"incumbent": "阅读", "best": "阅读", "margin": 0.0,
                    "skip": "margin", "n_scored": 4, "dropped": []},
        })
        self.assertEqual(row["decline_kind"], "agreed")

    def test_a_single_scored_candidate_is_not_agreement(self):
        row = load_one({
            "v": 3, "sel": "候选", "sel_idx": 1, "top": ["候选词", "候选"],
            "llm": {"incumbent": "候选词", "best": "候选词", "margin": 0.0,
                    "skip": "margin", "n_scored": 1, "dropped": ["候选", "侯选"]},
        })
        self.assertEqual(row["decline_kind"], "gated")

    def test_the_users_choice_removed_before_scoring_is_not_agreement(self):
        row = load_one({
            "v": 3, "sel": "候选", "sel_idx": 1, "top": ["候选词", "候选"],
            "llm": {"incumbent": "候选词", "best": "候选词", "margin": 0.0,
                    "skip": "margin", "n_scored": 4, "dropped": ["候选"]},
        })
        self.assertEqual(row["decline_kind"], "gated")

    def test_a_v2_line_cannot_be_split_and_stays_unclassified(self):
        row = load_one({
            "v": 2, "sel": "月度", "sel_idx": 1, "top": ["阅读", "月度"],
            "llm": {"incumbent": "阅读", "margin": 0.0, "skip": "margin"},
        })
        self.assertIsNone(row["decline_kind"])

    def test_the_split_reports_the_gated_bucket_separately(self):
        db = sqlite3.connect(":memory:")
        db.executescript(analyze.SCHEMA)
        for e in [
            {"v": 3, "sel": "月度", "sel_idx": 1, "top": ["阅读", "月度"],
             "llm": {"incumbent": "阅读", "best": "阅读", "margin": 0.0,
                     "skip": "margin", "n_scored": 4, "dropped": []}},
            {"v": 3, "sel": "候选", "sel_idx": 1, "top": ["候选词", "候选"],
             "llm": {"incumbent": "候选词", "best": "候选词", "margin": 0.0,
                     "skip": "margin", "n_scored": 1, "dropped": ["候选"]}},
        ]:
            analyze._load_event_line(db, e)
        split = analyze.decline_split(db)
        self.assertEqual((split["agreed"], split["gated"], split["blocked"]), (1, 1, 0))


class DisplacedHead(unittest.TestCase):
    """A filter that INSERTS above re-ranking's pick leaves that pick intact
    further down; one that rewrites it removes it from the list entirely. Both
    read as `top[0] != rr.text`, and only the second makes `sel` incomparable."""

    def test_a_pick_pushed_down_is_displaced_not_rewritten(self):
        row = load_one({
            "v": 3, "sel": "的", "sel_idx": 0, "top": ["的", "代", "到"],
            "rr": {"key": "算法", "text": "代", "from": 21},
        })
        self.assertEqual(row["head_altered"], 1)
        self.assertEqual(row["head_displaced"], 1)
        self.assertEqual(row["rr_pos_in_top"], 1)

    def test_a_pick_absent_from_the_list_was_rewritten_or_removed(self):
        row = load_one({
            "v": 3, "sel": "牠", "sel_idx": 0, "top": ["牠", "祂"],
            "rr": {"key": "算法", "text": "他", "from": 3},
        })
        self.assertEqual(row["head_altered"], 1)
        self.assertEqual(row["head_displaced"], 0)
        self.assertIsNone(row["rr_pos_in_top"])

    def test_an_untouched_head_is_neither(self):
        row = load_one({
            "v": 3, "sel": "代", "sel_idx": 0, "top": ["代", "的"],
            "rr": {"key": "算法", "text": "代", "from": 21},
        })
        self.assertEqual(row["head_altered"], 0)
        self.assertEqual(row["head_displaced"], 0)
        self.assertEqual(row["rr_pos_in_top"], 0)


class DisplacingHeads(unittest.TestCase):
    """What did the displacing. One text dominating means a pin, and the
    number worth having is what that pin costs: the times re-ranking was
    right and the user had to reach past the pinned head."""

    def test_heads_are_grouped_and_the_pick_still_chosen_is_counted(self):
        db = sqlite3.connect(":memory:")
        db.executescript(analyze.SCHEMA)
        for e in [
            # displaced by 的; the user reached past it for re-ranking's pick
            {"v": 3, "sel": "代", "sel_idx": 1, "top": ["的", "代"],
             "rr": {"key": "x", "text": "代", "from": 21}},
            # displaced by 的; the user took the pinned head instead
            {"v": 3, "sel": "的", "sel_idx": 0, "top": ["的", "到"],
             "rr": {"key": "x", "text": "到", "from": 9}},
            # rewritten, not displaced -- must not appear here
            {"v": 3, "sel": "牠", "sel_idx": 0, "top": ["牠"],
             "rr": {"key": "x", "text": "他", "from": 3}},
        ]:
            analyze._load_event_line(db, e)
        self.assertEqual(analyze.displaced_heads(db), [("的", 2, 1)])
