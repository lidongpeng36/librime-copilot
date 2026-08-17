import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_corpus import evalset


class BuildEvalsetTest(unittest.TestCase):
    def test_includes_bucket_a_segment_with_han_context(self):
        requests = [{"id": "r1", "ctx": "看看这个字", "keys": "..", "text": "故意"}]
        responses = [
            {
                "id": "r1",
                "status": "ok",
                "segments": [{"span": [0, 4], "want": "故意", "cands": ["故意", "顾忌"], "hit": 0}],
            }
        ]
        out = evalset.build_evalset(requests, responses, window=32)
        self.assertEqual(len(out), 1)
        rec = out[0]
        self.assertEqual(rec["bucket"], "A")
        self.assertEqual(rec["gold"], "故意")
        self.assertEqual(rec["gold_idx"], 0)
        self.assertEqual(rec["ctx"], "看看这个字")
        self.assertEqual(rec["cands"], ["故意", "顾忌"])

    def test_excludes_segment_with_no_trailing_han_context(self):
        # Segment 1 of every request is, by construction, preceded by
        # non-Han text -- it starts right where a maximal Han run begins.
        requests = [{"id": "r1", "ctx": "see this ", "keys": "..", "text": "故意"}]
        responses = [
            {
                "id": "r1",
                "status": "ok",
                "segments": [{"span": [0, 4], "want": "故意", "cands": ["故意"], "hit": 0}],
            }
        ]
        self.assertEqual(evalset.build_evalset(requests, responses, window=32), [])

    def test_second_segment_sees_the_first_segments_committed_text(self):
        """Nothing in the response carries per-segment context back directly
        -- it must be reconstructed exactly as replay_copilot.cc's own
        committed_so_far does: request ctx, plus what earlier segments in
        THIS response actually committed."""
        requests = [{"id": "r1", "ctx": "见", "keys": "....", "text": "故意的"}]
        responses = [
            {
                "id": "r1",
                "status": "ok",
                "segments": [
                    {"span": [0, 2], "want": "故", "cands": ["故", "顾"], "hit": 0},
                    {"span": [2, 6], "want": "意的", "cands": ["估计", "意的"], "hit": 1},
                ],
            }
        ]
        out = evalset.build_evalset(requests, responses, window=32)
        self.assertEqual(len(out), 2)
        self.assertEqual(out[0]["ctx"], "见")
        self.assertEqual(out[0]["bucket"], "A")
        self.assertEqual(out[1]["ctx"], "见故")
        self.assertEqual(out[1]["bucket"], "C")
        self.assertEqual(out[1]["gold_idx"], 1)

    def test_bucket_b_is_excluded(self):
        # cands[0] is not all-Han -- RawInputFilter's deliberate policy, not
        # a re-ranking target -- so this segment must not be exported at all.
        requests = [{"id": "r1", "ctx": "见", "keys": "..", "text": "故意"}]
        responses = [
            {
                "id": "r1",
                "status": "ok",
                "segments": [{"span": [0, 4], "want": "故意", "cands": ["gy故意", "故意"], "hit": 1}],
            }
        ]
        self.assertEqual(evalset.build_evalset(requests, responses, window=32), [])

    def test_bucket_d_is_excluded(self):
        requests = [{"id": "r1", "ctx": "见", "keys": "..", "text": "故意"}]
        responses = [
            {
                "id": "r1",
                "status": "diverged",
                "segments": [{"span": [0, 4], "want": "故意", "cands": [], "hit": -1}],
            }
        ]
        self.assertEqual(evalset.build_evalset(requests, responses, window=32), [])

    def test_error_responses_are_excluded(self):
        requests = [{"id": "r1", "ctx": "见", "keys": "..", "text": "故意"}]
        responses = [{"id": "r1", "status": "error", "segments": []}]
        self.assertEqual(evalset.build_evalset(requests, responses, window=32), [])

    def test_response_with_no_matching_request_is_skipped(self):
        responses = [
            {
                "id": "unknown",
                "status": "ok",
                "segments": [{"span": [0, 4], "want": "故意", "cands": ["故意"], "hit": 0}],
            }
        ]
        self.assertEqual(evalset.build_evalset([], responses, window=32), [])

    def test_segment_id_is_response_id_plus_segment_index(self):
        requests = [{"id": "abc#0", "ctx": "见", "keys": "..", "text": "故意"}]
        responses = [
            {
                "id": "abc#0",
                "status": "ok",
                "segments": [{"span": [0, 4], "want": "故意", "cands": ["故意"], "hit": 0}],
            }
        ]
        out = evalset.build_evalset(requests, responses, window=32)
        self.assertEqual(out[0]["id"], "abc#0:0")


if __name__ == "__main__":
    unittest.main()
