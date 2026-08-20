"""Trained torch weights -> the GGUF layout llama.cpp already reads here.

Deliberately a rename rather than a conversion. `model.py` stores linear
weights `[out, in]` and rotates adjacent RoPE pairs precisely so this step has
nothing to get wrong -- no transpose, and none of the Q/K permutation that
`convert_hf_to_gguf.py` has to apply to HuggingFace checkpoints.

`--check` is not optional in spirit: a RoPE convention mismatch produces a
model that loads, runs, and returns plausible numbers that are wrong, and
training is the natural thing to blame. Comparing torch's own logits against
llama.cpp's for the same input is what makes that failure loud.
"""
from __future__ import annotations

import argparse
from pathlib import Path

import numpy as np
import torch

from .mkgguf import GGUF_PY  # noqa: F401  (inserts gguf-py on sys.path)
from .model import Config, Model
from .vocab import BYTE_TOKENS, SPECIALS, Vocab, build as build_vocab

from gguf import GGMLQuantizationType, GGUFWriter, TokenType, quants  # noqa: E402


def _state_dict(blob) -> dict:
    """The weights, with torch.compile's wrapper prefix removed if present.

    `torch.compile` returns an OptimizedModule whose state_dict keys are all
    prefixed `_orig_mod.`, so a checkpoint saved from the compiled model does
    not load into the plain one. train.py now saves the unwrapped module, but
    checkpoints written before that still carry the prefix and are perfectly
    good weights -- refusing them would mean retraining for a naming
    difference.
    """
    return {k[len("_orig_mod."):] if k.startswith("_orig_mod.") else k: v
            for k, v in blob["model"].items()}


def write(checkpoint: Path, chars: Path, out: Path, dtype: str = "f16") -> None:
    blob = torch.load(checkpoint, map_location="cpu")
    cfg = Config(**blob["cfg"])
    model = Model(cfg)
    model.load_state_dict(_state_dict(blob))
    model.eval()

    pieces = build_vocab(chars)
    assert len(pieces) == cfg.vocab_size, (len(pieces), cfg.vocab_size)

    w = GGUFWriter(str(out), "llama")
    w.add_context_length(cfg.max_seq)
    w.add_embedding_length(cfg.dim)
    w.add_block_count(cfg.layers)
    w.add_feed_forward_length(cfg.ffn)
    w.add_head_count(cfg.heads)
    w.add_head_count_kv(cfg.kv_heads)
    w.add_layer_norm_rms_eps(cfg.norm_eps)
    w.add_rope_dimension_count(cfg.head_dim)
    w.add_rope_freq_base(cfg.rope_theta)
    w.add_tokenizer_model("llama")
    w.add_tokenizer_pre("default")
    w.add_token_list(pieces)
    w.add_token_scores([0.0] * len(pieces))
    w.add_token_types([TokenType.CONTROL] * len(SPECIALS)
                      + [TokenType.BYTE] * len(BYTE_TOKENS)
                      + [TokenType.NORMAL] * (len(pieces) - len(SPECIALS) - len(BYTE_TOKENS)))
    w.add_unk_token_id(0)
    w.add_bos_token_id(1)
    w.add_eos_token_id(2)
    # SPM prepends U+2581 ("thin space") to the input unless told not to, and
    # U+2581 is not in a character-level vocabulary -- so llama.cpp byte-fell
    # back on it and tokenized 继续修吧 as 7 tokens where torch gave 4. Caught
    # by export.py's agreement check, which is the only place a tokenizer
    # disagreement is visible: the model still loads, still scores, and is
    # still wrong.
    w.add_add_space_prefix(False)
    w.add_add_bos_token(True)
    w.add_add_eos_token(False)

    def put(name, tensor):
        array = tensor.detach().numpy()
        # RMSNorm weights stay F32 whatever the matrices are: ggml multiplies
        # them against F32 activations and aborts on a mixed-type binary op
        # ("unsupported types: dst: f32, src0: f32, src1: f16").
        if array.ndim == 1:
            w.add_tensor(name, array.astype(np.float32))
            return
        if dtype == "q8_0":
            # Scoring is memory-bandwidth bound -- measured p99 13.76 ms in f16
            # against a 10 ms budget -- and Q8_0 halves the weight traffic for
            # a quantization error far below what candidate ranking resolves.
            # Rows must be a multiple of the 32-element block; every tensor
            # here is, but a future shape change would silently not be.
            assert array.shape[-1] % 32 == 0, (name, array.shape)
            # No raw_shape: with a uint8 tensor and a quantized raw_dtype,
            # add_tensor_info treats the shape it is given as a BYTE shape and
            # converts it back (gguf_writer.py:363-364). Passing the logical
            # shape makes it try to read 512 bytes per row as Q8_0 blocks of
            # 34 and fail; letting it use the quantized array's own shape is
            # what the conversion expects.
            w.add_tensor(name, quants.quantize(array.astype(np.float32),
                                               GGMLQuantizationType.Q8_0),
                         raw_dtype=GGMLQuantizationType.Q8_0)
            return
        w.add_tensor(name, array.astype(np.float16 if dtype == "f16" else np.float32))

    put("token_embd.weight", model.embed.weight)
    for i, block in enumerate(model.blocks):
        p = f"blk.{i}."
        put(p + "attn_norm.weight", block.attn_norm.weight)
        put(p + "attn_q.weight", block.wq.weight)
        put(p + "attn_k.weight", block.wk.weight)
        put(p + "attn_v.weight", block.wv.weight)
        put(p + "attn_output.weight", block.wo.weight)
        put(p + "ffn_norm.weight", block.ffn_norm.weight)
        put(p + "ffn_gate.weight", block.gate.weight)
        put(p + "ffn_up.weight", block.up.weight)
        put(p + "ffn_down.weight", block.down.weight)
    put("output_norm.weight", model.norm.weight)
    put("output.weight", model.output.weight)

    w.write_header_to_file()
    w.write_kv_data_to_file()
    w.write_tensors_to_file()
    w.close()
    print(f"{out}: {cfg.parameters()/1e6:.1f}M parameters, vocab {len(pieces)}")


def reference_logprobs(checkpoint: Path, chars: Path, text: str) -> list[float]:
    """torch's own log P(each next token) for `text`, for --check to compare."""
    blob = torch.load(checkpoint, map_location="cpu")
    cfg = Config(**blob["cfg"])
    model = Model(cfg)
    model.load_state_dict(_state_dict(blob))
    model.eval()
    vocab = Vocab(build_vocab(chars))
    ids = vocab.encode(text, bos=True)
    with torch.no_grad():
        logits, _ = model(torch.tensor([ids]))
        logprobs = torch.log_softmax(logits[0].float(), dim=-1)
    return [logprobs[i, ids[i + 1]].item() for i in range(len(ids) - 1)]


def main() -> int:
    ap = argparse.ArgumentParser(prog="rime-train export")
    ap.add_argument("--checkpoint", required=True)
    ap.add_argument("--chars", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--dtype", default="f16", choices=("q8_0", "f16", "f32"))
    ap.add_argument("--check", metavar="TEXT",
                    help="print torch's per-token logprobs for TEXT, to compare "
                         "against llama.cpp's for the exported file")
    args = ap.parse_args()
    write(Path(args.checkpoint), Path(args.chars), Path(args.out), args.dtype)
    if args.check:
        values = reference_logprobs(Path(args.checkpoint), Path(args.chars), args.check)
        print("torch logprobs:", " ".join(f"{v:.4f}" for v in values))
        print(f"total: {sum(values):.4f}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
