"""The rule chain that decides what stays in the personal lexicon.

Every case in RegressionCases is a mistake an earlier draft of this chain made.
The chain is ordered and first-match-wins, so a case is really an assertion
about which rule wins, not just about the outcome.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot.clean import (Lexicon, Thresholds, classify, has_fragment_shape)
from rime_copilot.dictfile import Entry


def lexicon(known=None, chart=None, cuts=None, names=()):
    """A Lexicon with no jieba behind it.

    `cuts` maps a word to its segmentation; anything absent is treated as one
    piece, which is what jieba's HMM does for an out-of-vocabulary word.
    """
    return Lexicon(known=known or {},
                   chart=chart,
                   segment=lambda w: (cuts or {}).get(w, [w]),
                   tags=lambda w: ["nr"] if w in names else ["n"])


def entry(word, weight, pinyin="x"):
    return Entry(word, pinyin, weight)


class FragmentShape(unittest.TestCase):
    def test_leading_function_word(self):
        self.assertTrue(has_fragment_shape("的问题"))

    def test_trailing_function_word(self):
        self.assertTrue(has_fragment_shape("编译的"))

    def test_leading_pronoun(self):
        self.assertTrue(has_fragment_shape("这个我"))

    def test_plain_word(self):
        self.assertFalse(has_fragment_shape("落盘"))


class RuleChain(unittest.TestCase):
    def test_r0_character_outside_the_chart(self):
        lex = lexicon(chart=set("可以"))
        verdict = classify(entry("尅可以", 6), lex)
        self.assertEqual("drop", verdict.action)
        self.assertEqual("R0", verdict.rule)

    def test_r1_known_word_is_kept(self):
        lex = lexicon(known={"上线": 4210}, chart=set("上线"))
        verdict = classify(entry("上线", 2877), lex)
        self.assertEqual(("keep", "R1"), (verdict.action, verdict.rule))

    def test_r1_known_word_below_low_threshold_is_a_pointless_pin(self):
        lex = lexicon(known={"一份": 900}, chart=set("一份"))
        verdict = classify(entry("一份", 2), lex)
        self.assertEqual(("drop", "R1"), (verdict.action, verdict.rule))

    def test_r9_high_weight_overrules_fragment_shape(self):
        # 好了 is a real word whose shape is word+function. No dictionary in the
        # tree can say so, so the user's own 1691 commits have to.
        lex = lexicon(chart=set("好了"))
        verdict = classify(entry("好了", 1691), lex)
        self.assertEqual(("review", "R9-fragment"), (verdict.action, verdict.rule))
        self.assertEqual("drop", verdict.suggest)

    def test_r9_high_weight_without_fragment_shape_defaults_to_keep(self):
        lex = lexicon(chart=set("识六"))
        verdict = classify(entry("识六", 475), lex)
        self.assertEqual(("review", "R9"), (verdict.action, verdict.rule))
        self.assertEqual("keep", verdict.suggest)

    def test_r2_fragment_below_the_escape_hatch(self):
        lex = lexicon(chart=set("的问题"))
        verdict = classify(entry("的问题", 45), lex)
        self.assertEqual(("drop", "R2"), (verdict.action, verdict.rule))

    def test_r3_name(self):
        lex = lexicon(chart=set("子通"), names={"子通"})
        verdict = classify(entry("子通", 24), lex)
        self.assertEqual(("review", "R3-name"), (verdict.action, verdict.rule))
        self.assertEqual("keep", verdict.suggest)

    def test_r3_out_of_vocabulary_word(self):
        lex = lexicon(chart=set("落盘"))
        verdict = classify(entry("落盘", 24), lex)
        self.assertEqual(("review", "R3-oov"), (verdict.action, verdict.rule))

    def test_r3_out_of_vocabulary_below_low_threshold(self):
        lex = lexicon(chart=set("一鸣"))
        verdict = classify(entry("一鸣", 2), lex)
        self.assertEqual(("drop", "R3-oov"), (verdict.action, verdict.rule))

    def test_r4_compound_term_survives_on_weight(self):
        lex = lexicon(chart=set("自动驾驶"), cuts={"自动驾驶": ["自动", "驾驶"]})
        verdict = classify(entry("自动驾驶", 60), lex)
        self.assertEqual(("review", "R4"), (verdict.action, verdict.rule))

    def test_r4_phrase_with_a_single_character_piece(self):
        lex = lexicon(chart=set("台机器"), cuts={"台机器": ["台", "机器"]})
        verdict = classify(entry("台机器", 86), lex)
        self.assertEqual(("drop", "R4"), (verdict.action, verdict.rule))

    def test_r4_low_weight_compound(self):
        lex = lexicon(chart=set("上线窗口"), cuts={"上线窗口": ["上线", "窗口"]})
        verdict = classify(entry("上线窗口", 4), lex)
        self.assertEqual(("drop", "R4"), (verdict.action, verdict.rule))


class RegressionCases(unittest.TestCase):
    """Words two earlier drafts of this chain deleted, and why they must not be."""

    def test_high_frequency_colloquial_words_survive(self):
        lex = lexicon(chart=set("好了稍等是的要不"))
        for word, weight in (("好了", 1691), ("稍等", 793), ("是的", 660), ("要不", 566)):
            with self.subTest(word=word):
                self.assertNotEqual("drop", classify(entry(word, weight), lex).action)

    def test_shixxx_reaches_review_even_though_the_name_tagger_misses_it(self):
        # jieba tags 识六 as 识/六, so R3-name never fires. R9 is what covers it.
        lex = lexicon(chart=set("识六"), cuts={"识六": ["识", "六"]})
        self.assertEqual("R9", classify(entry("识六", 475), lex).rule)

    def test_fragments_still_go(self):
        lex = lexicon(chart=set("的问题上编译机器这个我台"),
                      cuts={"台机器": ["台", "机器"]})
        for word in ("的问题", "上的", "编译的", "这个我"):
            with self.subTest(word=word):
                self.assertEqual("drop", classify(entry(word, 45), lex).action)


class ReadChart(unittest.TestCase):
    def test_takes_single_characters_only(self):
        import tempfile
        from rime_copilot.clean import read_chart
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "8105.dict.yaml"
            path.write_text("---\nname: chart\n...\n\n个\tge\t14789406\n"
                            "改\tgai\t495567\n上线\tshang xian\t10\n",
                            encoding="utf-8")
            self.assertEqual({"个", "改"}, read_chart(path))


class LoadLexicon(unittest.TestCase):
    def test_reports_a_missing_jieba_usefully(self):
        import builtins
        from rime_copilot import clean
        real_import = builtins.__import__

        def deny(name, *args, **kwargs):
            if name.startswith("jieba"):
                raise ImportError("no jieba")
            return real_import(name, *args, **kwargs)

        builtins.__import__ = deny
        try:
            with self.assertRaises(ImportError) as caught:
                clean.load_lexicon()
        finally:
            builtins.__import__ = real_import
        message = str(caught.exception)
        self.assertIn("jieba", message)
        self.assertIn("interpreter", message)


class Partitioning(unittest.TestCase):
    def build(self):
        from rime_copilot.clean import partition
        lex = lexicon(known={"上线": 4210},
                      chart=set("上线的问题识六子通台机器一份"),
                      cuts={"台机器": ["台", "机器"]},
                      names={"子通"})
        entries = [entry("上线", 2877), entry("一份", 2), entry("的问题", 45),
                   entry("识六", 475), entry("子通", 24), entry("台机器", 86)]
        return partition(entries, lex)

    def test_three_way_split(self):
        part = self.build()
        self.assertEqual(["上线"], [e.word for e in part.keep])
        self.assertEqual({"识六", "子通"}, {e.word for e, _ in part.review})
        self.assertEqual({"一份", "的问题", "台机器"}, {e.word for e, _ in part.drop})

    def test_every_entry_lands_somewhere_exactly_once(self):
        part = self.build()
        total = len(part.keep) + len(part.review) + len(part.drop)
        self.assertEqual(6, total)

    def test_idempotent_on_an_already_cleaned_lexicon(self):
        from rime_copilot.clean import partition
        part = self.build()
        survivors = part.keep + [e for e, _ in part.review]
        lex = lexicon(known={"上线": 4210},
                      chart=set("上线识六子通"), names={"子通"})
        again = partition(survivors, lex)
        self.assertEqual({e.word for e in survivors},
                         {e.word for e in again.keep} | {e.word for e, _ in again.review})


class ReviewFile(unittest.TestCase):
    def render(self):
        from rime_copilot.clean import partition, render_review
        lex = lexicon(chart=set("好了识六子通落盘"), names={"子通"})
        entries = [entry("好了", 1691), entry("识六", 475),
                   entry("子通", 24), entry("落盘", 24)]
        return render_review(partition(entries, lex))

    def test_groups_appear_in_a_fixed_order(self):
        text = self.render()
        positions = [text.index(f"# {rule}") for rule in ("R9", "R3-name", "R3-oov")]
        self.assertEqual(sorted(positions), positions)

    def test_fragment_rows_are_pre_marked_drop_and_sink(self):
        text = self.render()
        rows = [line for line in text.splitlines()
                if line and not line.startswith("#")]
        haole = next(r for r in rows if "好了" in r)
        shiliu = next(r for r in rows if "识六" in r)
        self.assertTrue(haole.startswith("drop"))
        self.assertTrue(shiliu.startswith("keep"))
        self.assertGreater(rows.index(haole), rows.index(shiliu))

    def test_row_is_tab_separated_action_weight_word_rule_reason(self):
        text = self.render()
        row = next(line for line in text.splitlines() if "\t落盘\t" in line)
        action, weight, word, rule, reason = row.split("\t")
        self.assertEqual(("keep", "24", "落盘", "R3-oov"), (action, weight, word, rule))
        self.assertTrue(reason)


class ParseReview(unittest.TestCase):
    def test_reads_action_and_word_ignoring_comments_and_blanks(self):
        from rime_copilot.clean import parse_review
        text = ("# a comment\n"
                "\n"
                "keep\t475\t识六\tR9\tsomething\n"
                "drop\t1691\t好了\tR9-fragment\tsomething else\n")
        self.assertEqual({"识六": "keep", "好了": "drop"}, parse_review(text))

    def test_rejects_an_unknown_action(self):
        from rime_copilot.clean import parse_review
        with self.assertRaises(ValueError) as caught:
            parse_review("maybe\t1\t词\tR9\twhy\n")
        self.assertIn("maybe", str(caught.exception))

    def test_rejects_a_short_row(self):
        from rime_copilot.clean import parse_review
        with self.assertRaises(ValueError):
            parse_review("keep\t475\n")

    def test_rejects_a_word_decided_two_different_ways(self):
        from rime_copilot.clean import parse_review
        text = "keep\t475\t识六\tR9\tsomething\ndrop\t475\t识六\tR9\tsomething\n"
        with self.assertRaises(ValueError) as caught:
            parse_review(text)
        self.assertIn("识六", str(caught.exception))

    def test_a_word_repeated_with_the_same_action_is_not_a_conflict(self):
        from rime_copilot.clean import parse_review
        text = "keep\t475\t识六\tR9\tsomething\nkeep\t475\t识六\tR9\tsomething\n"
        self.assertEqual({"识六": "keep"}, parse_review(text))


class ApplyReview(unittest.TestCase):
    def build(self):
        from rime_copilot.clean import Partition, Verdict
        keep_suggested = Verdict("review", "R9", "why", "keep")
        drop_suggested = Verdict("review", "R9-fragment", "why", "drop")
        return Partition(keep=[entry("上线", 2877)],
                         review=[(entry("识六", 475), keep_suggested),
                                 (entry("好了", 1691), drop_suggested)],
                         drop=[(entry("的问题", 45), keep_suggested)])

    def test_keeps_the_kept_and_the_reviewer_approved(self):
        from rime_copilot.clean import apply_review
        result = apply_review(self.build(), {"识六": "keep", "好了": "drop"})
        self.assertEqual(["上线", "识六"], sorted(e.word for e in result))

    def test_a_review_row_with_no_decision_falls_back_to_its_own_suggestion(self):
        from rime_copilot.clean import apply_review
        result = apply_review(self.build(), {})
        # 识六 defaults to keep and survives; 好了 defaults to drop and does not —
        # so this fails against a bug that hardcodes one action for every row.
        self.assertEqual({"上线", "识六"}, {e.word for e in result})

    def test_sorted_by_first_character_then_descending_weight(self):
        from rime_copilot.clean import Partition, Verdict, apply_review
        part = Partition(keep=[entry("一次", 5), entry("一起", 589)], review=[], drop=[])
        result = apply_review(part, {})
        self.assertEqual(["一起", "一次"], [e.word for e in result])


if __name__ == "__main__":
    unittest.main()
