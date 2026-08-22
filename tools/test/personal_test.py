"""Generating the derived personal dictionary.

Every oracle is injected, so this module needs neither jieba nor pypinyin --
the same reason clean_test.py builds its own Lexicon.
"""
from __future__ import annotations

import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot import personal
from rime_copilot.clean import Lexicon
from rime_copilot.dictfile import Entry


def fake_segment(text: str) -> "list[str]":
    """Split on a literal '|' so tests state segmentation instead of guessing it."""
    return [piece for piece in text.split("|") if piece]


def fake_segment_by_char(text: str) -> "list[str]":
    """Every 2-character window, so a test corpus yields words without jieba."""
    return [text[index:index + 2] for index in range(0, len(text) - 1)]


class CorpusReaderTest(unittest.TestCase):
    def _corpus(self, records: "list[dict]", name: str = "a.jsonl") -> Path:
        directory = Path(tempfile.mkdtemp())
        with open(directory / name, "w", encoding="utf-8") as handle:
            for record in records:
                handle.write(json.dumps(record, ensure_ascii=False) + "\n")
        return directory

    def test_reads_text_from_every_jsonl(self):
        directory = self._corpus([{"id": "1", "src": "x", "ts": "2026-01-01", "text": "你好"}])
        with open(directory / "b.jsonl", "w", encoding="utf-8") as handle:
            handle.write(json.dumps({"id": "2", "text": "世界"}, ensure_ascii=False) + "\n")
        self.assertEqual(sorted(personal.iter_corpus_texts(directory)), ["世界", "你好"])

    def test_skips_blank_lines_and_records_without_text(self):
        directory = self._corpus([{"id": "1", "text": "你好"}, {"id": "2"}])
        with open(directory / "a.jsonl", "a", encoding="utf-8") as handle:
            handle.write("\n")
        self.assertEqual(list(personal.iter_corpus_texts(directory)), ["你好"])

    def test_a_malformed_line_is_refused_not_skipped(self):
        # A corpus file half-written by an interrupted `ingest` must not
        # silently yield a smaller vocabulary that still reports success.
        directory = self._corpus([{"id": "1", "text": "你好"}])
        with open(directory / "a.jsonl", "a", encoding="utf-8") as handle:
            handle.write("{not json\n")
        with self.assertRaises(ValueError) as caught:
            list(personal.iter_corpus_texts(directory))
        self.assertIn("a.jsonl", str(caught.exception))
        self.assertIn("line 2", str(caught.exception))

    def test_missing_directory_yields_nothing(self):
        self.assertEqual(list(personal.iter_corpus_texts(Path("/nonexistent/corpus"))), [])


class MineTest(unittest.TestCase):
    def test_counts_words_the_segmenter_produces(self):
        counts = personal.mine(["车端|联调", "车端|重构"], fake_segment, min_count=1)
        self.assertEqual(counts, {"车端": 2, "联调": 1, "重构": 1})

    def test_min_count_drops_the_singletons(self):
        counts = personal.mine(["车端|联调", "车端|重构"], fake_segment, min_count=2)
        self.assertEqual(counts, {"车端": 2})

    def test_rejects_words_shorter_than_min_chars(self):
        counts = personal.mine(["的|车端", "的|车端"], fake_segment, min_count=1)
        self.assertEqual(counts, {"车端": 2})

    def test_rejects_words_longer_than_max_chars(self):
        long_word = "一" * 9
        counts = personal.mine([f"{long_word}|车端"], fake_segment, min_count=1)
        self.assertEqual(counts, {"车端": 1})

    def test_rejects_anything_not_entirely_han(self):
        # Latin, digits and punctuation reach the segmenter; only Han runs are
        # spellable by a pinyin schema, so anything else is not a candidate.
        # `你好，` is REJECTED rather than trimmed to `你好`: a piece carrying
        # punctuation is a piece the segmenter got wrong, and trimming it would
        # invent a word nobody wrote.
        counts = personal.mine(["udeer|车端|3D|你好，|车端"], fake_segment, min_count=1)
        self.assertEqual(counts, {"车端": 2})


class NormaliseTest(unittest.TestCase):
    def test_the_most_frequent_word_normalises_to_one(self):
        self.assertEqual(personal.normalise({"a": 10, "b": 1})["a"], 1.0)

    def test_order_is_preserved(self):
        values = personal.normalise({"a": 100, "b": 10, "c": 2})
        self.assertGreater(values["a"], values["b"])
        self.assertGreater(values["b"], values["c"])

    def test_it_is_logarithmic_not_linear(self):
        # The point of the log: a 100x count is not a 100x weight. Linear
        # rescaling flattens a long tail -- dictdb.scale_weights measured
        # 1.07% of 542,928 entries above the floor and the rest tied.
        values = personal.normalise({"a": 100, "b": 10})
        self.assertGreater(values["b"], 0.4)

    def test_a_single_word_normalises_to_one(self):
        self.assertEqual(personal.normalise({"a": 5}), {"a": 1.0})

    def test_empty_input_is_empty_output(self):
        self.assertEqual(personal.normalise({}), {})

    def test_counts_below_one_do_not_produce_a_negative_fraction(self):
        # log(1+c) with c >= 1 is always positive; the guard is against a
        # zero or negative count reaching here from a hand-edited source.
        self.assertGreaterEqual(personal.normalise({"a": 0, "b": 4})["a"], 0.0)


class ToWeightTest(unittest.TestCase):
    def test_one_maps_to_the_ceiling(self):
        self.assertEqual(personal.to_weight(1.0), personal.WEIGHT_CEILING)

    def test_zero_maps_to_the_floor(self):
        self.assertEqual(personal.to_weight(0.0), personal.WEIGHT_FLOOR)

    def test_the_floor_clears_the_flat_100_tier(self):
        # ext + tencent + sogou are 1.47M entries at a flat weight of 100.
        # A personal entry below that is invisible, which is the whole defect
        # this module exists to fix.
        self.assertGreater(personal.WEIGHT_FLOOR, 100)

    def test_the_result_is_an_int(self):
        self.assertIsInstance(personal.to_weight(0.37), int)

    def test_out_of_range_fractions_are_clamped(self):
        self.assertEqual(personal.to_weight(-1.0), personal.WEIGHT_FLOOR)
        self.assertEqual(personal.to_weight(2.0), personal.WEIGHT_CEILING)


def oracle(known=None, chart=None):
    """A clean.Lexicon with no jieba behind it. `chart=None` disables the check."""
    return Lexicon(known=known or {}, chart=chart,
                   segment=lambda word: [word], tags=lambda word: [])


def fake_reading(word: str) -> str:
    """One syllable per character, so a misaligned reading is visible in a test."""
    return " ".join(f"py{index}" for index, _ in enumerate(word))


class BuildEntriesTest(unittest.TestCase):
    def test_a_custom_entry_keeps_its_own_pinyin(self):
        entries = personal.build_entries(
            [Entry("车端", "che duan", 40)], {}, oracle(), fake_reading)
        self.assertEqual([(e.word, e.pinyin) for e in entries], [("车端", "che duan")])

    def test_a_mined_word_gets_a_generated_reading(self):
        entries = personal.build_entries([], {"辅堂": 35}, oracle(), fake_reading)
        self.assertEqual([(e.word, e.pinyin) for e in entries], [("辅堂", "py0 py1")])

    def test_a_word_in_both_sources_produces_exactly_one_entry(self):
        entries = personal.build_entries(
            [Entry("车端", "che duan", 40)], {"车端": 16}, oracle(), fake_reading)
        self.assertEqual(len(entries), 1)
        self.assertEqual(entries[0].pinyin, "che duan")

    def test_a_heteronym_pair_in_custom_survives_as_two_entries(self):
        # Both readings are real; a mined count must not add a third.
        entries = personal.build_entries(
            [Entry("重", "zhong", 10), Entry("重", "chong", 4)],
            {"重": 50}, oracle(), fake_reading)
        self.assertEqual(sorted(e.pinyin for e in entries), ["chong", "zhong"])

    def test_a_heteronym_pairs_readings_keep_their_own_weight(self):
        # Finding 4: build_entries used to collapse a heteronym pair to one
        # word-level count (the max of its readings), so a rarer reading was
        # promoted to the same weight as the reading the user actually types
        # most. Each reading must instead be normalised on its OWN commit
        # count -- 10 vs 4, not 10 vs 10.
        entries = personal.build_entries(
            [Entry("重", "zhong", 10), Entry("重", "chong", 4)],
            {}, oracle(), fake_reading)
        by_pinyin = {e.pinyin: e.weight for e in entries}
        self.assertEqual(len(by_pinyin), 2)
        self.assertGreater(by_pinyin["zhong"], by_pinyin["chong"])
        # zhong is the custom-side maximum here, so it clears the ceiling;
        # chong's 4 commits must not borrow zhong's weight to get there too --
        # that is exactly the bug this test guards against.
        self.assertEqual(by_pinyin["zhong"], personal.WEIGHT_CEILING)
        self.assertLess(by_pinyin["chong"], personal.WEIGHT_CEILING)
        self.assertGreater(by_pinyin["chong"], personal.WEIGHT_FLOOR)

    def test_the_most_committed_custom_word_reaches_the_ceiling(self):
        entries = personal.build_entries(
            [Entry("一起", "yi qi", 589), Entry("一台", "yi tai", 129)],
            {}, oracle(), fake_reading)
        by_word = {e.word: e.weight for e in entries}
        self.assertEqual(by_word["一起"], personal.WEIGHT_CEILING)
        self.assertGreater(by_word["一台"], personal.WEIGHT_FLOOR)
        self.assertLess(by_word["一台"], personal.WEIGHT_CEILING)

    def test_every_weight_clears_the_flat_hundred_tier(self):
        entries = personal.build_entries(
            [Entry("甲", "jia", 3), Entry("乙", "yi", 2877)], {}, oracle(), fake_reading)
        self.assertTrue(all(e.weight > 100 for e in entries))

    def test_the_two_sources_are_normalised_separately(self):
        # Sogou's lifetime counts and the corpus's are on different scales.
        # Each source's own maximum is what maps to the ceiling, so the top of
        # the corpus is not buried by the top of the export.
        entries = personal.build_entries(
            [Entry("甲", "jia", 2877)], {"乙": 35}, oracle(), fake_reading)
        by_word = {e.word: e.weight for e in entries}
        self.assertEqual(by_word["甲"], personal.WEIGHT_CEILING)
        self.assertEqual(by_word["乙"], personal.WEIGHT_CEILING)

    def test_a_word_in_both_takes_the_larger_fraction(self):
        entries = personal.build_entries(
            [Entry("甲", "jia", 1), Entry("丙", "bing", 100)],
            {"甲": 50, "丁": 50}, oracle(), fake_reading)
        by_word = {e.word: e.weight for e in entries}
        # 甲 is the corpus maximum, so it reaches the ceiling despite being the
        # custom minimum.
        self.assertEqual(by_word["甲"], personal.WEIGHT_CEILING)

    def test_a_jieba_unknown_fragment_shape_is_dropped(self):
        entries = personal.build_entries([], {"的时": 91}, oracle(), fake_reading)
        self.assertEqual(entries, [])

    def test_a_jieba_known_word_survives_its_fragment_shape(self):
        # R9's lesson from clean.py: jieba is a POSITIVE oracle. 好的, 是的,
        # 那个 are words; two earlier drafts of that chain deleted them by
        # checking shape before consulting the dictionary.
        entries = personal.build_entries(
            [], {"好的": 184}, oracle(known={"好的": 500}), fake_reading)
        self.assertEqual([e.word for e in entries], ["好的"])

    def test_the_fragment_guard_does_not_touch_custom_entries(self):
        # custom.dict.yaml has already been through `clean`; re-judging it here
        # would apply the chain twice, without R9's commit-count escape.
        entries = personal.build_entries(
            [Entry("是的", "shi de", 60)], {}, oracle(), fake_reading)
        self.assertEqual([e.word for e in entries], ["是的"])

    def test_a_character_outside_the_chart_is_dropped(self):
        entries = personal.build_entries(
            [], {"車端": 5}, oracle(chart=set("车端")), fake_reading)
        self.assertEqual(entries, [])

    def test_output_is_ordered_by_first_character_then_weight_descending(self):
        entries = personal.build_entries(
            [Entry("甲乙", "a b", 5), Entry("甲丙", "a c", 50), Entry("乙丁", "d e", 20)],
            {}, oracle(), fake_reading)
        self.assertEqual([e.word for e in entries], ["乙丁", "甲丙", "甲乙"])

    def test_both_sources_empty_produces_no_entries(self):
        self.assertEqual(personal.build_entries([], {}, oracle(), fake_reading), [])


class ComputeVersionTest(unittest.TestCase):
    """Finding 1: `version` must answer "which inputs", not "when did this
    run" -- a date-derived version churned on the first `update` of every
    calendar day even on byte-identical output, which is exactly what a
    vaulted, frequently-regenerated file must not do (see vault.py's
    plan_restore, which compares by sha256).
    """

    def test_looks_like_a_short_hex_version(self):
        root = Path(tempfile.mkdtemp())
        version = personal.compute_version(root / "custom.dict.yaml", root / "corpus")
        self.assertRegex(version, r"^[0-9a-f]{12}$")

    def test_stable_across_repeated_calls_on_unchanged_inputs(self):
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("车端\tche duan\t40\n", encoding="utf-8")
        corpus = root / "corpus"
        corpus.mkdir()
        (corpus / "a.jsonl").write_text(
            json.dumps({"id": "1", "text": "你好"}, ensure_ascii=False) + "\n",
            encoding="utf-8")
        first = personal.compute_version(custom, corpus)
        second = personal.compute_version(custom, corpus)
        self.assertEqual(first, second)

    def test_changes_when_the_custom_dictionary_changes(self):
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("车端\tche duan\t40\n", encoding="utf-8")
        before = personal.compute_version(custom, root / "no-corpus")
        custom.write_text("车端\tche duan\t41\n", encoding="utf-8")
        after = personal.compute_version(custom, root / "no-corpus")
        self.assertNotEqual(before, after)

    def test_changes_when_the_corpus_changes(self):
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("车端\tche duan\t40\n", encoding="utf-8")
        corpus = root / "corpus"
        corpus.mkdir()
        before = personal.compute_version(custom, corpus)
        (corpus / "a.jsonl").write_text(
            json.dumps({"id": "1", "text": "你好"}, ensure_ascii=False) + "\n",
            encoding="utf-8")
        after = personal.compute_version(custom, corpus)
        self.assertNotEqual(before, after)

    def test_missing_custom_and_missing_corpus_still_produce_a_version(self):
        # Both are legitimate inputs (generate() accepts either being absent);
        # neither should make this raise.
        root = Path(tempfile.mkdtemp())
        version = personal.compute_version(root / "absent.dict.yaml", root / "absent-corpus")
        self.assertRegex(version, r"^[0-9a-f]{12}$")


class GenerateTest(unittest.TestCase):
    def test_writes_a_dictionary_with_a_header_and_entries(self):
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("---\nname: custom\n...\n\n车端\tche duan\t40\n", encoding="utf-8")
        corpus = root / "corpus"
        corpus.mkdir()
        (corpus / "a.jsonl").write_text(
            json.dumps({"id": "1", "text": "车端联调"}, ensure_ascii=False) + "\n"
            + json.dumps({"id": "2", "text": "车端联调"}, ensure_ascii=False) + "\n",
            encoding="utf-8")
        output = root / "personal.dict.yaml"

        count = personal.generate(
            custom_path=custom, corpus=corpus, chart_path=None, output=output,
            version="2026-08-22",
            segment=fake_segment_by_char, reading=fake_reading, known={})

        self.assertGreater(count, 0)
        text = output.read_text(encoding="utf-8")
        self.assertIn("name: personal", text)
        self.assertIn("sort: by_weight", text)
        self.assertIn('version: "2026-08-22"', text)
        self.assertIn("车端\tche duan\t", text)

    def test_no_corpus_still_produces_the_custom_half(self):
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("---\nname: custom\n...\n\n车端\tche duan\t40\n", encoding="utf-8")
        output = root / "personal.dict.yaml"
        count = personal.generate(
            custom_path=custom, corpus=root / "absent", chart_path=None,
            output=output, version="2026-08-22",
            segment=fake_segment_by_char, reading=fake_reading, known={})
        self.assertEqual(count, 1)

    def test_the_output_is_re_readable_by_the_package_s_own_parser(self):
        # write_dict and read_entries must agree, or the dictionary this
        # generates is one build_copilot and librime disagree about.
        from rime_copilot.dictfile import read_entries
        root = Path(tempfile.mkdtemp())
        custom = root / "custom.dict.yaml"
        custom.write_text("---\nname: custom\n...\n\n车端\tche duan\t40\n", encoding="utf-8")
        output = root / "personal.dict.yaml"
        personal.generate(custom_path=custom, corpus=root / "absent", chart_path=None,
                          output=output, version="2026-08-22",
                          segment=fake_segment_by_char, reading=fake_reading, known={})
        entries = read_entries(output)
        self.assertEqual([(e.word, e.pinyin) for e in entries], [("车端", "che duan")])
        self.assertGreater(entries[0].weight, 100)

    def test_a_half_injected_oracle_is_refused(self):
        # segment and known are one oracle. Accepting half of it silently
        # discarded the half given and loaded the real jieba dictionary --
        # expensive, and nothing said so.
        root = Path(tempfile.mkdtemp())
        for kwargs in ({"segment": fake_segment_by_char}, {"known": {}}):
            with self.subTest(**kwargs):
                with self.assertRaises(ValueError) as caught:
                    personal.generate(
                        custom_path=root / "absent", corpus=root / "absent",
                        chart_path=None, output=root / "out.dict.yaml",
                        version="2026-08-22", reading=fake_reading, **kwargs)
                self.assertIn("segment and known", str(caught.exception))


if __name__ == "__main__":
    unittest.main()
