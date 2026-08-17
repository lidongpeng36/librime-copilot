"""Regression tests for tools/replay_copilot.cc's matching + isolation.

Not a unit test in the ordinary sense: it drives the REAL replay_copilot
binary against a REAL deployed Rime directory (task-5-brief.md Step 5), so it
needs a real schema, dictionary, and a running ImeBridge -- it cannot run in
CI, the same way Task 6's verify-speller cannot (task-6-brief.md). Skipped,
not failed, when the binary or the replay directory named by
REPLAY_COPILOT / REPLAY_RIME_DIR are absent.

Guards two findings from task 5's review round (see task-5-report.md):

  - Critical 1: a fixed +2-key stride measured one character at a time
    against a candidate list that could span the WHOLE remaining input,
    manufacturing "opportunities" out of segments where the correct,
    already-first answer was a multi-character phrase. The fix matches the
    longest candidate that is a prefix of what's left, and stops the request
    on divergence instead of continuing with a fictional span.

  - Critical 2: RimeClearComposition does not clear
    Context::commit_history(), which auto_spacer_filter reads directly
    (plugins/copilot/src/filters.cc) to decide whether to prepend a space at
    a CJK/Latin boundary. Left uncleared, one request's auto-committed text
    silently changed the candidate list of the very next, unrelated request.
    The fix clears commit history after every request too.
"""

from __future__ import annotations

import copy
import json
import os
import subprocess
import unittest

REPLAYER = os.environ.get(
    "REPLAY_COPILOT",
    "/Users/lidongpeng/repo/librime/build/plugins/copilot/bin/replay_copilot",
)
RIME_DIR = os.environ.get(
    "REPLAY_RIME_DIR", os.path.expanduser("~/.local/share/rime-corpus/rime-dir")
)

_AVAILABLE = os.path.exists(REPLAYER) and os.path.isdir(RIME_DIR)
_SKIP_REASON = (
    "needs a built replay_copilot and a deployed replay Rime directory "
    "(task-5-brief.md Step 5) -- set REPLAY_COPILOT / REPLAY_RIME_DIR"
)


def _run(requests: list[dict]) -> list[dict]:
    payload = "".join(json.dumps(r, ensure_ascii=False) + "\n" for r in requests)
    result = subprocess.run(
        [REPLAYER, "--rime-dir", RIME_DIR],
        input=payload,
        capture_output=True,
        text=True,
        timeout=60,
    )
    if result.returncode != 0:
        raise AssertionError(f"replay_copilot exited {result.returncode}: {result.stderr}")
    return [json.loads(line) for line in result.stdout.splitlines() if line.strip()]


def _comparable(response: dict) -> dict:
    """Strips fields that are expected to differ between two otherwise-
    identical runs: `id` (the two requests are deliberately given different
    ids so their responses can be told apart) and `us` (wall-clock timing)."""
    stripped = copy.deepcopy(response)
    stripped.pop("id", None)
    for segment in stripped.get("segments", []):
        segment.pop("us", None)
    return stripped


@unittest.skipUnless(_AVAILABLE, _SKIP_REASON)
class LongestPrefixMatchTest(unittest.TestCase):
    def test_guyide_matches_the_whole_phrase_in_one_segment(self):
        """A fixed +2 stride reported THREE segments here
        (want='故','意','的', hit=12,18,0), manufacturing two "opportunities"
        out of a candidate list where the full, correct phrase was already
        first. It must be exactly one segment."""
        responses = _run([{"id": "t1", "ctx": "", "keys": "guyide", "text": "故意的"}])
        self.assertEqual(len(responses), 1)
        response = responses[0]
        self.assertEqual(response["status"], "ok")
        self.assertEqual(len(response["segments"]), 1)
        segment = response["segments"][0]
        self.assertEqual(segment["want"], "故意的")
        self.assertEqual(segment["hit"], 0)
        self.assertEqual(segment["span"], [0, 6])


@unittest.skipUnless(_AVAILABLE, _SKIP_REASON)
class CommitHistoryIsolationTest(unittest.TestCase):
    def test_a_committing_request_does_not_poison_the_next_one(self):
        """Completing 故意的 auto-commits it. Without clearing
        Context::commit_history() afterward, the very next request's literal-
        fallback candidate ('deyi') came back prefixed with a stray space --
        a CJK-to-Latin auto-spacer boundary manufactured from stale state,
        not from anything in the b1/b2 request itself. b1, run right after
        the commit, must be identical to b2, run in isolation."""
        clean = _run([{"id": "b2", "ctx": "", "keys": "deyi", "text": "得意"}])
        stream = _run(
            [
                {"id": "a", "ctx": "", "keys": "guyide", "text": "故意的"},
                {"id": "b1", "ctx": "", "keys": "deyi", "text": "得意"},
            ]
        )
        self.assertEqual(len(stream), 2)
        b1, b2 = stream[1], clean[0]
        self.assertEqual(_comparable(b1), _comparable(b2))


if __name__ == "__main__":
    unittest.main()
