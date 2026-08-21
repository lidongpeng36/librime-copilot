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

from . import recipe
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
    """Random training windows, skipping every held-out block.

    Rejection rather than an index remap: the held-out blocks are 0.1% of the
    corpus, so a redraw costs nothing measurable and the alternative (compacting
    the index space) would silently shift what every offset means.

    The stripe geometry is read straight from recipe, NOT taken as a parameter:
    this function decides what the model may train on and validation_loss below
    decides what it is scored on, from the same two constants. They were once a
    parameter here and a hard-coded `recipe.DEFAULT_*` there, which is a pair
    that can disagree -- and a disagreement means the validation set is data the
    model trained on, with nothing in the loss curve to show it.
    """
    block, period = recipe.DEFAULT_BLOCK, recipe.DEFAULT_PERIOD
    rng = np.random.default_rng(seed)
    n = len(tokens) - seq - 1
    while True:
        idx = []
        while len(idx) < batch:
            candidate = int(rng.integers(0, n))
            if recipe.overlaps_validation(candidate, recipe.training_footprint(seq), block, period):
                continue
            idx.append(candidate)
        x = np.stack([tokens[i:i + seq] for i in idx]).astype(np.int64)
        y = np.stack([tokens[i + 1:i + 1 + seq] for i in idx]).astype(np.int64)
        yield (torch.from_numpy(x).to(device, non_blocking=True),
               torch.from_numpy(y).to(device, non_blocking=True))


@torch.no_grad()
def validation_loss(model, tokens: np.ndarray, seq: int, device,
                    n_windows: int = 64) -> float:
    """Mean loss over the held-out blocks, on a fixed set of windows.

    Fixed, not sampled: a validation loss that moves because its own sample
    moved cannot rank two recipes, which is the only reason it is here.

    Same two constants `batches` rejects against, for the reason given there.
    """
    starts = recipe.validation_starts(len(tokens), recipe.DEFAULT_BLOCK,
                                      recipe.DEFAULT_PERIOD, seq, n_windows)
    if not starts:
        return float("nan")
    model.eval()
    try:
        total = 0.0
        for start in starts:
            x = torch.from_numpy(tokens[start:start + seq].astype(np.int64))[None].to(device)
            y = torch.from_numpy(tokens[start + 1:start + 1 + seq].astype(np.int64))[None].to(device)
            with torch.autocast(device_type=device, dtype=torch.bfloat16):
                _, loss = model(x, y)
            total += loss.item()
    finally:
        model.train()
    return total / len(starts)


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
    ap.add_argument("--lr", type=float, default=6e-4,
                    help="3e-4 was the first run's value and is conservative for "
                         "40.9M parameters at 49152 tokens per step")
    ap.add_argument("--warmup", type=int, default=500)
    ap.add_argument("--compile", action="store_true",
                    help="torch.compile the model; small models are launch-bound "
                         "and this is where most of the MFU is")
    ap.add_argument("--log-every", type=int, default=50)
    ap.add_argument("--legacy-recipe", action="store_true",
                    help="reproduce the recipe the shipped model was trained with: "
                         "weight decay on EVERY parameter including RMSNorm gains, "
                         "and a flat std=0.02 init. The held-out sampler is NOT "
                         "reverted -- a control arm whose validation loss is "
                         "contaminated cannot be compared with anything, which is "
                         "the entire reason for running one")
    ap.add_argument("--val-every", type=int, default=1000,
                    help="steps between held-out validation loss reports")
    ap.add_argument("--save-every", type=int, default=2000)
    args = ap.parse_args()

    device = "cuda" if torch.cuda.is_available() else "cpu"
    vocab = Vocab(build_vocab(Path(args.chars)))
    cfg = Config(vocab_size=len(vocab), dim=args.dim, layers=args.layers,
                 ffn=args.ffn, heads=args.heads, kv_heads=args.heads,
                 max_seq=args.seq, depth_scaled_init=not args.legacy_recipe)
    model = Model(cfg).to(device)
    print(f"{cfg.parameters()/1e6:.1f}M parameters, vocab {len(vocab)}, device {device}",
          flush=True)

    tokens = np.memmap(args.tokens, dtype=np.uint16, mode="r")
    print(f"{len(tokens)/1e9:.2f}B tokens, "
          f"{args.steps * args.batch * args.accum * args.seq / 1e9:.2f}B will be seen",
          flush=True)

    if args.compile:
        model = torch.compile(model)

    # Weight decay on projections only. It used to hit every parameter,
    # including RMSNorm gains -- decaying a norm's gain pulls it toward zero,
    # which the norm then fights with its own scale. recipe.decays_weight is
    # the rule, tested without torch.
    decay, no_decay = [], []
    for name, param in getattr(model, "_orig_mod", model).named_parameters():
        if not param.requires_grad:
            continue
        # --legacy-recipe puts everything in the decayed group, which is what
        # `AdamW(model.parameters(), weight_decay=0.1)` did before 2026-08-21.
        # The printed counts are the check: 93/0 legacy against 72/21 now.
        decayed = True if args.legacy_recipe else recipe.decays_weight(name, param.dim())
        (decay if decayed else no_decay).append(param)
    print(f"weight decay on {len(decay)} tensors, off on {len(no_decay)}", flush=True)
    opt = torch.optim.AdamW(
        [{"params": decay, "weight_decay": 0.1},
         {"params": no_decay, "weight_decay": 0.0}],
        lr=args.lr, betas=(0.9, 0.95))
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
        if step % args.val_every == 0 or step == args.steps:
            print(f"  val {validation_loss(getattr(model, '_orig_mod', model), tokens, args.seq, device):.4f}",
                  flush=True)
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
