import json
import tempfile
import unittest
from pathlib import Path

from rime_corpus.adapters import claude


def _line(**kw):
    base = {
        "type": "user",
        "timestamp": "2026-08-14T10:00:00.000Z",
        "promptSource": "typed",
        "message": {"content": "这个顺序是故意的"},
    }
    base.update(kw)
    return json.dumps(base, ensure_ascii=False)


class ClaudeAdapterTest(unittest.TestCase):
    def _root(self, lines):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        project = Path(tmp.name) / "-Users-x-repo"
        project.mkdir(parents=True)
        (project / "s.jsonl").write_text("\n".join(lines) + "\n", encoding="utf-8")
        return Path(tmp.name)

    def test_yields_typed_prompts(self):
        root = self._root([_line()])
        self.assertEqual(
            list(claude.iter_utterances(root)),
            [("2026-08-14T10:00:00.000Z", "这个顺序是故意的")],
        )

    def test_skips_tool_results_whose_content_is_a_list(self):
        root = self._root([_line(message={"content": [{"type": "tool_result"}]})])
        self.assertEqual(list(claude.iter_utterances(root)), [])

    def test_skips_assistant_lines(self):
        root = self._root([_line(type="assistant")])
        self.assertEqual(list(claude.iter_utterances(root)), [])

    def test_skips_system_and_absent_prompt_source(self):
        root = self._root([_line(promptSource="system")])
        self.assertEqual(list(claude.iter_utterances(root)), [])
        root = self._root([json.dumps({"type": "user", "message": {"content": "嗨"}})])
        self.assertEqual(list(claude.iter_utterances(root)), [])

    def test_skips_suggestion_accepted(self):
        root = self._root([_line(promptSource="suggestion_accepted")])
        self.assertEqual(list(claude.iter_utterances(root)), [])

    def test_accepts_queued(self):
        root = self._root([_line(promptSource="queued")])
        self.assertEqual(len(list(claude.iter_utterances(root))), 1)

    def test_skips_harness_injected_wrappers(self):
        for text in ("<task-notification>\nx", "<command-name>/compact</command-name>"):
            root = self._root([_line(message={"content": text})])
            self.assertEqual(list(claude.iter_utterances(root)), [], text)

    def test_skips_content_carrying_a_system_reminder(self):
        root = self._root([_line(message={"content": "看下\n<system-reminder>x</system-reminder>"})])
        self.assertEqual(list(claude.iter_utterances(root)), [])

    def test_survives_a_corrupt_line(self):
        root = self._root(["{not json", _line()])
        self.assertEqual(len(list(claude.iter_utterances(root))), 1)


if __name__ == "__main__":
    unittest.main()
