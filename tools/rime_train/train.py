"""Train the scorer. Run on a CUDA host; nothing else here imports torch.

The model is a plain causal LM over characters, because that is exactly the
quantity the task needs: `score_candidates` ranks whole candidate sentences by
log P(candidate | context), so a next-token objective on ordinary text IS the
training objective, with no task-specific head or fine-tuning stage to get
wrong.

Data is memory-mapped token ids rather than text: at a few billion tokens the
corpus does not fit in RAM, and re-tokenizing per epoch would cost more than
the forward pass.
"""
from __future__ import annotations

import argparse
import math
import time
from pathlib import Path

import numpy as np
import torch

from .model import Config, Model
from .vocab import Vocab, build as build_vocab


def tokenize_corpus(corpus: Path, vocab: Vocab, out: Path, limit: int | None = None) -> int:
    """One-time text -> uint16 token ids on disk.

    uint16 holds the whole vocabulary with room to spare (8536 of 65536) and
    halves the bytes a training step has to read; at billions of tokens that
    is the difference between the loader keeping the GPU fed and not.
    """
    assert len(vocab) < 65536, "vocabulary no longer fits uint16"
    total = 0
    with open(corpus, encoding="utf-8") as handle, open(out, "wb") as sink:
        chunk: list[int] = []
        for line in handle:
            line = line.strip()
            if not line:
                continue
            # Every sentence ends with EOS: the corpus is Han runs, and without
            # a boundary token the model would learn to run one sentence into
            # the next -- which is exactly the collocation-across-boundary
            # error the n-gram pipeline had to avoid too.
            chunk.extend(vocab.encode(line))
            chunk.append(2)
            if len(chunk) >= 1 << 20:
                np.asarray(chunk, dtype=np.uint16).tofile(sink)
                total += len(chunk)
                chunk = []
                if limit and total >= limit:
                    return total
        if chunk:
            np.asarray(chunk, dtype=np.uint16).tofile(sink)
            total += len(chunk)
    return total


def batches(tokens: np.ndarray, batch: int, seq: int, device, seed: int = 0):
    rng = np.random.default_rng(seed)
    n = len(tokens) - seq - 1
    while True:
        idx = rng.integers(0, n, size=batch)
        x = np.stack([tokens[i:i + seq] for i in idx]).astype(np.int64)
        y = np.stack([tokens[i + 1:i + 1 + seq] for i in idx]).astype(np.int64)
        yield (torch.from_numpy(x).to(device, non_blocking=True),
               torch.from_numpy(y).to(device, non_blocking=True))


def main() -> int:
    ap = argparse.ArgumentParser(prog="rime-train train")
    ap.add_argument("--tokens", required=True, help="uint16 token file from --tokenize")
    ap.add_argument("--chars", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--layers", type=int, default=10)
    ap.add_argument("--dim", type=int, default=512)
    ap.add_argument("--ffn", type=int, default=1408)
    ap.add_argument("--heads", type=int, default=8)
    ap.add_argument("--seq", type=int, default=256)
    ap.add_argument("--batch", type=int, default=48)
    ap.add_argument("--accum", type=int, default=4)
    ap.add_argument("--steps", type=int, default=20000)
    ap.add_argument("--lr", type=float, default=3e-4)
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--compile", action="store_true",
                    help="torch.compile the model; small models are launch-bound "
                         "and this is where most of the MFU is")
    ap.add_argument("--log-every", type=int, default=50)
    ap.add_argument("--save-every", type=int, default=2000)
    args = ap.parse_args()

    device = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = Vocab(build_vocab(Path(args.chars)))
    cfg = Config(vocab_size=len(vocab), dim=args.dim, layers=args.layers,
                 ffn=args.ffn, heads=args.heads, kv_heads=args.heads,
                 max_seq=args.seq)
    model = Model(cfg).to(device)
    print(f"{cfg.parameters()/1e6:.1f}M parameters, vocab {len(vocab)}, device {device}",
          flush=True)

    tokens = np.memmap(args.tokens, dtype=np.uint16, mode="r")
    print(f"{len(tokens)/1e9:.2f}B tokens, "
          f"{args.steps * args.batch * args.accum * args.seq / 1e9:.2f}B will be seen",
          flush=True)

    if args.compile:
        model = torch.compile(model)

    opt = torch.optim.AdamW(model.parameters(), lr=args.lr, betas=(0.9, 0.95),
                            weight_decay=0.1)
    # No GradScaler: it exists for fp16's narrow exponent range, and bf16 has
    # fp32's. Keeping it would add an unscale pass and a host sync per step for
    # nothing -- and on a model this small the step is short enough that the
    # sync is a measurable share of it.
    stream = batches(tokens, args.batch, args.seq, device)
    start = time.time()
    for step in range(1, args.steps + 1):
        # Linear warmup then cosine decay: the standard schedule, named here
        # only because a flat LR on a from-scratch run this short is the
        # difference between converging and not.
        if step <= args.warmup:
            lr = args.lr * step / args.warmup
        else:
            progress = (step - args.warmup) / max(1, args.steps - args.warmup)
            lr = 0.1 * args.lr + 0.9 * args.lr * 0.5 * (1 + math.cos(math.pi * progress))
        for group in opt.param_groups:
            group["lr"] = lr

        opt.zero_grad(set_to_none=True)
        total = 0.0
        for _ in range(args.accum):
            x, y = next(stream)
            with torch.autocast(device_type=device, dtype=torch.bfloat16):
                _, loss = model(x, y)
            (loss / args.accum).backward()
            total += loss.item() / args.accum
        torch.nn.utils.clip_grad_norm_(model.parameters(), 1.0)
        opt.step()

        if step % args.log_every == 0:
            seen = step * args.batch * args.accum * args.seq
            elapsed = time.time() - start
            print(f"step {step:6}/{args.steps}  loss {total:.4f}  lr {lr:.2e}  "
                  f"{seen/1e6:.0f}M tokens  {seen/elapsed/1e3:.0f}k tok/s", flush=True)
        if step % args.save_every == 0 or step == args.steps:
            # Unwrap torch.compile: an OptimizedModule's state_dict prefixes
            # every key with `_orig_mod.`, which then will not load into the
            # plain Model the exporter builds.
            weights = getattr(model, "_orig_mod", model).state_dict()
            torch.save({"model": weights, "cfg": cfg.__dict__}, args.out)
            print(f"  saved {args.out}", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
