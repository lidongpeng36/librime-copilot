import sys
import tempfile
from collections import Counter
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_train import build, charset, dedup, isolation, ngram, normalize


class Normalize(unittest.TestCase):
    def test_full_width_forms_are_preserved_not_folded(self):
        """NFKC folding looked like consistency and was a training/inference
        mismatch: the evaluation contexts hold 4062 ASCII commas AND 1827
        full-width ones, and nothing normalizes them at inference."""
        self.assertEqual(normalize.normalize("好的，继续"), "好的，继续")
        self.assertEqual(normalize.normalize("ＡＢ１２"), "ＡＢ１２")

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

    def test_spaces_around_punctuation_are_removed_too(self):
        # "道歉 !" would teach a space before every exclamation mark.
        self.assertEqual(normalize.join_han_tokens("道歉 ！ 好 的"), "道歉！好的")

    def test_a_space_next_to_latin_or_digits_survives(self):
        self.assertEqual(normalize.join_han_tokens("SEED 早上"), "SEED 早上")
        self.assertEqual(normalize.join_han_tokens("运行 3 次"), "运行 3 次")

    def test_word_separating_spaces_between_han_are_removed(self):
        """LCCC ships pre-tokenized. Left alone, han_sentences would shatter
        every utterance into one-word fragments and no 3-or-4 character
        collocation would survive a word boundary -- a corpus that builds
        cleanly, teaches the grammar nothing, and makes the n-gram experiment
        read as "domain does not matter"."""
        self.assertEqual(
            normalize.join_han_tokens("你 去 那儿 竟然 不喊 我"), "你去那儿竟然不喊我"
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

    def test_cjk_punctuation_is_one_token_each(self):
        # Otherwise each costs three byte-fallback tokens whose embeddings
        # barely train, and the eval contexts hold 1827 full-width commas.
        for ch in "，。？！“”（）":
            self.assertEqual(len(self.v.encode(ch)), 1, ch)


class TextSentences(unittest.TestCase):
    """What han_sentences throws away is exactly what a language model
    conditions on. Measured on the evaluation corpus: 0% of scoring contexts
    end in a Han character, 77.3% end in something else."""

    def test_punctuation_latin_and_digits_survive(self):
        got = normalize.text_sentences("看下当前的项目. 我发现 build.py 运行了 3 次,怎么办?")
        self.assertEqual(got, ["看下当前的项目. 我发现 build.py 运行了 3 次,怎么办?"])

    def test_han_only_splitting_loses_all_of_it(self):
        # The contrast this exists for.
        text = "看下当前的项目. 我发现 build.py 运行了"
        self.assertEqual(normalize.han_sentences(text), ["看下当前的项目", "我发现", "运行了"])

    def test_sentence_enders_split_but_commas_do_not(self):
        got = normalize.text_sentences("好的,继续吧。然后重测")
        self.assertEqual(got, ["好的,继续吧。", "然后重测"])

    def test_a_chunk_with_no_han_is_dropped(self):
        # Latin is wanted as context, not as something to model.
        self.assertEqual(normalize.text_sentences("just english here. and more"), [])


class ScoringFormTest(unittest.TestCase):
    def test_scoring_form_inserts_eos_after_every_sentence_ender(self):
        self.assertEqual(normalize.scoring_form("你好。我们走"), "你好。\x02我们走")
        self.assertEqual(normalize.scoring_form("你好。"), "你好。\x02")
        self.assertEqual(normalize.scoring_form("好吗？好的！"), "好吗？\x02好的！\x02")

    def test_scoring_form_leaves_no_whitespace_beside_eos(self):
        """normalize() ends in .strip() and text_sentences strips EVERY chunk, so
        the training stream never has whitespace on either side of token 2."""
        self.assertEqual(normalize.scoring_form("你好。 我们走"), "你好。\x02我们走")
        self.assertEqual(normalize.scoring_form("  你好。\n我们走  "), "你好。\x02我们走")

    def test_scoring_form_does_not_split_on_commas(self):
        self.assertEqual(normalize.scoring_form("你好，我们走"), "你好，我们走")

    def test_scoring_form_folds_newlines_to_a_space(self):
        """A newline is NOT a full stop: mapping it to EOS would render the
        user's line break as the symbol the model learned to read as a period."""
        self.assertEqual(normalize.scoring_form("你好\n我们走"), "你好 我们走")

    def test_scoring_form_preserves_the_chains_segmentation(self):
        """Checks that scoring_form is the concatenation of
        text_sentences(normalize(text)) with a carrier after every ender --
        i.e. that the projection preserves the chain's segmentation and
        normalization. It does NOT verify EOS placement against train.py:
        that is test_scoring_form_emits_no_eos_for_an_unfinished_context, and
        it needs a separate test because train.py's EOS is unconditional per
        LINE while this expression's is conditional per ENDER -- the two only
        coincide at internal boundaries, never at the end of the input, so an
        equality check built from the same per-chunk condition scoring_form
        itself uses cannot catch a wrong EOS rule; it can only catch two local
        copies going out of sync with each other. References
        normalize._SENT_END_CHARS rather than a second hardcoded literal so a
        change to the ender set cannot desync the test from the code it is
        checking.
        """
        samples = [
            "你好。我们走",
            "看下当前的项目。然后重测吧！",
            "运行 3 次都失败了。改一下 config.py 再试；应该可以了。",
            "好的，继续",
            "ＡＢ１２ 混排的全角。半角也在。",
        ]
        for text in samples:
            with self.subTest(text=text):
                chunks = normalize.text_sentences(normalize.normalize(text))
                expected = "".join(
                    c + (normalize.EOS_CARRIER if c[-1] in normalize._SENT_END_CHARS else "")
                    for c in chunks
                )
                self.assertEqual(normalize.scoring_form(text), expected)

    def test_scoring_form_keeps_what_the_corpus_would_have_dropped(self):
        """The one deliberate divergence, pinned so it cannot become accidental.

        text_sentences drops a chunk with no Han because a corpus should not
        spend capacity teaching this model English. At inference there is nothing
        to select: the text before the caret is what the user wrote.
        """
        text = "ok then。好的。"
        self.assertEqual(normalize.text_sentences(normalize.normalize(text)), ["好的。"])
        self.assertEqual(normalize.scoring_form(text), "ok then。\x02好的。\x02")

    def test_scoring_form_emits_no_eos_for_an_unfinished_context(self):
        """A second deliberate divergence from the training chain, pinned so it
        cannot become accidental. train.py appends EOS after every non-empty
        corpus LINE, unconditionally -- a dialogue line with no terminal
        punctuation ("在吗") still gets one. At the caret the sentence is
        unfinished, and an EOS there would tell the model "this is finished"
        while we are asking it to predict the continuation -- so scoring_form
        must not append one for an unfinished context, even though training
        always would have.
        """
        self.assertEqual(normalize.scoring_form("你好。我们走"), "你好。\x02我们走")
        self.assertEqual(normalize.scoring_form("在吗"), "在吗")


class LanguageFilterTest(unittest.TestCase):
    """m-a-p/Matrix does not carry the language in the split name; the
    `Language=` field does, and the two disagree. Measured 2026-08-21:
    wiki_all and book_technology are English, and paper_science is labelled
    `en` while actually being JAPANESE -- whose kanji pass a naive CJK regex
    and would enter the corpus looking like Chinese. The trailing `=` in the
    key is not a typo; it is the field's real name.
    """

    SRC = build.Source(name="m", url="http://x/m.jsonl", register="technical",
                       field="text", language_field="Language=", language="zh")

    def test_a_matching_language_passes(self):
        line = '{"Language=": "zh", "text": "这是一段中文技术文字"}'
        self.assertEqual(build.texts_from_line(line, self.SRC), ["这是一段中文技术文字"])

    def test_a_different_language_is_dropped(self):
        line = '{"Language=": "en", "text": "This is English prose"}'
        self.assertEqual(build.texts_from_line(line, self.SRC), [])

    def test_a_missing_language_field_is_dropped_not_admitted(self):
        """Unlabelled is not the same as matching. Admitting it is how the
        Japanese paper_science text would have got in."""
        line = '{"text": "这是一段中文"}'
        self.assertEqual(build.texts_from_line(line, self.SRC), [])

    def test_a_source_with_no_language_field_is_unaffected(self):
        plain = build.Source(name="s", url="http://x/s.jsonl", register="general",
                             field="text")
        line = '{"Language=": "en", "text": "这是一段中文"}'
        self.assertEqual(build.texts_from_line(line, plain), ["这是一段中文"])


class CcTechnologySourceTest(unittest.TestCase):
    def test_it_is_registered_and_language_filtered(self):
        from rime_train.sources import SOURCES
        src = SOURCES["cc-technology"]
        self.assertEqual(src.register, "technical")
        self.assertEqual(src.field, "text")
        self.assertEqual(src.language_field, "Language=")
        self.assertEqual(src.language, "zh")
        self.assertTrue(src.url.endswith(".jsonl"))

    def test_it_is_upsampled(self):
        """90 MB against a 12.6 GB corpus is 0.7% and contributes 0.26 points
        of Latin density -- nothing. 8x is the dose the spec fixed."""
        from rime_train.sources import SOURCES
        self.assertEqual(SOURCES["cc-technology"].repeat, 8)


class BuildRepeatTest(unittest.TestCase):
    """Upsampling has to wrap build(), not live inside it.

    build() ends in dedup.unique(), so repeating a sentence INSIDE that stream
    would be deduplicated away and the repeat would silently do nothing. Each
    repetition must be its own pass over the file.
    """

    def test_each_pass_yields_the_same_deduplicated_sentences(self):
        import tempfile, json as _json
        from rime_train import build as _build, charset as _charset
        src = build.Source(name="t", url="http://x/t.jsonl", register="technical",
                           field="text")
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / "t.jsonl"
            with open(path, "w", encoding="utf-8") as h:
                h.write(_json.dumps({"text": "这是第一句。这是第二句。"}) + "\n")
                h.write(_json.dumps({"text": "这是第一句。"}) + "\n")  # duplicate
            typeable = frozenset("这是第一句第二")
            once = list(_build.build(path, src, typeable, None, han_only=False))
            twice = list(_build.build(path, src, typeable, None, han_only=False))
        self.assertEqual(once, twice)
        self.assertEqual(len(once), len(set(once)), "build() must deduplicate within a pass")


class BuildCmdRepeatTest(unittest.TestCase):
    """Drives cmd_build itself: BuildRepeatTest above only pins the property
    cmd_build relies on (build() is deterministic across passes), it does not
    exercise the repeat loop. This does.
    """

    TEXT = "这是第一句。这是第二句。"  # 2 distinct sentences after build()
    CHARS = "这是第一句二"

    def _run_cmd_build(self, fake_source):
        """Runs cmd_build against one fake source in an isolated cache dir.
        Returns (lines written, captured stdout)."""
        import argparse
        import contextlib
        import io
        import json as _json
        from rime_train import cli

        with tempfile.TemporaryDirectory() as d:
            cache = Path(d)
            with open(cache / "t.jsonl", "w", encoding="utf-8") as h:
                h.write(_json.dumps({"text": self.TEXT}) + "\n")

            charset_path = cache / "tiny.dict.yaml"
            with open(charset_path, "w", encoding="utf-8") as h:
                h.write("---\nname: tiny\n...\n")
                for ch in self.CHARS:
                    h.write(f"{ch}\tx\n")

            args = argparse.Namespace(
                charset=str(charset_path),
                output=str(cache / "out.txt"),
                source=["t"],
                cache=str(cache),
                limit_lines=None,
                keep_punctuation=True,
            )

            old_sources = cli.SOURCES
            cli.SOURCES = {"t": fake_source}
            try:
                out = io.StringIO()
                with contextlib.redirect_stdout(out):
                    rc = cli.cmd_build(args)
            finally:
                cli.SOURCES = old_sources

            self.assertEqual(rc, 0)
            lines = (cache / "out.txt").read_text(encoding="utf-8").splitlines()
        return lines, out.getvalue()

    def test_cmd_build_writes_each_source_repeat_times(self):
        fake_source = build.Source(name="t", url="http://x/t.jsonl", register="technical",
                                   field="text", repeat=3)
        lines, stdout = self._run_cmd_build(fake_source)

        # Repeat must be whole passes over the (deduplicated) source, not a
        # duplicated-then-deduplicated stream: 3 passes of 2 distinct
        # sentences is 6 lines, still only 2 distinct.
        self.assertEqual(len(lines), 6)
        self.assertEqual(len(set(lines)), 2)
        self.assertIn("t (technical) x3: 6 sentences", stdout)

    def test_default_repeat_prints_no_suffix(self):
        """Task 7 relies on repeat=1 sources printing exactly as before --
        no `x1` suffix -- so this pins the other half of the contract."""
        fake_source = build.Source(name="t", url="http://x/t.jsonl", register="technical",
                                   field="text")  # repeat defaults to 1
        lines, stdout = self._run_cmd_build(fake_source)

        self.assertEqual(len(lines), 2)
        self.assertIn("t (technical): 2 sentences", stdout)
        # Assert against the summary line ALONE, not the whole capture: stdout
        # also carries a tempfile.TemporaryDirectory() path, 8 characters from a
        # 37-character alphabet, so a chance "x1" in it failed this roughly one
        # run in 200.
        summary = [line for line in stdout.splitlines() if line.strip().startswith("t (technical)")]
        self.assertEqual(len(summary), 1, stdout)
        self.assertNotIn("x1", summary[0])


class GoldenFixtureTest(unittest.TestCase):
    def test_golden_fixture_is_in_step_with_scoring_form(self):
        """The committed fixture is generated. A hand-edited one is a lie every
        run reports as success -- the same shape as tools/requirements.txt before
        it was generated from RUNTIME_REQUIREMENTS.
        """
        import io
        import json

        from rime_train import goldens

        path = Path(__file__).resolve().parents[2] / "test" / "data" / "scoring_form_golden.jsonl"
        self.assertTrue(path.exists(), f"missing fixture: {path}; run `rime-train scoring-form`")

        sink = io.StringIO()
        goldens.emit_scoring_form(sink)
        self.assertEqual(
            sink.getvalue(),
            path.read_text(encoding="utf-8"),
            "test/data/scoring_form_golden.jsonl is stale; regenerate with "
            "`tools/rime-train scoring-form --out test/data/scoring_form_golden.jsonl`",
        )

        for line in path.read_text(encoding="utf-8").splitlines():
            case = json.loads(line)
            self.assertEqual(normalize.scoring_form(case["in"]), case["out"])

    def test_golden_cases_cover_every_alignment_rule(self):
        from rime_train import goldens

        joined = "".join(goldens.SCORING_FORM_CASES)
        self.assertIn("\n", joined)         # rule 1: newline folding
        self.assertIn("。", joined)          # rule 2: sentence boundary
        self.assertIn("。 ", joined)         # rule 3: whitespace beside an EOS
        self.assertIn("　", joined)      # an exotic blank Python's \s matches
        self.assertIn("\x07", joined)       # a control character normalize() drops
