"""The vocabulary, defined once for both training and GGUF export.

The whole reason a ~40M model can do this job is that it does not carry a
general-purpose vocabulary. Qwen3-0.6B spends 155.6M parameters -- 31% of
itself -- on a 151,936-token multilingual table (read from the deployed gguf,
not from memory). This one is 8,536 tokens, and at `d_model` 512 that is 4.4M
parameters of embedding.

Three parts, in a fixed order that the GGUF token ids depend on:

  0-2       <unk> <s> </s>
  3-258     the 256 SPM byte-fallback tokens <0x00>..<0xFF>
  259+      every single character of the schema's own character table,
            then printable ASCII not already present

Byte fallback is not decoration: it is what llama.cpp's SPM loader looks the
newline token up in, and it is what a character-level model needs for anything
outside a fixed table -- the table is 8k characters and the world is not.
"""
from __future__ import annotations

from pathlib import Path
from typing import Sequence

SPECIALS = ["<unk>", "<s>", "</s>"]
UNK_ID, BOS_ID, EOS_ID = 0, 1, 2
BYTE_TOKENS = [f"<0x{b:02X}>" for b in range(256)]


def characters(dict_path: Path) -> list[str]:
    """Single characters from a Rime `.dict.yaml`, in file order.

    Read directly rather than through `rime_copilot.dictfile`: that parser
    resolves missing readings with pypinyin, which this does not need and
    which would make the vocabulary depend on a package the training host may
    not have.
    """
    seen: set[str] = set()
    out: list[str] = []
    with open(dict_path, encoding="utf-8") as handle:
        for line in handle:
            parts = line.strip().split("\t")
            if parts and len(parts[0]) == 1 and parts[0] not in seen:
                seen.add(parts[0])
                out.append(parts[0])
    return out


def build(dict_path: Path) -> list[str]:
    """The full token list, in GGUF id order."""
    pieces = list(SPECIALS) + list(BYTE_TOKENS)
    seen = set(pieces)
    for ch in characters(dict_path):
        if ch not in seen:
            seen.add(ch)
            pieces.append(ch)
    for code in range(32, 127):
        ch = chr(code)
        if ch not in seen:
            seen.add(ch)
            pieces.append(ch)
    return pieces


class Vocab:
    """Character-level encoding with byte fallback.

    Encoding matches what llama.cpp's SPM tokenizer will do with this token
    list at inference: a known character becomes its own id, and anything else
    becomes its UTF-8 bytes. Training on a different segmentation from the one
    inference uses would make every measured score wrong in a way no test
    downstream could see.
    """

    def __init__(self, pieces: Sequence[str]):
        self.pieces = list(pieces)
        self.ids = {piece: i for i, piece in enumerate(self.pieces)}
        self.byte_base = len(SPECIALS)

    def __len__(self) -> int:
        return len(self.pieces)

    def encode(self, text: str, bos: bool = False) -> list[int]:
        out = [BOS_ID] if bos else []
        for ch in text:
            token = self.ids.get(ch)
            if token is not None:
                out.append(token)
            else:
                out.extend(self.byte_base + b for b in ch.encode("utf-8"))
        return out

    def decode(self, ids: Sequence[int]) -> str:
        chars: list[str] = []
        pending = bytearray()

        def flush() -> None:
            if pending:
                chars.append(pending.decode("utf-8", errors="replace"))
                pending.clear()

        for i in ids:
            if self.byte_base <= i < self.byte_base + 256:
                pending.append(i - self.byte_base)
                continue
            flush()
            if i >= self.byte_base + 256:
                chars.append(self.pieces[i])
        flush()
        return "".join(chars)
