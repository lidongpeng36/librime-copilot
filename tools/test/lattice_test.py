import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import lattice


def build(*entries: tuple[str, str]) -> lattice.Lexicon:
    """(text, "space separated readings") pairs."""
    lex = lattice.Lexicon()
    for text, reading in entries:
        lex.add(text, reading.split())
    return lex


CHARS = (
    ("高", "gao"),
    ("屋", "wu"),
    ("建", "jian"),
    ("瓴", "ling"),
)


class LexiconTest(unittest.TestCase):
    def test_spans_are_matched_on_text_and_reading_together(self):
        lex = build(("银行", "yin hang"))
        self.assertTrue(lex.has("银行", ("yin", "hang")))
        # right text, wrong reading -- 行 is a 多音字 and the other reading is
        # a different entry, not the same one.
        self.assertFalse(lex.has("银行", ("yin", "xing")))

    def test_a_character_collects_every_reading_it_has(self):
        lex = build(("行", "hang"), ("行", "xing"), ("银", "yin"))
        self.assertEqual(lex.readings_of("行"), ("hang", "xing"))
        self.assertEqual(lex.readings_of("银"), ("yin",))
        self.assertEqual(lex.readings_of("瓴"), ())

    def test_multi_character_entries_do_not_contribute_readings(self):
        # 银行's reading belongs to the phrase, not to 银 on its own.
        lex = build(("银行", "yin hang"))
        self.assertEqual(lex.readings_of("银"), ())

    def test_entries_whose_reading_count_misaligns_are_counted_not_dropped(self):
        lex = build(("你好", "ni hao hao"), ("好", "hao"))
        self.assertEqual(lex.misaligned, 1)
        self.assertEqual(len(lex), 1)


class AnalyzeTest(unittest.TestCase):
    def test_single_characters_alone_already_reach_gold(self):
        lex = build(*CHARS)
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex)
        self.assertTrue(got.reachable)
        self.assertEqual(got.min_words, 4)
        self.assertEqual(got.singles, 4)

    def test_a_known_phrase_collapses_the_path(self):
        lex = build(*CHARS, ("高屋建瓴", "gao wu jian ling"))
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex)
        self.assertEqual(got.min_words, 1)
        self.assertEqual(got.singles, 0)

    def test_ties_are_broken_toward_fewer_single_characters(self):
        # 建瓴 and 瓴 both let a 3-word path exist; the one using the phrase
        # spends one fewer single character and must be the one reported.
        lex = build(*CHARS, ("高屋", "gao wu"), ("建瓴", "jian ling"))
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex)
        self.assertEqual(got.min_words, 2)
        self.assertEqual(got.singles, 0)

    def test_a_phrase_with_the_wrong_reading_does_not_span(self):
        # The lexicon has 银行/yin hang; the user's reading is yin xing, so the
        # phrase cannot be used and the path falls back to characters.
        lex = build(("银行", "yin hang"), ("银", "yin"), ("行", "xing"))
        got = lattice.analyze("银行", ["yin", "xing"], lex)
        self.assertTrue(got.reachable)
        self.assertEqual(got.min_words, 2)
        self.assertEqual(got.singles, 2)

    def test_a_character_the_lexicon_lacks_entirely_is_missing_char(self):
        lex = build(("高", "gao"), ("屋", "wu"), ("建", "jian"))
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex)
        self.assertFalse(got.reachable)
        self.assertIsNone(got.min_words)
        self.assertEqual(len(got.gaps), 1)
        gap = got.gaps[0]
        self.assertEqual((gap.index, gap.char, gap.cause), (3, "瓴", lattice.MISSING_CHAR))
        self.assertEqual(gap.have, ())

    def test_a_reading_the_lexicon_lacks_is_missing_reading_and_shows_what_it_has(self):
        lex = build(("银", "yin"), ("行", "hang"))
        got = lattice.analyze("银行", ["yin", "xing"], lex)
        self.assertFalse(got.reachable)
        gap = got.gaps[0]
        self.assertEqual((gap.char, gap.want, gap.cause), ("行", "xing", lattice.MISSING_READING))
        # The readings it DOES have are what tells a 多音字 pypinyin mis-read
        # apart from a real lexicon gap.
        self.assertEqual(gap.have, ("hang",))

    def test_every_unspellable_character_is_reported_not_just_the_first(self):
        lex = build(("高", "gao"))
        got = lattice.analyze("高屋建", ["gao", "wu", "jian"], lex)
        self.assertEqual([g.char for g in got.gaps], ["屋", "建"])

    def test_a_phrase_cannot_rescue_a_character_that_has_no_entry(self):
        # 屋建 spans positions 1-3, but nothing spells 瓴, so gold stays
        # unreachable -- and the gap is reported against 瓴 only.
        lex = build(("高", "gao"), ("屋建", "wu jian"))
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex)
        self.assertFalse(got.reachable)
        self.assertEqual([g.char for g in got.gaps], ["屋", "建", "瓴"])

    def test_a_syllable_count_that_disagrees_with_the_text_is_its_own_cause(self):
        # What a non-Han character in the unit looks like: speller.syllables
        # drops it, so the counts stop matching. It must not be charged to the
        # lexicon.
        lex = build(*CHARS)
        got = lattice.analyze("高屋A建", ["gao", "wu", "jian"], lex)
        self.assertFalse(got.reachable)
        self.assertEqual(got.gaps[0].cause, lattice.NO_SYLLABLES)

    def test_entries_longer_than_max_word_are_not_considered(self):
        lex = build(*CHARS, ("高屋建瓴", "gao wu jian ling"))
        got = lattice.analyze("高屋建瓴", ["gao", "wu", "jian", "ling"], lex, max_word=3)
        self.assertEqual(got.min_words, 4)

    def test_empty_text_is_reachable_by_the_empty_path(self):
        got = lattice.analyze("", [], build(*CHARS))
        self.assertTrue(got.reachable)
        self.assertEqual((got.min_words, got.singles), (0, 0))


def ranked(*entries: tuple[str, str, float]) -> lattice.Lexicon:
    """(text, "space separated readings", log-weight) triples."""
    lex = lattice.Lexicon(by_reading=True)
    for text, reading, weight in entries:
        lex.add(text, reading.split(), weight)
    return lex


class KBestTest(unittest.TestCase):
    def test_orders_homophones_by_weight(self):
        lex = ranked(("市", "shi", -2.0), ("是", "shi", -1.0), ("事", "shi", -3.0))
        got = lattice.kbest(["shi"], lex, k=3)
        self.assertEqual([text for text, _ in got], ["是", "市", "事"])

    def test_a_phrase_beats_two_characters_of_equal_total_weight(self):
        # One entry pays WORD_PENALTY once, two pay it twice. This is the
        # whole reason the lexicon's own ordering prefers longer words, so a
        # k-best that lost it would not be the baseline it claims to be.
        lex = ranked(
            ("北京", "bei jing", -6.0),
            ("北", "bei", -3.0),
            ("京", "jing", -3.0),
        )
        got = lattice.kbest(["bei", "jing"], lex, k=5)
        self.assertEqual(got[0][0], "北京")

    def test_two_segmentations_of_one_text_are_a_single_candidate(self):
        # 北京 spelled as a phrase and as two characters is the same answer to
        # a user and to a scorer; keeping both would spend the beam twice on
        # it. The better-scoring segmentation is the one that survives.
        lex = ranked(
            ("北京", "bei jing", -6.0),
            ("北", "bei", -3.0),
            ("京", "jing", -3.0),
            ("背", "bei", -3.5),
        )
        got = lattice.kbest(["bei", "jing"], lex, k=10)
        texts = [text for text, _ in got]
        self.assertEqual(len(texts), len(set(texts)))
        self.assertEqual(texts[0], "北京")
        # and it kept the phrase's score, not the two-character one
        self.assertAlmostEqual(got[0][1], -6.0 + lattice.WORD_PENALTY)

    def test_returns_at_most_k(self):
        lex = ranked(*[(c, "shi", -float(i)) for i, c in enumerate("是市事式试")])
        self.assertEqual(len(lattice.kbest(["shi"], lex, k=3)), 3)

    def test_a_syllable_no_entry_reads_yields_nothing(self):
        lex = ranked(("是", "shi", -1.0))
        self.assertEqual(lattice.kbest(["shi", "zzz"], lex, k=5), [])

    def test_scores_are_sums_of_weights_and_one_penalty_per_entry(self):
        lex = ranked(("北", "bei", -3.0), ("京", "jing", -4.0))
        (text, score), = lattice.kbest(["bei", "jing"], lex, k=5)
        self.assertEqual(text, "北京")
        self.assertAlmostEqual(score, -3.0 - 4.0 + 2 * lattice.WORD_PENALTY)

    def test_entries_longer_than_max_word_are_not_used(self):
        lex = ranked(
            ("北京", "bei jing", -1.0),
            ("北", "bei", -3.0),
            ("京", "jing", -3.0),
        )
        got = lattice.kbest(["bei", "jing"], lex, k=5, max_word=1)
        self.assertEqual([text for text, _ in got], ["北京"])
        # reached as two entries, so it paid the penalty twice
        self.assertAlmostEqual(got[0][1], -6.0 + 2 * lattice.WORD_PENALTY)

    def test_words_at_requires_the_by_reading_index(self):
        # Building it means a pypinyin call for every entry of a reading-less
        # dictionary, so it is opt-in; asking without it must fail loudly
        # rather than silently return no candidates.
        with self.assertRaises(RuntimeError):
            build(("是", "shi")).words_at(("shi",))

    def test_the_same_word_from_two_tables_keeps_the_larger_weight(self):
        lex = ranked(("是", "shi", -5.0), ("是", "shi", -1.0), ("是", "shi", -3.0))
        self.assertEqual(lex.words_at(("shi",)), {"是": -1.0})


class ImportTablesTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)
        self.addCleanup(self.tmp.cleanup)

    def write(self, text: str) -> Path:
        path = self.dir / "private.dict.yaml"
        path.write_text(text, encoding="utf-8")
        return path

    def test_reads_the_declared_tables_in_order(self):
        got = lattice.import_tables(
            self.write(
                "---\nname: private\nimport_tables:\n"
                "  - cn_dicts/8105\n  - cn_dicts/base\n...\n"
            )
        )
        self.assertEqual(
            [self.dir / "cn_dicts/8105.dict.yaml", self.dir / "cn_dicts/base.dict.yaml"], got
        )

    def test_trailing_comments_are_not_part_of_the_name(self):
        got = lattice.import_tables(
            self.write("---\nimport_tables:\n  - cn_dicts/8105     # 字表\n...\n")
        )
        self.assertEqual([self.dir / "cn_dicts/8105.dict.yaml"], got)

    def test_commented_out_imports_are_skipped(self):
        # This is how the real file records a table the user chose NOT to
        # load (cn_dicts/41448). Reading it would measure a lexicon nobody
        # types with.
        got = lattice.import_tables(
            self.write(
                "---\nimport_tables:\n  - cn_dicts/8105\n"
                "  # - cn_dicts/41448\n  - cn_dicts/base\n...\n"
            )
        )
        self.assertEqual(
            [self.dir / "cn_dicts/8105.dict.yaml", self.dir / "cn_dicts/base.dict.yaml"], got
        )

    def test_blank_lines_inside_the_block_do_not_end_it(self):
        got = lattice.import_tables(
            self.write("---\nimport_tables:\n  - a\n\n  - b\n...\n")
        )
        self.assertEqual([self.dir / "a.dict.yaml", self.dir / "b.dict.yaml"], got)

    def test_a_following_key_ends_the_block(self):
        got = lattice.import_tables(
            self.write("---\nimport_tables:\n  - a\nvocabulary: essay\n...\n")
        )
        self.assertEqual([self.dir / "a.dict.yaml"], got)

    def test_table_name_strips_the_whole_dict_yaml_suffix(self):
        # Path.stem gives "tencent.dict", which matches no table name and
        # silently skips whatever the caller meant to select.
        self.assertEqual(lattice.table_name(Path("cn_dicts/tencent.dict.yaml")), "tencent")
        self.assertEqual(lattice.table_name(Path("private/custom.dict.yaml")), "custom")

    def test_a_dictionary_declaring_no_imports_is_an_error(self):
        # Returning [] would build an empty lexicon and report 0% coverage as
        # though it were a measurement.
        with self.assertRaises(ValueError):
            lattice.import_tables(self.write("---\nname: private\n...\n"))


class SubstringsTest(unittest.TestCase):
    def test_every_substring_up_to_the_cap(self):
        self.assertEqual(
            sorted(set(lattice.substrings("abc", max_word=2))),
            ["a", "ab", "b", "bc", "c"],
        )

    def test_the_whole_text_is_included_when_it_fits(self):
        self.assertIn("abc", set(lattice.substrings("abc", max_word=3)))

    def test_empty_text_yields_nothing(self):
        self.assertEqual(list(lattice.substrings("")), [])


if __name__ == "__main__":
    unittest.main()
