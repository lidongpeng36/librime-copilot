"""Weight merging and prefix/suffix splitting.

The `top` stacking test is the important one: overwriting instead of stacking
was measured to invert real frequencies (建立 193 > 建设 45 > 建议 4, the exact
reverse of reality).
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot.dictfile import Entry
from rime_copilot.dictdb import Source, load_sources, merge, scale_weights, write_pairs


def entry(word, weight, pinyin="x"):
    return Entry(word, pinyin, weight)


class LoadSources(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, config) -> Path:
        path = self.dir / "dict.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        return path

    def test_reads_flags(self):
        sources = load_sources(self.write([
            {"dict": "~/a.dict.yaml", "top": True},
            {"dict": "/b.dict.yaml", "scale": 2},
            {"dict": "/c.dict.yaml", "range": [1, 200]},
        ]))
        self.assertEqual(3, len(sources))
        self.assertTrue(sources[0].top)
        self.assertEqual(Path.home() / "a.dict.yaml", sources[0].path)
        self.assertEqual(2, sources[1].scale)
        self.assertEqual((1, 200), sources[2].scale_range)

    def test_scale_and_range_together_is_an_error(self):
        with self.assertRaises(ValueError):
            load_sources(self.write([{"dict": "/a", "scale": 2, "range": [1, 2]}]))


class Merge(unittest.TestCase):
    def test_dedups_on_word_and_pinyin_keeping_the_largest(self):
        a = Source(Path("/a"))
        b = Source(Path("/b"))
        merged = merge([(a, [entry("建议", 4)]), (b, [entry("建议", 900)])])
        self.assertEqual([entry("建议", 900)], merged)

    def test_same_word_different_pinyin_is_two_entries(self):
        a = Source(Path("/a"))
        merged = merge([(a, [Entry("空落落", "kong luo luo", 5),
                             Entry("空落落", "kong lao lao", 3)])])
        self.assertEqual(2, len(merged))

    def test_top_stacks_above_everything_and_keeps_internal_order(self):
        plain = Source(Path("/plain"))
        top = Source(Path("/top"), top=True)
        merged = merge([
            (plain, [entry("普通", 1000)]),
            (top, [entry("建立", 193), entry("建设", 45), entry("建议", 4)]),
        ])
        by_word = {e.word: e.weight for e in merged}
        self.assertGreater(by_word["建议"], by_word["普通"])
        # Real frequency order survives the boost.
        self.assertGreater(by_word["建立"], by_word["建设"])
        self.assertGreater(by_word["建设"], by_word["建议"])

    def test_top_adds_to_an_existing_weight_rather_than_replacing_it(self):
        plain = Source(Path("/plain"))
        top = Source(Path("/top"), top=True)
        merged = merge([(plain, [entry("建议", 500)]), (top, [entry("建议", 4)])])
        self.assertEqual(500 + 500 + 4, merged[0].weight)  # ceiling + existing + own

    def test_sorted_by_first_character_then_descending_weight(self):
        a = Source(Path("/a"))
        merged = merge([(a, [entry("建议", 4), entry("阿安", 9), entry("建立", 90)])])
        self.assertEqual(["建立", "建议", "阿安"], [e.word for e in merged])

    def test_fractional_scale_is_not_truncated(self):
        source = Source(Path("/s"), scale=0.5)
        merged = merge([(source, [entry("甲", 11), entry("乙", 10)])])
        by_word = {e.word: e.weight for e in merged}
        self.assertEqual(5.5, by_word["甲"])
        self.assertEqual(5.0, by_word["乙"])
        self.assertNotEqual(by_word["甲"], by_word["乙"])  # the tie truncation caused


class ScaleWeights(unittest.TestCase):
    def test_linear_min_max(self):
        scaled = scale_weights([entry("a", 0), entry("b", 100)], 1, 201)
        self.assertEqual([1, 201], [e.weight for e in scaled])

    def test_all_equal_collapses_to_a_constant(self):
        scaled = scale_weights([entry("a", 7), entry("b", 7)], 1, 200)
        self.assertEqual([100, 100], [e.weight for e in scaled])

    def test_empty(self):
        self.assertEqual([], scale_weights([], 1, 200))

    def test_matches_the_original_formula_where_reassociation_would_drift(self):
        # Hoisting the span out of this expression changes the result by 1 here.
        scaled = scale_weights([entry("a", 202)] + [entry("z", 1), entry("y", 336)], 1, 201)
        self.assertEqual(121, scaled[0].weight)


class LogBoost(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def write(self, config) -> Path:
        path = self.dir / "dict.json"
        path.write_text(json.dumps(config), encoding="utf-8")
        return path

    def test_own_commits_now_move_the_result(self):
        # ceiling = 900. 上线 is the most-committed personal entry so it gets the
        # full ceiling boost; 一份's two commits get log1p(2)/log1p(2877) of it.
        #   上线  900 + 500 + 900.0    = 2300.0
        #   一份  900 + 900 + 124.139 = 1924.139
        # Pinned to exact values, not just order: with boost=None every one of
        # this class's tests but `test_a_dominant_public_weight_can_still_win`
        # still passes on the unboosted numbers (4277 vs 1802 here), so
        # relative-order assertions alone do not detect the feature.
        sources = [
            (Source(path=Path("/public.dict.yaml")),
             [entry("上线", 500), entry("一份", 900)]),
            (Source(path=Path("/personal.dict.yaml"), top=True, boost="log"),
             [entry("上线", 2877), entry("一份", 2)]),
        ]
        merged = {e.word: e.weight for e in merge(sources)}
        self.assertGreater(merged["上线"], merged["一份"])
        self.assertEqual(2300.0, merged["上线"])
        self.assertAlmostEqual(1924.139, merged["一份"], places=3)

    def test_public_weight_breaks_a_tie_in_own_commits(self):
        # Equal commits mean an equal boost, so the public weight decides.
        #   甲  900 +  10 + 900.0 = 1810.0
        #   乙  900 + 900 + 900.0 = 2700.0
        sources = [
            (Source(path=Path("/public.dict.yaml")),
             [entry("甲", 10), entry("乙", 900)]),
            (Source(path=Path("/personal.dict.yaml"), top=True, boost="log"),
             [entry("甲", 50), entry("乙", 50)]),
        ]
        merged = {e.word: e.weight for e in merge(sources)}
        self.assertGreater(merged["乙"], merged["甲"])
        self.assertEqual(1810.0, merged["甲"])
        self.assertEqual(2700.0, merged["乙"])

    def test_a_dominant_public_weight_can_still_win(self):
        """The known limit, pinned so it stays a decision rather than a surprise.

        The boost gives the personal term the same range as the public term
        (both 0..ceiling); it does not make it lexicographically first. A word
        whose public weight is near the ceiling still outranks a heavily
        committed one whose public weight is tiny:
            甲  900 +  10 + 900.0 = 1810.0
            乙  900 + 900 +  78.3 = 1878.3
        In the real lexicon this is rare — personal entries mostly carry public
        weights far below the 19.26M ceiling — which is why co-equal ranges are
        enough. If it ever bites, that is a threshold conversation, not a bug.
        """
        sources = [
            (Source(path=Path("/public.dict.yaml")),
             [entry("甲", 10), entry("乙", 900)]),
            (Source(path=Path("/personal.dict.yaml"), top=True, boost="log"),
             [entry("甲", 2877), entry("乙", 1)]),
        ]
        merged = {e.word: e.weight for e in merge(sources)}
        self.assertGreater(merged["乙"], merged["甲"])
        self.assertEqual(1810.0, merged["甲"])
        self.assertAlmostEqual(1878.323, merged["乙"], places=3)

    def test_without_the_flag_the_old_stacking_is_unchanged(self):
        sources = [
            (Source(path=Path("/public.dict.yaml")), [entry("上线", 500)]),
            (Source(path=Path("/personal.dict.yaml"), top=True), [entry("上线", 7)]),
        ]
        merged = {e.word: e.weight for e in merge(sources)}
        self.assertEqual(500 + 500 + 7, merged["上线"])

    def test_load_sources_reads_the_flag(self):
        sources = load_sources(self.write([
            {"dict": "/a.dict.yaml", "top": True, "boost": "log"},
            {"dict": "/b.dict.yaml", "top": True},
        ]))
        self.assertEqual("log", sources[0].boost)
        self.assertIsNone(sources[1].boost)

    def test_boost_requires_top(self):
        with self.assertRaises(ValueError) as caught:
            load_sources(self.write([{"dict": "/a.dict.yaml", "boost": "log"}]))
        self.assertIn("top", str(caught.exception))

    def test_unknown_boost_is_refused(self):
        with self.assertRaises(ValueError):
            load_sources(self.write([{"dict": "/a.dict.yaml", "top": True,
                                      "boost": "sqrt"}]))

    def test_boost_with_scale_is_refused(self):
        # _apply_shaping runs before the boost, so a linear rescale would
        # compose non-linearly with the log normalisation.
        with self.assertRaises(ValueError) as caught:
            load_sources(self.write([{"dict": "/a.dict.yaml", "top": True,
                                      "boost": "log", "scale": 2}]))
        self.assertIn("boost", str(caught.exception))

    def test_boost_with_range_is_refused(self):
        with self.assertRaises(ValueError) as caught:
            load_sources(self.write([{"dict": "/a.dict.yaml", "top": True,
                                      "boost": "log", "range": [1, 200]}]))
        self.assertIn("boost", str(caught.exception))


class WritePairs(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.out = Path(self.tmp.name) / "pairs.tsv"

    def tearDown(self):
        self.tmp.cleanup()

    def lines(self):
        return self.out.read_text(encoding="utf-8").splitlines()

    def test_splits_at_every_point(self):
        write_pairs([entry("建议书", 5)], self.out, float("inf"))
        self.assertEqual(["建\t议书\t5", "建议\t书\t5"], self.lines())

    def test_single_character_word_yields_nothing(self):
        write_pairs([entry("建", 5)], self.out, float("inf"))
        self.assertEqual([], self.lines())

    def test_duplicate_pairs_from_multiple_readings_keep_the_largest(self):
        # 空落落 has two readings; both split to the same (prefix, suffix).
        write_pairs([Entry("空落", "kong luo", 5), Entry("空落", "kong lao", 9)],
                    self.out, float("inf"))
        self.assertEqual(["空\t落\t9"], self.lines())

    def test_per_key_cap(self):
        write_pairs([entry("建议", 9), entry("建立", 8), entry("建设", 7)], self.out, 2)
        prefixes = [line.split("\t")[0] for line in self.lines()]
        self.assertEqual(2, prefixes.count("建"))

    # --- the ordering invariant the runtime cap depends on -------------------
    #
    # Everything below pins ONE property: within a prefix, continuations are
    # emitted weight-descending. Nothing in this file used to check it, and
    # test_per_key_cap above counts survivors without asking WHICH survived --
    # so the order could invert silently.
    #
    # It matters outside this module. copilot/max_candidates caps the runtime
    # lookup inside DBProvider::Lookup (src/db_provider.h), and that cap is
    # lossless ONLY because the stored list is weight-descending: the global
    # top-K is then exactly the union of each key's top-K. Measured, a
    # 10356-continuation key came back with zero inversions, and capping it at
    # 100 cut a 2.43 ms prediction to 0.009 ms with identical output. Break the
    # order here and that cap silently becomes "keep an arbitrary K".
    #
    # The property is EMERGENT, not asserted anywhere in the implementation:
    # merge() sorts by (first character, -weight); write_pairs walks that order
    # into a plain dict, whose iteration order is insertion order; and
    # `if e.weight > block.get(key, 0)` means the first insertion (the heaviest
    # word introducing that pair) is the one that sticks. Three independent
    # steps, any of which someone could change without noticing this.

    def test_continuations_are_emitted_weight_descending(self):
        # write_pairs' documented precondition is merge()'s output order.
        words = [entry("建议", 90), entry("建立", 80), entry("建设", 70),
                 entry("建国", 60), entry("建造", 50)]
        write_pairs(words, self.out, float("inf"))
        weights = [int(line.split("\t")[2]) for line in self.lines()
                   if line.split("\t")[0] == "建"]
        self.assertEqual([90, 80, 70, 60, 50], weights)
        self.assertEqual(sorted(weights, reverse=True), weights)

    def test_per_key_cap_keeps_the_HEAVIEST_k_not_an_arbitrary_k(self):
        # The consequence the runtime cap rides on. If write_pairs ever emits
        # in some other order this fails, and so does the claim that
        # copilot/max_candidates is lossless.
        words = [entry("建议", 90), entry("建立", 80), entry("建设", 70),
                 entry("建国", 60)]
        write_pairs(words, self.out, 2)
        kept = [line.split("\t")[1] for line in self.lines()
                if line.split("\t")[0] == "建"]
        self.assertEqual(["议", "立"], kept)

    def test_duplicate_pairs_keep_the_largest_IN_MERGE_ORDER(self):
        # The third step of the invariant, and the one nothing tested.
        # test_duplicate_pairs_from_multiple_readings_keep_the_largest above
        # feeds 5 then 9 -- ASCENDING, which is not the order write_pairs
        # documents itself as receiving. In ascending order "keep the largest"
        # and "keep the last" agree, so that test cannot tell them apart:
        # replacing `if e.weight > block.get(key, 0)` with a plain assignment
        # leaves the whole suite green. Descending is merge()'s real output,
        # and there the two disagree -- last-wins would store 5.
        write_pairs([Entry("空落", "kong luo", 9), Entry("空落", "kong lao", 5)],
                    self.out, float("inf"))
        self.assertEqual(["空\t落\t9"], self.lines())

    def test_the_order_survives_a_real_merge(self):
        # The two halves pinned together: merge() is what actually establishes
        # the precondition, so a change to ITS sort key has to fail something.
        source = Source(Path("/s"))
        entries = [entry("建设", 70), entry("建议", 90), entry("建立", 80)]
        merged = merge([(source, entries)])
        write_pairs(merged, self.out, float("inf"))
        weights = [int(line.split("\t")[2]) for line in self.lines()
                   if line.split("\t")[0] == "建"]
        self.assertEqual(sorted(weights, reverse=True), weights)

    def test_every_line_survives_build_copilot_column_split(self):
        write_pairs([entry("建议书", 5)], self.out, float("inf"))
        for line in self.lines():
            parts = line.split()
            self.assertEqual(3, len(parts), line)
            int(parts[2])


if __name__ == "__main__":
    unittest.main()
