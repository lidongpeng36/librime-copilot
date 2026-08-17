import unittest

from rime_corpus import metrics


class BucketTest(unittest.TestCase):
    def test_hit_zero_is_bucket_a(self):
        self.assertEqual(metrics.bucket(0, ["故意"], 32), "A")

    def test_inside_window_with_ascii_first_candidate_is_bucket_b(self):
        """cands[0] is NOT all-Han: RawInputFilter's deliberate raw-input-
        first insertion (src/filters.cc:139). This is policy the plugin
        chose, not headroom a re-ranker could take -- it must never be
        counted as bucket C. Pinning this distinction explicitly because
        merging it into "opportunity" once produced a bound of 67.1% where
        the truth was 27.5%."""
        self.assertEqual(metrics.bucket(1, ["guyide", "故意的"], 32), "B")

    def test_inside_window_with_han_first_candidate_is_bucket_c(self):
        """cands[0] IS all-Han but wrong: the only real re-ranking
        opportunity. Same 0 < hit < window as the ASCII case above -- the
        Han-ness of cands[0] is the ENTIRE distinction between B and C."""
        self.assertEqual(metrics.bucket(1, ["顾忌", "故意"], 32), "C")

    def test_mixed_script_first_candidate_is_bucket_b_not_c(self):
        # Mixing ASCII and Han is still not "all Han".
        self.assertEqual(metrics.bucket(1, ["guyide故意", "故意"], 32), "B")

    def test_hit_at_or_beyond_window_is_bucket_d(self):
        self.assertEqual(metrics.bucket(32, ["a"] * 33, 32), "D")

    def test_hit_absent_is_bucket_d(self):
        self.assertEqual(metrics.bucket(-1, [], 32), "D")

    def test_empty_candidate_list_with_hit_inside_window_is_bucket_b(self):
        # "" is not all-Han, so an empty (truncated-away) cands list must not
        # silently fall into C.
        self.assertEqual(metrics.bucket(5, [], 32), "B")


class SeverityTest(unittest.TestCase):
    def test_exact_match_is_correct(self):
        self.assertEqual(metrics.severity("故意的", "故意的"), "correct")

    def test_picking_a_prefix_of_gold_is_prefix(self):
        # e.g. promoting "运行" when gold was "运行了" -- the user keeps
        # typing and lands on the right sentence anyway. Not harmful.
        self.assertEqual(metrics.severity("运行", "运行了"), "prefix")

    def test_picking_gold_plus_more_is_extension(self):
        self.assertEqual(metrics.severity("运行了很久", "运行了"), "extension")

    def test_unrelated_word_is_wrong_word(self):
        # The only harmful case: neither string is a prefix of the other.
        self.assertEqual(metrics.severity("估计", "故意"), "wrong-word")

    def test_empty_picked_against_nonempty_gold_is_wrong_word(self):
        self.assertEqual(metrics.severity("", "故意"), "wrong-word")


class SummarizeTest(unittest.TestCase):
    @staticmethod
    def _resp(id_, status, segments):
        return {"id": id_, "status": status, "segments": segments}

    def test_four_way_split_and_oracle_bound(self):
        responses = [
            self._resp("a", "ok", [{"hit": 0, "cands": ["x"]}]),  # A
            self._resp("b", "ok", [{"hit": 1, "cands": ["ab", "x"]}]),  # B
            self._resp("c", "ok", [{"hit": 1, "cands": ["顾忌", "故意"]}]),  # C
            self._resp("d", "ok", [{"hit": -1, "cands": []}]),  # D
        ]
        out = metrics.summarize(responses, window=32)
        b = out["buckets"]
        self.assertEqual((b["A"], b["B"], b["C"], b["D"], b["total"]), (1, 1, 1, 1, 4))
        self.assertAlmostEqual(b["oracle_bound"], 0.25)

    def test_error_responses_are_excluded_entirely(self):
        responses = [
            self._resp("a", "error", [{"hit": 0, "cands": ["x"]}]),
            self._resp("b", "ok", [{"hit": 0, "cands": ["x"]}]),
        ]
        out = metrics.summarize(responses, window=32)
        self.assertEqual(out["buckets"]["total"], 1)
        self.assertEqual(out["errors"], 1)

    def test_divergence_rate_excludes_errors_from_the_denominator(self):
        responses = [
            self._resp("a", "diverged", [{"hit": -1, "cands": []}]),
            self._resp("b", "ok", [{"hit": 0, "cands": ["x"]}]),
            self._resp("c", "error", []),
        ]
        out = metrics.summarize(responses, window=32)
        self.assertAlmostEqual(out["divergence_rate"], 0.5)

    def test_empty_input_does_not_divide_by_zero(self):
        out = metrics.summarize([], window=32)
        self.assertEqual(out["buckets"]["oracle_bound"], 0.0)
        self.assertEqual(out["divergence_rate"], 0.0)

    def test_gold_rank_breakdown_is_keyed_by_hit_within_bucket_c(self):
        responses = [
            self._resp("a", "ok", [{"hit": 1, "cands": ["顾忌", "故意"]}]),
            self._resp("b", "ok", [{"hit": 1, "cands": ["顾忌", "故意"]}]),
            self._resp("c", "ok", [{"hit": 3, "cands": ["顾忌", "估计", "意外", "故意"]}]),
            self._resp("d", "ok", [{"hit": 1, "cands": ["ab", "故意"]}]),  # bucket B, excluded
        ]
        out = metrics.summarize(responses, window=32)
        self.assertEqual(out["gold_rank_in_c"], {1: 2, 3: 1})


if __name__ == "__main__":
    unittest.main()
