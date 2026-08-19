import sys
import tempfile
from collections import Counter
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_train import build, charset, dedup, isolation, ngram, normalize


class Normalize(unittest.TestCase):
    def test_full_width_latin_and_digits_fold_to_ascii(self):
        # The user's own keyboard produces ASCII; leaving both forms in the
        # corpus splits the counts for what is the same text.
        self.assertEqual(normalize.normalize("ＡＢ１２"), "AB12")

    def test_control_characters_are_dropped_and_whitespace_collapsed(self):
        self.assertEqual(normalize.normalize("  你好\x07  世界 "), "你好 世界")

    def test_newlines_survive_normalization(self):
        self.assertIn("\n", normalize.normalize("你好\n世界").replace(" ", "\n"))

    def test_han_runs_split_on_every_non_han_boundary(self):
        # Not only punctuation: any non-Han character ends a Han collocation,
        # because the lookup key is a run of Han characters.
        self.assertEqual(
            normalize.han_sentences("看下当前的项目. I fixed it 然后重测吧！好的"),
            ["看下当前的项目", "然后重测吧", "好的"],
        )

    def test_text_with_no_han_yields_nothing(self):
        self.assertEqual(normalize.han_sentences("git commit -m 'x'"), [])

    def test_word_separating_spaces_between_han_are_removed(self):
        """LCCC ships pre-tokenized. Left alone, han_sentences would shatter
        every utterance into one-word fragments and no 3-or-4 character
        collocation would survive a word boundary -- a corpus that builds
        cleanly, teaches the grammar nothing, and makes the n-gram experiment
        read as "domain does not matter"."""
        self.assertEqual(
            normalize.join_han_tokens("你 去 那儿 竟然 不喊 我"), "你去那儿竟然不喊我"
        )

    def test_spaces_around_latin_are_kept(self):
        # Only spaces with Han on BOTH sides are word separators.
        self.assertEqual(
            normalize.join_han_tokens("竟然 SEED 早上"), "竟然 SEED 早上"
        )

    def test_joining_then_splitting_recovers_whole_utterances(self):
        joined = normalize.join_han_tokens("领个 搓衣板 去 吧")
        self.assertEqual(normalize.han_sentences(joined), ["领个搓衣板去吧"])


class Extract(unittest.TestCase):
    """Line shapes verified against the real files, not their dataset cards --
    the card said LCCC lines were objects with a `dialog` key; they are bare
    JSON arrays."""

    def setUp(self):
        from rime_train.sources import SOURCES
        self.lccc = SOURCES["lccc-base"]
        self.skypile = SOURCES["skypile-0"]

    def test_a_bare_array_line_is_the_value_itself(self):
        self.assertEqual(
            build.texts_from_line('["你好", "再见"]', self.lccc), ["你好", "再见"]
        )

    def test_a_field_bearing_line_is_read_by_field(self):
        self.assertEqual(
            build.texts_from_line('{"text": "你好"}', self.skypile), ["你好"]
        )

    def test_a_missing_field_yields_nothing(self):
        self.assertEqual(build.texts_from_line('{"other": 1}', self.skypile), [])

    def test_an_unparseable_line_is_skipped_not_raised(self):
        # Multi-gigabyte web dumps contain broken lines; refusing the whole
        # corpus for one of them trades a complete run for nothing.
        self.assertEqual(build.texts_from_line("not json", self.lccc), [])

    def test_an_array_line_read_by_field_yields_nothing(self):
        self.assertEqual(build.texts_from_line('["你好"]', self.skypile), [])


class Charset(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        path = Path(self.tmp.name) / "chars.dict.yaml"
        path.write_text(
            "---\nname: t\n...\n继\tji\t1\n续\txu\t1\n修\txiu\t1\n吧\tba\t1\n"
            "继续\tji xu\t1\n",
            encoding="utf-8",
        )
        self.charset = charset.load_charset(path)

    def test_only_single_characters_enter_the_set(self):
        self.assertEqual(self.charset, frozenset("继续修吧"))

    def test_a_sentence_of_known_characters_is_typeable(self):
        self.assertTrue(charset.is_typeable("继续修吧", self.charset))

    def test_one_unknown_character_disqualifies_the_sentence(self):
        # All-or-nothing rather than a ratio: a collocation containing a
        # character this schema cannot type is unreachable, not merely rare.
        self.assertFalse(charset.is_typeable("继续修理", self.charset))

    def test_non_han_characters_are_not_checked(self):
        # Latin cannot appear in a collocation key at all, so it neither
        # qualifies nor disqualifies a sentence.
        self.assertTrue(charset.is_typeable("继续 v2 修吧", self.charset))


class Ngram(unittest.TestCase):
    def test_counts_every_window_of_each_length(self):
        counts = ngram.count(["继续修吧"])
        self.assertEqual(counts["继续修"], 1)
        self.assertEqual(counts["续修吧"], 1)
        self.assertEqual(counts["继续修吧"], 1)
        # 2 characters is below octagram's collocation_min_length
        self.assertEqual(counts["继续"], 0)

    def test_windows_never_cross_a_sentence_boundary(self):
        # The whole reason splitting is load-bearing: 吧来进 spans two
        # sentences and is a collocation nobody ever types.
        counts = ngram.count(["继续修吧", "来进行吧"])
        self.assertEqual(counts["吧来进"], 0)

    def test_a_sentence_shorter_than_the_window_contributes_nothing(self):
        self.assertEqual(ngram.count(["好的"]), {})

    def test_emit_drops_singletons_by_default(self):
        counts = ngram.count(["继续修吧", "继续修吧", "来进行吧"])
        emitted = list(ngram.emit(counts))
        self.assertIn("继续修 2", emitted)
        self.assertFalse([line for line in emitted if line.endswith(" 1")])

    def test_emit_produces_the_two_column_shape_build_grammar_reads(self):
        # build_grammar does `cin >> key >> value`, so a key with whitespace
        # in it would silently shift every following field.
        for line in ngram.emit(ngram.count(["继续修吧", "继续修吧"])):
            key, value = line.split(" ")
            self.assertNotIn(" ", key)
            self.assertGreaterEqual(int(value), 1)


class Conditional(unittest.TestCase):
    """Raw counts are what octagram's arithmetic treats as the score --
    `log(count) + collocation_penalty`, monotone in frequency with no
    conditioning -- so a social-media corpus hands 哈哈哈 a large bonus
    regardless of context. Measured: raw counts 65.2%, conditional 68.5%,
    on the same corpus and the same 3287 runs."""

    def counts(self):
        # min_length one shorter than octagram looks up, so every key's
        # prefix is present to divide by.
        return ngram.count(["继续修吧", "继续修吧", "继续修理"], min_length=2)

    def test_value_is_the_conditional_probability_scaled(self):
        emitted = dict(
            line.split(" ") for line in ngram.emit_conditional(self.counts(), min_count=1)
        )
        # 继续修 occurs 3 times, 继续修吧 2 of them
        self.assertEqual(int(emitted["继续修吧"]), int(2 / 3 * ngram.PROB_SCALE))

    def test_the_scale_is_what_keeps_a_probability_above_the_format_clamp(self):
        # build_grammar stores max(0, int(log(value) * 10000)), so an unscaled
        # probability -- always below 1, always a negative log -- would clamp
        # to 0 and rank nothing.
        import math
        self.assertGreater(math.log(0.001 * ngram.PROB_SCALE), 0)

    def test_only_lengths_octagram_looks_up_are_emitted(self):
        keys = [line.split(" ")[0] for line in
                ngram.emit_conditional(self.counts(), min_count=1)]
        self.assertTrue(all(3 <= len(k) <= 4 for k in keys), keys)

    def test_a_key_whose_prefix_is_absent_is_skipped(self):
        # Cannot divide by a count that was never taken; emitting it with a
        # made-up denominator would be worse than omitting it.
        counts = Counter({"继续修": 5})
        self.assertEqual(list(ngram.emit_conditional(counts, min_count=1)), [])

    def test_a_key_and_its_prefix_always_share_a_shard(self):
        # What makes conditional emission possible under sharding at all:
        # sharding is on the first character, and key[:-1] keeps it.
        for key in ("继续修吧", "看下当前"):
            self.assertEqual(ord(key[0]) % 8, ord(key[:-1][0]) % 8)


class Dedup(unittest.TestCase):
    def test_keeps_first_occurrence_and_drops_repeats(self):
        self.assertEqual(list(dedup.unique(["a", "b", "a", "c", "b"])), ["a", "b", "c"])

    def test_distinct_lines_all_survive(self):
        self.assertEqual(list(dedup.unique(["a", "b", "c"])), ["a", "b", "c"])


class Isolation(unittest.TestCase):
    def test_a_short_sentence_produces_no_fingerprint(self):
        """The difference between a check and a noise generator. Fingerprinting
        short sentences whole reported 861 overlaps against 17.5M LCCC
        sentences, every one of them 嗯 / 好的 / 是啊 / 哈哈 -- not leakage,
        just Chinese."""
        self.assertEqual(isolation.fingerprints(["继续修吧"]), set())

    def test_a_long_sentence_is_fingerprinted_by_windows(self):
        marks = isolation.fingerprints(["一二三四五六七八九十十一"], span=10)
        self.assertEqual(marks, {"一二三四五六七八九十", "二三四五六七八九十十", "三四五六七八九十十一"})

    def test_an_exact_repeat_of_a_long_eval_sentence_is_reported(self):
        long_sentence = "看下当前的项目然后重新测一遍"
        marks = isolation.fingerprints([long_sentence])
        self.assertEqual(
            isolation.overlaps([long_sentence, "别的话"], marks), [long_sentence]
        )

    def test_a_common_short_utterance_is_never_an_overlap(self):
        """The false positive that made the first version useless: every chat
        corpus contains 好的, and sharing it with the eval set is not
        evidence of anything."""
        marks = isolation.fingerprints(["好的", "嗯", "哈哈"])
        self.assertEqual(isolation.overlaps(["好的", "嗯", "哈哈"], marks), [])

    def test_a_long_shared_span_is_reported_even_inside_a_longer_sentence(self):
        marks = isolation.fingerprints(["看下当前的项目然后重测"], span=10)
        hits = isolation.overlaps(["前面加一段看下当前的项目然后重测后面再加一段"], marks, span=10)
        self.assertEqual(len(hits), 1)

    def test_a_merely_common_short_phrase_is_not_an_overlap(self):
        # A shared 3-character collocation is not evidence of anything;
        # flagging it would make the check useless by crying wolf.
        marks = isolation.fingerprints(["看下当前的项目然后重测"], span=10)
        self.assertEqual(isolation.overlaps(["好的我看下"], marks, span=10), [])


if __name__ == "__main__":
    unittest.main()


class VocabTest(unittest.TestCase):
    """Encoding must match what llama.cpp's SPM tokenizer does with the same
    token list, or every score measured offline describes a different
    segmentation from the one inference uses. Verified end to end by
    export.py's --check: torch -27.9069, llama.cpp -27.9071."""

    def setUp(self):
        from rime_train import vocab as _vocab
        self.mod = _vocab
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        path = Path(self.tmp.name) / "c.dict.yaml"
        path.write_text("---\nname: t\n...\n继\tji\t1\n续\txu\t1\n修\txiu\t1\n吧\tba\t1\n",
                        encoding="utf-8")
        self.pieces = self.mod.build(path)
        self.v = self.mod.Vocab(self.pieces)

    def test_specials_then_bytes_then_characters(self):
        self.assertEqual(self.pieces[:3], self.mod.SPECIALS)
        self.assertEqual(self.pieces[3], "<0x00>")
        self.assertEqual(self.pieces[258], "<0xFF>")
        self.assertEqual(self.pieces[259], "继")

    def test_a_known_character_is_one_token(self):
        self.assertEqual(len(self.v.encode("继续修吧")), 4)

    def test_an_unknown_character_falls_back_to_its_utf8_bytes(self):
        # 8k characters is not the world; byte fallback is what a
        # character-level vocabulary needs to cover the rest.
        ids = self.v.encode("✓")
        self.assertEqual(len(ids), 3)
        self.assertTrue(all(self.v.byte_base <= i < self.v.byte_base + 256 for i in ids))

    def test_round_trips_through_both_paths(self):
        text = "继续修吧 ok ✓"
        self.assertEqual(self.v.decode(self.v.encode(text)), text)

    def test_bos_is_prepended_only_when_asked(self):
        self.assertEqual(self.v.encode("继", bos=True)[0], self.mod.BOS_ID)
        self.assertNotEqual(self.v.encode("继")[0], self.mod.BOS_ID)

    def test_the_vocabulary_fits_uint16(self):
        # train.py stores token ids as uint16 to halve what each step reads.
        self.assertLess(len(self.v), 65536)

    def test_ascii_is_present_for_the_latin_half_of_the_corpus(self):
        for ch in "abz09.,":
            self.assertIn(ch, self.v.ids)
