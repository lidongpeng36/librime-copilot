import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import redact


class RedactTest(unittest.TestCase):
    def test_email_is_replaced_and_reported(self):
        out, cats = redact.redact("联系我 lidongpeng.ai@gmail.com 谢谢")
        self.assertEqual(out, "联系我 ⟦EMAIL⟧ 谢谢")
        self.assertEqual(cats, ["email"])

    def test_mobile_number(self):
        out, cats = redact.redact("我的号码 13800138000")
        self.assertEqual(out, "我的号码 ⟦PHONE⟧")
        self.assertEqual(cats, ["phone"])

    def test_api_key_shapes(self):
        out, cats = redact.redact("用 sk-abcd1234EFGH5678ijkl 这个")
        self.assertEqual(out, "用 ⟦KEY⟧ 这个")
        self.assertEqual(cats, ["key"])

    def test_home_path_username(self):
        out, cats = redact.redact("看 /Users/lidongpeng/repo/x 这里")
        self.assertEqual(out, "看 ⟦PATH⟧/repo/x 这里")
        self.assertEqual(cats, ["path"])

    def test_url_including_query(self):
        out, cats = redact.redact("见 https://a.example.com/x?token=abc 说明")
        self.assertEqual(out, "见 ⟦URL⟧ 说明")
        self.assertEqual(cats, ["url"])

    def test_categories_are_sorted_and_deduplicated(self):
        out, cats = redact.redact("a@b.com 和 c@d.com 还有 1.2.3.4")
        self.assertEqual(cats, ["email", "ip"])
        self.assertNotIn("@", out)

    def test_clean_text_is_untouched(self):
        out, cats = redact.redact("这个 filter 的顺序是故意的")
        self.assertEqual(out, "这个 filter 的顺序是故意的")
        self.assertEqual(cats, [])

    def test_han_ranges_match_the_corpus_definition(self):
        from rime_corpus import corpus
        self.assertEqual(redact._HAN_RANGES, corpus.HAN_CLASS)

    def test_pii_glued_directly_to_han_text(self):
        """Chinese has no spaces around inline URLs, emails or tokens, so this is
        the common case in this corpus -- and it is what the \\b-anchored rules
        silently failed on."""
        cases = [
            ("我的邮箱a@b.com给你", "我的邮箱⟦EMAIL⟧给你", "email"),
            ("见https://a.example.com/x说明", "见⟦URL⟧说明", "url"),
            ("用sk-abcd1234EFGH5678ijkl这个", "用⟦KEY⟧这个", "key"),
            ("号码13800138000找我", "号码⟦PHONE⟧找我", "phone"),
            ("在/Users/lidongpeng的目录下", "在⟦PATH⟧的目录下", "path"),
        ]
        for text, want, category in cases:
            out, cats = redact.redact(text)
            self.assertEqual(out, want, text)
            self.assertIn(category, cats, text)


if __name__ == "__main__":
    unittest.main()
