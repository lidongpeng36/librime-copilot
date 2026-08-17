import unittest

from rime_corpus import speller


class FlypyTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.sp = speller.Speller(speller.load_rules(speller.FLYPY_RULES))

    def test_ordinary_syllables(self):
        cases = {
            "jian": "jm", "gu": "gu", "yi": "yi", "de": "de", "shi": "ui",
            "zhong": "vs", "xue": "xt", "guo": "go", "chuang": "il",
            "zhuan": "vr", "qiong": "qs", "xiang": "xl", "niao": "nn",
            "hui": "hv", "jun": "jy", "wu": "wu", "ye": "ye", "lv": "lv",
        }
        for syllable, want in cases.items():
            self.assertEqual(self.sp.keys(syllable), want, syllable)

    def test_zero_initial_syllables_go_through_the_derive_branch(self):
        # `ou` is the case where the ORIGINAL survives sorted()[0] rather than
        # the derive branch: {ou, oz} sorts to "ou". Both are spellings this
        # schema accepts -- that is what `derive` means -- so both produce the
        # same candidates. Do not "fix" this to "oz" without re-running
        # verify-speller, which is the authority.
        cases = {"a": "aa", "o": "oo", "e": "ee", "ai": "ad", "an": "aj",
                 "en": "ef", "ou": "ou", "ang": "ah", "eng": "eg", "er": "er"}
        for syllable, want in cases.items():
            self.assertEqual(self.sp.keys(syllable), want, syllable)

    def test_single_letter_syllables_are_unspellable(self):
        """呣/嗯 are `m` and `n` in pinyin -- no initial, no final, so no rule
        produces two keys. build_requests drops any run containing them."""
        self.assertIsNone(self.sp.keys("m"))
        self.assertIsNone(self.sp.keys("n"))

    def test_the_one_inherent_collision_is_preserved(self):
        """lo and luo both spell `lo` in 小鹤. This is the scheme's own
        ambiguity, which a real user also types, so replay must reproduce it
        rather than paper over it."""
        self.assertEqual(self.sp.keys("lo"), "lo")
        self.assertEqual(self.sp.keys("luo"), "lo")

    def test_every_result_is_exactly_two_keys(self):
        for syllable in ("zhuang", "shuang", "chuang", "xiong", "jiong"):
            self.assertEqual(len(self.sp.keys(syllable)), 2, syllable)

    def test_unknown_syllable_returns_none(self):
        self.assertIsNone(self.sp.keys("zzz"))

    def test_result_is_deterministic_across_calls(self):
        first = [self.sp.keys("ai") for _ in range(5)]
        self.assertEqual(len(set(first)), 1)


class SyllablesTest(unittest.TestCase):
    def test_splits_han_into_syllables(self):
        self.assertEqual(speller.syllables("故意的"), ["gu", "yi", "de"])

    def test_one_syllable_per_han_character(self):
        text = "高屋建瓴"
        self.assertEqual(len(speller.syllables(text)), len(text))


if __name__ == "__main__":
    unittest.main()
