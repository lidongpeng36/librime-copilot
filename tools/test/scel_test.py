"""Sogou .scel unpacking, driven by synthetic blobs.

Format (from the file header layout Sogou uses):
  byte 4          0x44 -> word table at 0x2628, 0x45 -> at 0x26c4
  0x1540+4        pinyin table: (index u16, byte length u16, UTF-16LE syllable)
  word table      (homophone count u16, index-array byte length u16,
                   index u16 ..., then per word: byte length u16, UTF-16LE
                   word, ext length u16, 10 ext bytes)
"""
from __future__ import annotations

import struct
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_copilot.scel import merge_scel_dir, unpack


def build_scel(records, mask=0x44) -> bytes:
    """records: [(["a", "an"], ["阿安", "啊安"]), ...]"""
    hz_offset = 0x2628 if mask == 0x44 else 0x26c4
    buf = bytearray(hz_offset)
    buf[4] = mask
    for offset, text in ((0x130, "title"), (0x338, "category"),
                         (0x540, "desc"), (0xd40, "samples")):
        blob = text.encode("utf-16-le")
        buf[offset:offset + len(blob)] = blob

    syllables = sorted({s for pinyin, _ in records for s in pinyin})
    index = {s: i for i, s in enumerate(syllables)}
    cursor = 0x1540 + 4
    for syllable, i in sorted(index.items(), key=lambda kv: kv[1]):
        blob = syllable.encode("utf-16-le")
        struct.pack_into("<HH", buf, cursor, i, len(blob))
        cursor += 4
        buf[cursor:cursor + len(blob)] = blob
        cursor += len(blob)

    body = bytearray()
    for pinyin, words in records:
        body += struct.pack("<HH", len(words), 2 * len(pinyin))
        for syllable in pinyin:
            body += struct.pack("<H", index[syllable])
        for word in words:
            blob = word.encode("utf-16-le")
            body += struct.pack("<H", len(blob)) + blob + struct.pack("<H", 10) + bytes(10)
    return bytes(buf) + bytes(body)


class Unpack(unittest.TestCase):
    def test_single_record(self):
        data = build_scel([(["a", "an"], ["阿安"])])
        self.assertEqual([("a an", "阿安")], unpack(data))

    def test_homophones_share_a_pinyin(self):
        data = build_scel([(["a", "an"], ["阿安", "啊安"])])
        self.assertEqual([("a an", "阿安"), ("a an", "啊安")], unpack(data))

    def test_alternate_word_table_offset(self):
        data = build_scel([(["a"], ["啊"])], mask=0x45)
        self.assertEqual([("a", "啊")], unpack(data))

    def test_unknown_mask_is_rejected(self):
        data = bytearray(build_scel([(["a"], ["啊"])]))
        data[4] = 0x99
        with self.assertRaises(ValueError):
            unpack(bytes(data))

    def test_truncated_file_does_not_hang_or_raise(self):
        data = build_scel([(["a"], ["啊"]), (["b"], ["吧"])])
        # Chop mid-record; the reader must stop, not loop or explode.
        self.assertIsInstance(unpack(data[:-7]), list)

    def test_odd_index_bytes_trailing_garbage(self):
        # Two valid records, then trailing garbage with odd index_bytes.
        data = build_scel([(["a"], ["啊"]), (["b"], ["吧"])])
        # Append a header with odd index_bytes (1281 = 0x0501) and enough bytes
        # so the read is complete rather than short. This tests the odd-length
        # path is reached with a full buffer, not just truncation.
        garbage_header = struct.pack("<HH", 1, 1281) + bytes(1281)
        data_with_garbage = data + garbage_header
        records = unpack(data_with_garbage)
        # Both valid records should survive.
        self.assertEqual([("a", "啊"), ("b", "吧")], records)

    def test_odd_length_word_trailing_garbage(self):
        # Start with one valid record.
        data = build_scel([(["a"], ["啊"])])
        # Append a second record with odd word length (3, requesting 3 bytes
        # for UTF-16LE, which is incomplete). The first record should survive.
        # Must provide enough trailing bytes so stream.read(12) for the ext field
        # completes, otherwise the pre-existing truncation guard returns early
        # and never reaches word.decode().
        # Format: homophones (1), index_bytes (2), index (0 for 'a'),
        # word_length (3, odd), 3-byte word, 12-byte ext.
        bad_record = struct.pack("<HHHH", 1, 2, 0, 3) + bytes(3) + bytes(12)
        data_with_bad_word = data + bad_record
        records = unpack(data_with_bad_word)
        # The valid record should survive; the bad one should not.
        self.assertEqual([("a", "啊")], records)


class MergeScelDir(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.dir = Path(self.tmp.name)

    def tearDown(self):
        self.tmp.cleanup()

    def test_dedups_across_files_keeping_first_occurrence_order(self):
        (self.dir / "one.scel").write_bytes(
            build_scel([(["a", "an"], ["阿安"]), (["a", "ba"], ["阿爸"])]))
        (self.dir / "two.scel").write_bytes(
            build_scel([(["a", "an"], ["阿安"]), (["a", "die"], ["阿爹"])]))
        entries, names = merge_scel_dir(self.dir, weight=100)
        self.assertEqual(["阿安", "阿爸", "阿爹"], [e.word for e in entries])
        self.assertEqual([100, 100, 100], [e.weight for e in entries])
        self.assertEqual({"one", "two"}, set(names))

    def test_ignores_non_scel_files(self):
        (self.dir / "one.scel").write_bytes(build_scel([(["a"], ["啊"])]))
        (self.dir / "notes.txt").write_text("ignore me", encoding="utf-8")
        entries, names = merge_scel_dir(self.dir)
        self.assertEqual(["啊"], [e.word for e in entries])
        self.assertEqual(["one"], names)


if __name__ == "__main__":
    unittest.main()
