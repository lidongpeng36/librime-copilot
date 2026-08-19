"""Emit a random-weight Llama-architecture GGUF of a given size.

S2-a: the runtime budget (p99 < 10ms, < 200MB) implies a parameter count only
if the inference throughput is known, and the only number available for that
was reverse-engineered from Qwen3-0.6B running through an UNBATCHED,
per-candidate KV-branching scorer -- which spans a 5x range depending on how
much of that inefficiency is inherent. Guessing costs a whole training run at
the wrong size. Measuring costs an hour.

Random weights are the point: latency does not depend on what the weights are,
only on their shape, so this measures the constraint without training anything.
"""
import argparse
import os
import sys
from pathlib import Path

# gguf-py ships inside the llama.cpp source CMake already fetches for this
# plugin (librime/build/_deps, four directories up from here), so there is
# nothing extra to install and nothing that can drift from the llama.cpp the
# plugin actually links. Override with GGUF_PY.
GGUF_PY = os.environ.get(
    "GGUF_PY",
    str(Path(__file__).resolve().parents[4] / "build/_deps/llama-src/gguf-py"),
)
sys.path.insert(0, GGUF_PY)

import numpy as np  # noqa: E402
from gguf import GGUFWriter, TokenType  # noqa: E402


def han_vocab(chars_path):
    """Single-character pieces: every character the schema can type, plus
    ASCII. The vocabulary is where a general model's capacity goes -- 31% of
    Qwen3-0.6B is a 151,936-token multilingual table -- so the whole point of
    training our own is that this one is ~10k, not 150k."""
    seen, vocab = set(), []
    for line in open(chars_path, encoding="utf-8"):
        parts = line.strip().split("\t")
        if len(parts) >= 2 and len(parts[0]) == 1 and parts[0] not in seen:
            seen.add(parts[0])
            vocab.append(parts[0])
    for code in range(32, 127):
        ch = chr(code)
        if ch not in seen:
            seen.add(ch)
            vocab.append(ch)
    return vocab


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--layers", type=int, required=True)
    ap.add_argument("--dim", type=int, required=True)
    ap.add_argument("--ffn", type=int, required=True)
    ap.add_argument("--heads", type=int, default=6)
    ap.add_argument("--kv-heads", type=int, default=None)
    ap.add_argument("--ctx", type=int, default=512)
    ap.add_argument("--chars", required=True,
                    help="a .dict.yaml whose single-character entries become the vocabulary")
    args = ap.parse_args()

    # SPM needs the 256 byte tokens: llama.cpp looks up the newline byte by
    # name at load time, and without them there is no fallback for a character
    # outside the vocabulary either -- which a char-level model needs by
    # definition, since the table is 8k characters and the world is not.
    byte_tokens = [f"<0x{b:02X}>" for b in range(256)]
    specials = ["<unk>", "<s>", "</s>"]
    pieces = specials + byte_tokens + han_vocab(args.chars)
    n_vocab = len(pieces)
    kv_heads = args.kv_heads or args.heads
    head_dim = args.dim // args.heads

    w = GGUFWriter(args.out, "llama")
    w.add_context_length(args.ctx)
    w.add_embedding_length(args.dim)
    w.add_block_count(args.layers)
    w.add_feed_forward_length(args.ffn)
    w.add_head_count(args.heads)
    w.add_head_count_kv(kv_heads)
    w.add_layer_norm_rms_eps(1e-5)
    w.add_rope_dimension_count(head_dim)

    w.add_tokenizer_model("llama")
    w.add_tokenizer_pre("default")
    w.add_token_list(pieces)
    w.add_token_scores([0.0] * n_vocab)
    types = ([TokenType.CONTROL] * len(specials) + [TokenType.BYTE] * len(byte_tokens)
             + [TokenType.NORMAL] * (n_vocab - len(specials) - len(byte_tokens)))
    w.add_token_types(types)
    w.add_unk_token_id(0)
    w.add_bos_token_id(1)
    w.add_eos_token_id(2)
    w.add_add_bos_token(True)
    w.add_add_eos_token(False)

    rng = np.random.default_rng(0)

    def t(name, shape):
        # float32 so this measures architecture, not a quantization scheme.
        w.add_tensor(name, (rng.standard_normal(shape) * 0.02).astype(np.float32))

    t("token_embd.weight", (n_vocab, args.dim))
    for i in range(args.layers):
        p = f"blk.{i}."
        t(p + "attn_norm.weight", (args.dim,))
        t(p + "attn_q.weight", (args.dim, args.dim))
        t(p + "attn_k.weight", (kv_heads * head_dim, args.dim))
        t(p + "attn_v.weight", (kv_heads * head_dim, args.dim))
        t(p + "attn_output.weight", (args.dim, args.dim))
        t(p + "ffn_norm.weight", (args.dim,))
        t(p + "ffn_gate.weight", (args.ffn, args.dim))
        t(p + "ffn_up.weight", (args.ffn, args.dim))
        t(p + "ffn_down.weight", (args.dim, args.ffn))
    t("output_norm.weight", (args.dim,))
    t("output.weight", (n_vocab, args.dim))

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()

    params = n_vocab * args.dim * 2 + args.layers * (
        2 * args.dim * args.dim + 2 * kv_heads * head_dim * args.dim + 3 * args.dim * args.ffn)
    print(f"{args.out}: vocab {n_vocab}, {params/1e6:.1f}M params")


if __name__ == "__main__":
    main()
