"""The analyser's derived columns.

`analyze_telemetry.py` is the component that turns the log into a decision,
and it had no tests until this file. Every case here is a line shape the live
log actually produces.
"""
from __future__ import annotations

import importlib.util
import sqlite3
import sys
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
                    "margin": 0.0, "skip": "margin", "dropped": []},
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
                         "skip": "margin", "dropped": []}},
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
