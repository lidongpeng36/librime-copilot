"""bench_matrix's pure logic: parsing, provenance, and the comparison table.

Nothing here runs bench_scorer -- the binary needs a 42MB model and a GPU, and
the parts that get a comparison wrong are not the parts that talk to it. Every
case is a shape a real run produces.
"""
from __future__ import annotations

import importlib.util
import io
import json
import contextlib
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

_SPEC = importlib.util.spec_from_file_location(
    "bench_matrix", Path(__file__).resolve().parents[1] / "bench_matrix.py")
bm = importlib.util.module_from_spec(_SPEC)
_SPEC.loader.exec_module(bm)


def _row(name, **kw):
    base = {"name": name, "score_p50_ms": 2.0, "score_p99_ms": 4.0,
            "prefill_p50_ms": 2.0, "cpu_ms_per_iter": 1.8, "decoded_per_iter": 4.0}
    base.update(kw)
    return base


def _run(rows, **kw):
    base = {"created": "2026-09-04T12:00:00Z", "label": None, "machine": "M4Pro",
            "platform": "Darwin 25.6.0 arm64", "model_sha256": "abc",
            "llama_tag": "b10456", "git_head": "be8bef4", "git_dirty": False,
            "rows": rows}
    base.update(kw)
    return base


class ParseBenchOutput(unittest.TestCase):
    def test_the_object_is_found_among_other_lines(self):
        # llama.cpp installs its own logger before bench_scorer can silence it,
        # so a stray line is not a corrupt run.
        out = "ggml: something\n" + json.dumps({"score_p50_ms": 2.1}) + "\nbye\n"
        self.assertEqual(bm.parse_bench_output(out), {"score_p50_ms": 2.1})

    def test_no_object_is_an_error_rather_than_an_empty_result(self):
        with self.assertRaises(RuntimeError):
            bm.parse_bench_output("ggml: only noise\n")


class LlamaTag(unittest.TestCase):
    """The tag is what a before/after across a bump is ABOUT, so reading it
    wrong makes two identical versions look different."""

    def _tag(self, text):
        with tempfile.TemporaryDirectory() as tmp:
            (Path(tmp) / "CMakeLists.txt").write_text(text, encoding="utf-8")
            return bm.llama_tag(tmp)

    def test_a_tag_closed_on_the_same_line_does_not_keep_the_paren(self):
        self.assertEqual(self._tag(
            "FetchContent_Declare(\n  llama\n  GIT_TAG b10456)\n"), "b10456")

    def test_a_tag_on_its_own_line_reads_the_same(self):
        self.assertEqual(self._tag(
            "FetchContent_Declare(\n  llama\n  GIT_TAG b10456\n)\n"), "b10456")

    def test_a_file_without_one_is_none_rather_than_a_guess(self):
        self.assertIsNone(self._tag("project(x)\n"))

    def test_a_missing_file_is_none(self):
        self.assertIsNone(bm.llama_tag("/nonexistent-dir-for-this-test"))


class ProvenanceDiff(unittest.TestCase):
    """A latency comparison is a claim about what changed. If the model, the
    machine or the llama.cpp tag also moved, the claim is not about the commit
    -- so the difference has to be stated, not left to be noticed."""

    def test_identical_provenance_reports_nothing(self):
        self.assertEqual(bm.provenance_diff(_run([]), _run([])), [])

    def test_a_llama_bump_is_named(self):
        d = bm.provenance_diff(_run([]), _run([], llama_tag="b10796"))
        self.assertEqual(d, [("llama_tag", "b10456", "b10796")])

    def test_a_different_model_is_named(self):
        d = bm.provenance_diff(_run([]), _run([], model_sha256="def"))
        self.assertIn("model_sha256", [f for f, _, _ in d])

    def test_a_dirty_tree_is_named(self):
        # Otherwise a clean `before` paired with a dirty `after` credits the
        # difference to the commit rather than to the working tree.
        d = bm.provenance_diff(_run([]), _run([], git_dirty=True))
        self.assertEqual(d, [("git_dirty", False, True)])


class CompareRows(unittest.TestCase):
    def test_rows_are_aligned_by_name_not_by_position(self):
        before = _run([_row("a", score_p50_ms=2.0), _row("b", score_p50_ms=12.0)])
        after = _run([_row("b", score_p50_ms=6.0), _row("a", score_p50_ms=1.0)])
        got = {(n, m): (b, a) for n, m, b, a, _, _, _ in bm.compare_rows(before, after)}
        self.assertEqual(got[("a", "score p50")], (2.0, 1.0))
        self.assertEqual(got[("b", "score p50")], (12.0, 6.0))

    def test_a_row_missing_from_one_side_is_skipped_not_compared_to_zero(self):
        before = _run([_row("a"), _row("gone")])
        after = _run([_row("a")])
        names = {n for n, _, _, _, _, _, _ in bm.compare_rows(before, after)}
        self.assertEqual(names, {"a"})

    def test_the_percentage_is_signed_so_lower_reads_as_negative(self):
        before = _run([_row("a", score_p50_ms=10.0)])
        after = _run([_row("a", score_p50_ms=8.0)])
        pct = {m: p for _, m, _, _, p, _, _ in bm.compare_rows(before, after)}
        self.assertAlmostEqual(pct["score p50"], -20.0)

    def test_a_zero_baseline_yields_no_percentage_rather_than_a_division(self):
        before = _run([_row("a", score_p50_ms=0.0)])
        after = _run([_row("a", score_p50_ms=1.0)])
        pct = {m: p for _, m, _, _, p, _, _ in bm.compare_rows(before, after)}
        self.assertIsNone(pct["score p50"])

    def test_a_metric_absent_from_a_row_is_skipped(self):
        # An older result file predating a metric must not compare as a change.
        before = _run([{"name": "a", "score_p50_ms": 2.0}])
        after = _run([{"name": "a", "score_p50_ms": 1.0}])
        got = {m for _, m, _, _, _, _, _ in bm.compare_rows(before, after)}
        self.assertEqual(got, {"score p50"})


class CompareOutput(unittest.TestCase):
    def _compare(self, before, after):
        with tempfile.TemporaryDirectory() as tmp:
            b = Path(tmp) / "b.json"
            a = Path(tmp) / "a.json"
            b.write_text(json.dumps(before), encoding="utf-8")
            a.write_text(json.dumps(after), encoding="utf-8")
            buf = io.StringIO()
            with contextlib.redirect_stdout(buf):
                bm.main(["compare", str(b), str(a)])
            return buf.getvalue()

    # The MARKER, not the word: the footer explains that lower is better on
    # every run, so asserting on "better" alone passes whatever the table says.
    # The first draft of these tests did exactly that and the noise-floor case
    # is what caught it.
    IMPROVED = "<-- better"
    REGRESSED = "<-- WORSE"

    def test_a_real_improvement_is_marked(self):
        out = self._compare(_run([_row("idle/decode", score_p50_ms=12.0)]),
                            _run([_row("idle/decode", score_p50_ms=6.0)]))
        self.assertIn(self.IMPROVED, out)
        self.assertNotIn(self.REGRESSED, out)

    def test_a_regression_is_marked(self):
        out = self._compare(_run([_row("idle/decode", score_p50_ms=6.0)]),
                            _run([_row("idle/decode", score_p50_ms=12.0)]))
        self.assertIn(self.REGRESSED, out)
        self.assertNotIn(self.IMPROVED, out)

    def test_a_change_inside_the_noise_floor_is_not_marked_either_way(self):
        out = self._compare(_run([_row("idle/decode", score_p50_ms=12.00)]),
                            _run([_row("idle/decode", score_p50_ms=11.76)]))  # -2%
        self.assertNotIn(self.IMPROVED, out)
        self.assertNotIn(self.REGRESSED, out)

    def test_two_clean_runs_of_the_same_commit_say_nothing_should_have_changed(self):
        out = self._compare(_run([_row("a")]), _run([_row("a")]))
        self.assertIn("nothing here that should have changed", out)

    def test_two_dirty_runs_of_the_same_commit_do_not_claim_the_source_matched(self):
        # The normal way this tool is used is edit -> rebuild -> re-run, all
        # before committing. Both runs then record the same commit while
        # measuring different source, and the first real change measured with
        # this tool was reported as "same commit -- so this diff is noise".
        out = self._compare(_run([_row("a")], git_dirty=True),
                            _run([_row("a")], git_dirty=True))
        self.assertIn("says nothing", out)
        self.assertNotIn("nothing here that should have changed", out)

    def test_a_swing_inside_the_measured_spread_is_not_marked(self):
        out = self._compare(
            _run([_row("idle/decode", score_p50_ms=12.0, score_p50_ms_spread_pct=25.0)]),
            _run([_row("idle/decode", score_p50_ms=8.9, score_p50_ms_spread_pct=25.0)]))
        self.assertNotIn(self.IMPROVED, out)
        self.assertNotIn(self.REGRESSED, out)

    def test_a_run_that_measured_no_noise_says_so(self):
        out = self._compare(_run([_row("a")]), _run([_row("a")]))
        self.assertIn("neither run measured its own noise", out)

    def test_a_run_that_did_measure_noise_does_not_carry_the_warning(self):
        out = self._compare(_run([_row("a", score_p50_ms_spread_pct=4.0)]),
                            _run([_row("a", score_p50_ms_spread_pct=4.0)]))
        self.assertNotIn("neither run measured its own noise", out)

    def test_a_changed_matrix_is_called_out(self):
        out = self._compare(_run([_row("a"), _row("gone")]), _run([_row("a")]))
        self.assertIn("the matrix changed", out)
        self.assertIn("gone", out)


class NoiseIsMeasuredNotAssumed(unittest.TestCase):
    """The lesson this file exists for. A fixed 8% threshold marked 14 of 20
    metrics as changes across two runs of the IDENTICAL build, the largest at
    -40.7%, because the idle rows measure the machine's power state as much as
    the code. A difference must clear the spread actually observed."""

    def test_median_of_an_even_sample_averages_the_middle_two(self):
        self.assertEqual(bm.median([1.0, 2.0, 3.0, 4.0]), 2.5)

    def test_median_of_an_odd_sample_is_the_middle(self):
        self.assertEqual(bm.median([3.0, 1.0, 2.0]), 2.0)

    def test_spread_is_the_full_range_over_the_median(self):
        self.assertAlmostEqual(bm.spread_pct([8.0, 10.0, 12.0]), 40.0)

    def test_a_single_sample_measures_no_spread_rather_than_none(self):
        # 0.0 here means "not measured", and is_change's floor is all that is
        # left -- which cmd_compare warns about rather than silently trusting.
        self.assertEqual(bm.spread_pct([10.0]), 0.0)

    def test_a_difference_inside_the_observed_spread_is_not_a_change(self):
        # -30% looks decisive until the two runs' own repeats ranged 20% and
        # 25%: that is exactly the idle/decode row's real behaviour.
        self.assertFalse(bm.is_change(-30.0, 20.0, 25.0))

    def test_a_difference_clearing_both_the_spread_and_the_floor_is_a_change(self):
        self.assertTrue(bm.is_change(-30.0, 5.0, 5.0))

    def test_a_tiny_difference_is_not_a_change_even_with_no_noise(self):
        self.assertFalse(bm.is_change(-3.0, 0.0, 0.0))

    def test_no_percentage_is_not_a_change(self):
        self.assertFalse(bm.is_change(None, 0.0, 0.0))


class Matrix(unittest.TestCase):
    def test_the_deployed_condition_is_in_the_default_matrix(self):
        # Every figure in bench_scorer.cc's header was measured at idle 0, the
        # one condition under which the dominant cost cannot appear. A matrix
        # that drifts back to only that reproduces the original mistake.
        self.assertTrue(any(r["idle_ms"] > 0 for r in bm.DEFAULT_MATRIX))

    def test_both_cost_modes_are_in_the_default_matrix(self):
        chars = {r["candidate_chars"] for r in bm.DEFAULT_MATRIX}
        self.assertIn(1, chars)  # decode-free
        self.assertIn(2, chars)  # decode-bearing

    def test_every_row_has_a_distinct_name_since_compare_keys_on_it(self):
        names = [r["name"] for r in bm.DEFAULT_MATRIX]
        self.assertEqual(len(names), len(set(names)))


if __name__ == "__main__":
    unittest.main()
