"""`rime-corpus ingest`, driven with fake adapters instead of a network.

The harvest window is the only thing here worth testing, and it is worth it
because it is the difference between an evaluation corpus that can resolve a
tuning change and one that cannot: at 90 days the DingTalk source yields 996 of
the user's own messages, at 730 it yields 5712, and the corpus's confidence
interval scales as 1/sqrt(n).
"""
from __future__ import annotations

import argparse
import json
import sys
import tempfile
import unittest
from contextlib import redirect_stderr, redirect_stdout
from io import StringIO
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import cli


class _Windowed:
    """Stands in for adapters.dingtalk: a source with a time window."""

    SOURCE = "windowed"
    DEFAULT_WINDOW_DAYS = 90

    def __init__(self):
        self.calls = []

    def iter_utterances(self, start=None, max_items=None):
        self.calls.append({"start": start, "max_items": max_items})
        yield ("2026-08-14T10:00:00+08:00", "这个改动我看下")


class _Unwindowed:
    """Stands in for adapters.claude: everything local, no window to speak of."""

    SOURCE = "unwindowed"

    def __init__(self):
        self.calls = []

    def iter_utterances(self):
        self.calls.append({})
        yield ("2026-08-14T10:00:00+08:00", "这个也看下")


def _run(sources, registry, **flags):
    """Invoke _ingest against `registry`, returning (stdout, stderr)."""
    args = argparse.Namespace(
        corpus_dir=flags.pop("corpus_dir"),
        source=sources,
        since_days=flags.pop("since_days", None),
        max_items=flags.pop("max_items", None),
    )
    assert not flags, flags
    out, err = StringIO(), StringIO()
    original = cli.REGISTRY
    cli.REGISTRY = registry
    try:
        with redirect_stdout(out), redirect_stderr(err):
            cli._ingest(args)
    finally:
        cli.REGISTRY = original
    return out.getvalue(), err.getvalue()


class IngestWindowTest(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.dir = self._tmp.name
        self.addCleanup(self._tmp.cleanup)

    def test_no_flag_leaves_the_adapter_default_alone(self):
        """Absent --since-days, nothing is passed, so the adapter's own default
        (90 days for dingtalk) applies. Passing an explicit 90 here instead
        would silently pin the CLI to a number the adapter is free to change."""
        w = _Windowed()
        _run(["windowed"], {"windowed": w}, corpus_dir=self.dir)
        self.assertEqual(w.calls, [{"start": None, "max_items": None}])

    def test_since_days_becomes_an_iso_start(self):
        w = _Windowed()
        _run(["windowed"], {"windowed": w}, corpus_dir=self.dir, since_days=730)
        self.assertEqual(len(w.calls), 1)
        start = w.calls[0]["start"]
        self.assertIsNotNone(start)
        # An offset-aware ISO 8601 string the adapter can hand to dws as-is.
        from datetime import datetime, timezone

        parsed = datetime.fromisoformat(start)
        self.assertIsNotNone(parsed.tzinfo)
        age = (datetime.now(timezone.utc) - parsed).days
        self.assertGreaterEqual(age, 729)
        self.assertLessEqual(age, 731)

    def test_max_items_is_passed_through(self):
        w = _Windowed()
        _run(["windowed"], {"windowed": w}, corpus_dir=self.dir, max_items=5000)
        self.assertEqual(w.calls[0]["max_items"], 5000)

    def test_unwindowed_source_is_never_handed_a_window(self):
        """adapters.claude's iter_utterances() takes no `start`; handing it one
        is a TypeError, and quietly dropping the flag is worse -- the run would
        report success having harvested a window it never applied."""
        u = _Unwindowed()
        _run(["unwindowed"], {"unwindowed": u}, corpus_dir=self.dir, since_days=730)
        self.assertEqual(u.calls, [{}])

    def test_unwindowed_source_says_the_flag_did_not_apply(self):
        u = _Unwindowed()
        _, err = _run(["unwindowed"], {"unwindowed": u}, corpus_dir=self.dir, since_days=730)
        self.assertIn("unwindowed", err)
        self.assertIn("--since-days", err)

    def test_silent_when_no_window_flag_was_given(self):
        u = _Unwindowed()
        _, err = _run(["unwindowed"], {"unwindowed": u}, corpus_dir=self.dir)
        self.assertEqual(err, "")

    def test_mixed_run_windows_only_the_source_that_has_one(self):
        w, u = _Windowed(), _Unwindowed()
        _run(["windowed", "unwindowed"], {"windowed": w, "unwindowed": u},
             corpus_dir=self.dir, since_days=365)
        self.assertIsNotNone(w.calls[0]["start"])
        self.assertEqual(u.calls, [{}])

    def test_records_still_land_in_the_corpus(self):
        w = _Windowed()
        _run(["windowed"], {"windowed": w}, corpus_dir=self.dir, since_days=730)
        with (Path(self.dir) / "windowed.jsonl").open(encoding="utf-8") as handle:
            written = list(handle)
        self.assertEqual(len(written), 1)
        self.assertEqual(json.loads(written[0])["text"], "这个改动我看下")


class IngestArgparseTest(unittest.TestCase):
    """What `main` puts on the namespace, since `_ingest` above is driven with a
    hand-built one and would not notice the parser drifting away from it."""

    def _parse(self, argv):
        captured = {}

        def fake_ingest(args):
            captured["args"] = args
            return 0

        registry, ingest = cli.REGISTRY, cli._ingest
        cli.REGISTRY = {"windowed": _Windowed()}
        cli._ingest = fake_ingest
        try:
            with redirect_stdout(StringIO()):
                cli.main(argv)
        finally:
            cli.REGISTRY, cli._ingest = registry, ingest
        return captured["args"]

    def test_both_flags_default_to_none_not_a_number(self):
        """None means "the adapter's own default". Any int here would override
        dingtalk's 90 on every plain `ingest`, from the CLI rather than from the
        adapter that owns the number."""
        args = self._parse(["ingest", "windowed"])
        self.assertIsNone(args.since_days)
        self.assertIsNone(args.max_items)

    def test_both_flags_parse(self):
        args = self._parse(["ingest", "windowed", "--since-days", "730",
                            "--max-items", "5000"])
        self.assertEqual(args.since_days, 730)
        self.assertEqual(args.max_items, 5000)

    def test_since_days_needs_a_value(self):
        with self.assertRaises(SystemExit):
            self._parse(["ingest", "windowed", "--since-days"])


if __name__ == "__main__":
    unittest.main()
