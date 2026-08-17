import json
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import corpus


class MakeRecordTest(unittest.TestCase):
    def test_keeps_mixed_script(self):
        rec = corpus.make_record("claude", "2026-08-14T10:00:00+08:00", "这个 PR 我看下")
        self.assertIsNotNone(rec)
        self.assertEqual(rec["text"], "这个 PR 我看下")
        self.assertEqual(rec["v"], 1)
        self.assertEqual(rec["src"], "claude")

    def test_drops_text_with_no_han(self):
        self.assertIsNone(corpus.make_record("claude", "t", "just english here"))

    def test_drops_overlong_text(self):
        long_text = "这" * (corpus.MAX_CHARS + 1)
        self.assertIsNone(corpus.make_record("claude", "t", long_text))

    def test_id_is_stable_and_content_addressed(self):
        a = corpus.make_record("claude", "t1", "故意的")
        b = corpus.make_record("dingtalk", "t2", "故意的")
        self.assertEqual(a["id"], b["id"])
        self.assertEqual(len(a["id"]), 16)

    def test_redaction_is_applied_and_reported(self):
        rec = corpus.make_record("claude", "t", "发到 a@b.com 这个邮箱")
        self.assertEqual(rec["text"], "发到 ⟦EMAIL⟧ 这个邮箱")
        self.assertEqual(rec["redacted"], ["email"])

    def test_id_is_of_redacted_text(self):
        """Two utterances differing only in a redacted span collapse to one id."""
        a = corpus.make_record("claude", "t", "发到 a@b.com 这个邮箱")
        b = corpus.make_record("claude", "t", "发到 c@d.com 这个邮箱")
        self.assertEqual(a["id"], b["id"])


class AppendTest(unittest.TestCase):
    def test_append_skips_ids_already_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "claude.jsonl"
            recs = [corpus.make_record("claude", "t", "故意的")]
            self.assertEqual(corpus.append(path, recs), 1)
            self.assertEqual(corpus.append(path, recs), 0)
            self.assertEqual(len(list(corpus.iter_records(path))), 1)

    def test_append_deduplicates_within_one_batch(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "claude.jsonl"
            rec = corpus.make_record("claude", "t", "故意的")
            self.assertEqual(corpus.append(path, [rec, dict(rec)]), 1)

    def test_written_lines_are_one_json_object_each(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "claude.jsonl"
            corpus.append(path, [corpus.make_record("claude", "t", "故意的")])
            lines = path.read_text(encoding="utf-8").splitlines()
            self.assertEqual(len(lines), 1)
            self.assertEqual(json.loads(lines[0])["text"], "故意的")

    def test_chinese_is_not_escaped_on_disk(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "claude.jsonl"
            corpus.append(path, [corpus.make_record("claude", "t", "故意的")])
            self.assertIn("故意的", path.read_text(encoding="utf-8"))


class HanClassTest(unittest.TestCase):
    def test_matches_every_han_range(self):
        for char in ("汉", "㐀", "豈", "\U00020000"):
            self.assertTrue(corpus.has_han(char), repr(char))

    def test_does_not_match_korean_or_private_use(self):
        """U+F900 and U+8C48 are both 豈. Writing the range with literal
        characters once made this class span [8C48-FAFF], swallowing Hangul
        and the entire PUA."""
        for char in ("안녕하세요", "", "abc", "、", "１"):
            self.assertFalse(corpus.has_han(char), repr(char))


class CorpusDirTest(unittest.TestCase):
    def test_env_var_wins(self):
        import os

        old = os.environ.get("RIME_CORPUS_DIR")
        os.environ["RIME_CORPUS_DIR"] = "/tmp/xyz-corpus"
        try:
            self.assertEqual(corpus.corpus_dir(), Path("/tmp/xyz-corpus"))
        finally:
            if old is None:
                del os.environ["RIME_CORPUS_DIR"]
            else:
                os.environ["RIME_CORPUS_DIR"] = old

    def test_explicit_wins_over_env(self):
        self.assertEqual(corpus.corpus_dir("/tmp/abc"), Path("/tmp/abc"))


if __name__ == "__main__":
    unittest.main()
