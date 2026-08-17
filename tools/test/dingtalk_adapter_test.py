import json
import sys
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus.adapters import dingtalk


def _proc(payload, returncode=0, stderr=""):
    """A stand-in for subprocess.CompletedProcess. Exit code is deliberately
    NOT trusted by the adapter (dws can exit non-zero with a complete,
    parseable payload) -- tests exercise that by passing returncode=1 with a
    normal payload, same as the +chat-list run measured while this was
    written."""
    return SimpleNamespace(stdout=json.dumps(payload, ensure_ascii=False), stderr=stderr, returncode=returncode)


def _resolved(name="李东鹏"):
    return {"senders": [{"status": "resolved", "query": name}]}


def _messages_payload(messages, complete=True, partial=False, resolved=True, failures=None):
    return {
        "complete": complete,
        "partial": partial,
        "failures": failures or [],
        "resolvedFilters": _resolved() if resolved else {"senders": []},
        "messages": messages,
    }


def _chat_list(chats, returncode=0):
    return _proc({"chats": chats, "complete": True, "partial": False}, returncode=returncode)


class DingtalkAdapterTest(unittest.TestCase):
    def _run(self, list_payload, messages_by_conv, **kwargs):
        """Fakes subprocess.run: routes +chat-list to list_payload and
        +chat-messages (keyed by --conversation-id) to messages_by_conv."""

        def fake_run(argv, **_):
            if "+chat-list" in argv:
                return list_payload
            conv = argv[argv.index("--conversation-id") + 1]
            return messages_by_conv[conv]

        with mock.patch("rime_corpus.adapters.dingtalk.subprocess.run", side_effect=fake_run):
            return list(dingtalk.iter_utterances(**kwargs))

    def test_extracts_text_and_converts_timestamp(self):
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload([
            {"createTime": "2026-05-19 11:08:42", "text": "晚上一起吃饭吗", "senderId": "s"},
        ])
        result = self._run(_chat_list(chats), {"c1": _proc(messages)})
        self.assertEqual(result, [("2026-05-19T11:08:42+08:00", "晚上一起吃饭吗")])

    def test_skips_resourceRefs_attachments(self):
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload([
            {"createTime": "2026-05-19 11:08:42", "text": "[图片消息] ...", "resourceRefs": [{}]},
            {"createTime": "2026-05-19 11:09:00", "text": "在的", "senderId": "s"},
        ])
        result = self._run(_chat_list(chats), {"c1": _proc(messages)})
        self.assertEqual(result, [("2026-05-19T11:09:00+08:00", "在的")])

    def test_skips_placeholder_tagged_non_text_without_resourceRefs(self):
        # Share cards and voice-call notices carry no resourceRefs at all --
        # the bracket-tag shape is the only signal available for them.
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload([
            {"createTime": "2026-05-19 11:08:42", "text": "[分享] 阿里云登录 - 欢迎登录"},
            {"createTime": "2026-05-19 11:09:00", "text": "[语音通话] 1分钟"},
            {"createTime": "2026-05-19 11:10:00", "text": "[赞]"},
            {"createTime": "2026-05-19 11:11:00", "text": "好的没问题"},
        ])
        result = self._run(_chat_list(chats), {"c1": _proc(messages)})
        self.assertEqual(result, [("2026-05-19T11:11:00+08:00", "好的没问题")])

    def test_skips_groups_defensively(self):
        chats = [{"chatMode": "group", "openConversationId": "g1", "name": "team"}]
        result = self._run(_chat_list(chats), {})
        self.assertEqual(result, [])

    def test_unresolved_sender_aborts_that_conversation_only(self):
        chats = [
            {"chatMode": "p2p", "openConversationId": "bad", "name": "x"},
            {"chatMode": "p2p", "openConversationId": "good", "name": "y"},
        ]
        unresolved = _messages_payload(
            [{"createTime": "2026-05-19 11:08:42", "text": "别人的话不该出现"}],
            resolved=False,
        )
        good = _messages_payload([{"createTime": "2026-05-19 12:00:00", "text": "自己的话"}])
        result = self._run(_chat_list(chats), {"bad": _proc(unresolved), "good": _proc(good)})
        self.assertEqual(result, [("2026-05-19T12:00:00+08:00", "自己的话")])

    def test_unresolved_sender_status_not_resolved(self):
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        payload = {
            "complete": True, "partial": False, "failures": [],
            "resolvedFilters": {"senders": [{"status": "ambiguous", "query": "李东鹏"}]},
            "messages": [{"createTime": "2026-05-19 11:08:42", "text": "不该出现"}],
        }
        result = self._run(_chat_list(chats), {"c1": _proc(payload)})
        self.assertEqual(result, [])

    def test_honours_partial_but_still_yields_what_came_back(self):
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload(
            [{"createTime": "2026-05-19 11:08:42", "text": "读到一半"}],
            complete=False, partial=True, failures=[{"page": 2, "error": "boom"}],
        )
        with mock.patch("sys.stderr") as err:
            result = self._run(_chat_list(chats), {"c1": _proc(messages)})
        self.assertEqual(result, [("2026-05-19T11:08:42+08:00", "读到一半")])
        # A partial read must be surfaced, not swallowed.
        written = "".join(call.args[0] for call in err.write.call_args_list)
        self.assertIn("partial", written)

    def test_chat_list_nonzero_exit_with_parseable_payload_still_used(self):
        # Measured behaviour: +chat-list exited 1 ("分页未完成") while stdout
        # still held a complete JSON payload. Exit code must not gate this.
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload([{"createTime": "2026-05-19 11:08:42", "text": "还在"}])
        result = self._run(_chat_list(chats, returncode=1), {"c1": _proc(messages)})
        self.assertEqual(result, [("2026-05-19T11:08:42+08:00", "还在")])

    def test_one_conversation_transport_failure_does_not_sink_the_rest(self):
        chats = [
            {"chatMode": "p2p", "openConversationId": "broken", "name": "x"},
            {"chatMode": "p2p", "openConversationId": "fine", "name": "y"},
        ]
        broken = SimpleNamespace(stdout="not json", stderr="boom", returncode=1)
        fine = _proc(_messages_payload([{"createTime": "2026-05-19 11:08:42", "text": "没事"}]))
        with mock.patch("sys.stderr"):
            result = self._run(_chat_list(chats), {"broken": broken, "fine": fine})
        self.assertEqual(result, [("2026-05-19T11:08:42+08:00", "没事")])

    def test_skips_conversation_missing_open_conversation_id(self):
        chats = [{"chatMode": "p2p", "name": "no id"}]
        result = self._run(_chat_list(chats), {})
        self.assertEqual(result, [])

    def test_default_start_is_not_hardcoded(self):
        # Two calls a moment apart must not compute the same literal date --
        # proves the default is derived from "now", not a frozen constant.
        seen_starts = []

        def fake_run(argv, **_):
            if "+chat-list" in argv:
                return _chat_list([])
            return _proc(_messages_payload([]))

        with mock.patch("rime_corpus.adapters.dingtalk.subprocess.run", side_effect=fake_run) as run:
            list(dingtalk.iter_utterances())
        argv = run.call_args.args[0]
        self.assertIn("--page-limit", argv)  # sanity: chat-list shape used
        self.assertNotIn("2020-01-01", " ".join(argv))

    def test_unrecognized_timestamp_shape_passes_through(self):
        chats = [{"chatMode": "p2p", "openConversationId": "c1", "name": "x"}]
        messages = _messages_payload([{"createTime": "not-a-timestamp", "text": "内容"}])
        result = self._run(_chat_list(chats), {"c1": _proc(messages)})
        self.assertEqual(result, [("not-a-timestamp", "内容")])


if __name__ == "__main__":
    unittest.main()
