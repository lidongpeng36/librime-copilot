"""The train/eval split, which is what keeps the model off its own test set.

The evaluation corpus is the user's own text and so is the best training data --
the same 5428 utterances. Splitting by TIME rather than at random is the point:
a deployed model is trained on the past and used on the future, and a random
split would put adjacent messages of one conversation on both sides, where topic
and phrasing are strongly correlated.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import split


def rec(ts, text="这个改动我看下", src="dingtalk"):
    return {"v": 1, "id": ts, "src": src, "ts": ts, "text": text, "redacted": []}


class PartitionByTimeTest(unittest.TestCase):
    def test_before_the_cut_is_training(self):
        train, ev = split.partition_by_time([rec("2025-06-30T23:59:59+08:00")], "2025-07-01")
        self.assertEqual(len(train), 1)
        self.assertEqual(ev, [])

    def test_the_cut_day_itself_is_evaluation(self):
        """Half-open [cut, ...): the boundary belongs to eval, so 'trained on
        everything before July' and 'measured on July onward' are exhaustive
        and cannot both claim a record."""
        train, ev = split.partition_by_time([rec("2025-07-01T00:00:00+08:00")], "2025-07-01")
        self.assertEqual(train, [])
        self.assertEqual(len(ev), 1)

    def test_local_date_decides_not_utc(self):
        """Timestamps carry +08:00. 2025-07-01T00:30+08:00 is still 2025-06-30
        in UTC; the user's own calendar day is the meaningful boundary."""
        train, ev = split.partition_by_time([rec("2025-07-01T00:30:00+08:00")], "2025-07-01")
        self.assertEqual(len(ev), 1)

    def test_records_keep_their_order_within_each_half(self):
        rs = [rec("2024-01-01T00:00:00+08:00"), rec("2026-01-01T00:00:00+08:00"),
              rec("2024-06-01T00:00:00+08:00")]
        train, ev = split.partition_by_time(rs, "2025-07-01")
        self.assertEqual([r["ts"] for r in train],
                         ["2024-01-01T00:00:00+08:00", "2024-06-01T00:00:00+08:00"])
        self.assertEqual([r["ts"] for r in ev], ["2026-01-01T00:00:00+08:00"])

    def test_an_unparseable_timestamp_is_reported_not_guessed(self):
        """Silently filing a bad record on either side is how a test set gets
        trained on. Raising names the record instead."""
        with self.assertRaises(ValueError):
            split.partition_by_time([rec("not-a-timestamp")], "2025-07-01")


if __name__ == "__main__":
    unittest.main()
