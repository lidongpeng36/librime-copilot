#!/usr/bin/env python3
"""What one re-ranking would cost on MLX, in the condition the scorer runs in.

The counterpart to tools/bench_scorer.cc, and deliberately the same experiment:
same model shape, same batch geometry, same --idle-ms placement, same rule
about where the clock stops. Read that file's header first -- everything it
says about the two conditions this measurement has to reproduce applies here
unchanged, and the two timing traps it records were both about ending a phase
before the GPU had finished.

WHY THIS EXISTS. After 2026-09-04 the deployed cost is understood
(docs/superpowers/specs/2026-09-04-scoring-latency-results.md): a
decode-bearing scoring costs ~9 ms after any idle over 50 ms, of which graph
construction is 0.07 ms, KV bookkeeping was 2.3 ms and has been taken, and the
model mutex is 0. The remaining ~7 ms is Metal command encoding, dispatch and
synchronization, stretched by a downclocked core. The ONE mechanism identified
that could attack it is issuing fewer Metal dispatches -- kernel fusion -- which
is what MLX offers. Nothing published measures this: every MLX-vs-llama.cpp
figure is sustained throughput, i.e. the condition where there is no problem.

WHAT THIS IS NOT. Not an implementation, not a port, and not general: it builds
exactly the deployed model's shape and exactly the deployed scoring geometry,
and nothing else. In particular the weights are RANDOM. That is sound for the
question -- latency here is set by dispatch count and tensor shapes, and the
same forward is run either way -- and it is unsound for any other question, so
this prints no logprob that means anything.

SCOPE OF THE COMPARISON, stated because it is easy to overclaim. The C++ path
runs its log-softmax on the HOST over 8573 floats and copies whole logit rows
back; the natural MLX shape does it on the GPU and copies four scalars. That is
a real difference between the two designs, not a thing held constant, and it
flatters MLX. It is also what an MLX implementation would actually do, so it is
the honest thing to measure -- but a difference in the total does not by itself
locate the cause.

Requires `pip install mlx` in a throwaway environment. Deliberately NOT in
RUNTIME_REQUIREMENTS (tools/rime_copilot/install.py): nothing shipped depends
on it, and tools/requirements.txt is generated from that list.

    python3 -m venv ~/.local/share/rime-corpus/mlx-venv
    ~/.local/share/rime-corpus/mlx-venv/bin/pip install mlx
    ~/.local/share/rime-corpus/mlx-venv/bin/python tools/bench_mlx.py --iters 200
"""

import argparse
import json
import math
import resource
import statistics
import sys
import time

try:
    import mlx.core as mx
    import mlx.nn as nn
except ImportError:  # pragma: no cover - the tool is useless without it
    print("mlx is not installed; see this file's docstring", file=sys.stderr)
    raise SystemExit(1)


# rime40m-v2-q8.gguf's own metadata, read with tools/rime_train/mkgguf.py's
# gguf-py. Hard-coded rather than parsed because this tool builds the shape and
# never loads the file -- a parser would imply it does.
N_LAYERS = 10
D_MODEL = 512
N_HEADS = 8
N_KV_HEADS = 8
HEAD_DIM = D_MODEL // N_HEADS  # 64, and llama.rope.dimension_count agrees
FFN_DIM = 1408
VOCAB = 8573
RMS_EPS = 1e-5
ROPE_THETA = 10000.0


class Attention(nn.Module):
    def __init__(self):
        super().__init__()
        self.q_proj = nn.Linear(D_MODEL, N_HEADS * HEAD_DIM, bias=False)
        self.k_proj = nn.Linear(D_MODEL, N_KV_HEADS * HEAD_DIM, bias=False)
        self.v_proj = nn.Linear(D_MODEL, N_KV_HEADS * HEAD_DIM, bias=False)
        self.o_proj = nn.Linear(N_HEADS * HEAD_DIM, D_MODEL, bias=False)
        # traditional=True, i.e. rotate ADJACENT pairs. This is not a style
        # choice: model.py rotates adjacent pairs because llama.cpp's LLAMA
        # architecture is GGML_ROPE_TYPE_NORM, and HuggingFace's halves
        # convention is the other one. Measured against score_candidates on the
        # real weights, adjacent pairs agree to 0.0022 (Q8_0 dequantization
        # rounding) and halves is wrong by up to 3.24 -- while loading, running
        # and returning entirely plausible numbers. tools/rime_train/export.py's
        # --check exists for this exact failure.
        self.rope = nn.RoPE(HEAD_DIM, traditional=True, base=ROPE_THETA)
        self.scale = HEAD_DIM ** -0.5

    def __call__(self, x, cache=None, mask=None):
        b, t, _ = x.shape
        q = self.q_proj(x).reshape(b, t, N_HEADS, HEAD_DIM).transpose(0, 2, 1, 3)
        k = self.k_proj(x).reshape(b, t, N_KV_HEADS, HEAD_DIM).transpose(0, 2, 1, 3)
        v = self.v_proj(x).reshape(b, t, N_KV_HEADS, HEAD_DIM).transpose(0, 2, 1, 3)
        offset = cache[0].shape[2] if cache is not None else 0
        q = self.rope(q, offset=offset)
        k = self.rope(k, offset=offset)
        if cache is not None:
            # The candidate branch. The prefix is shared, so it is BROADCAST to
            # the candidate batch rather than copied per sequence -- which is
            # the whole llama.cpp seq_cp dance (kMaxCandidates `seq_rm` plus
            # `seq_cp` pairs per scoring, both walking cells) collapsing into a
            # shape. That is a structural difference between the two runtimes
            # and is exactly the term that made n_ctx_seq matter over there.
            ck, cv = cache
            if ck.shape[0] != b:
                ck = mx.broadcast_to(ck, (b,) + ck.shape[1:])
                cv = mx.broadcast_to(cv, (b,) + cv.shape[1:])
            k = mx.concatenate([ck, k], axis=2)
            v = mx.concatenate([cv, v], axis=2)
        out = mx.fast.scaled_dot_product_attention(q, k, v, scale=self.scale, mask=mask)
        out = out.transpose(0, 2, 1, 3).reshape(b, t, -1)
        return self.o_proj(out), (k, v)


class Block(nn.Module):
    def __init__(self):
        super().__init__()
        self.attn = Attention()
        self.attn_norm = nn.RMSNorm(D_MODEL, eps=RMS_EPS)
        self.ffn_norm = nn.RMSNorm(D_MODEL, eps=RMS_EPS)
        self.gate_proj = nn.Linear(D_MODEL, FFN_DIM, bias=False)
        self.up_proj = nn.Linear(D_MODEL, FFN_DIM, bias=False)
        self.down_proj = nn.Linear(FFN_DIM, D_MODEL, bias=False)

    def __call__(self, x, cache=None, mask=None):
        h, new_cache = self.attn(self.attn_norm(x), cache=cache, mask=mask)
        x = x + h
        y = self.ffn_norm(x)
        x = x + self.down_proj(nn.silu(self.gate_proj(y)) * self.up_proj(y))
        return x, new_cache


class Model(nn.Module):
    def __init__(self):
        super().__init__()
        self.embed = nn.Embedding(VOCAB, D_MODEL)
        self.blocks = [Block() for _ in range(N_LAYERS)]
        self.norm = nn.RMSNorm(D_MODEL, eps=RMS_EPS)
        self.out = nn.Linear(D_MODEL, VOCAB, bias=False)

    def __call__(self, tokens, cache=None, mask=None):
        x = self.embed(tokens)
        new_cache = []
        for i, block in enumerate(self.blocks):
            x, c = block(x, cache=cache[i] if cache is not None else None, mask=mask)
            new_cache.append(c)
        return self.out(self.norm(x)), new_cache


def now_ms():
    return time.perf_counter() * 1000.0


def cpu_ms():
    r = resource.getrusage(resource.RUSAGE_SELF)
    return (r.ru_utime + r.ru_stime) * 1000.0


def pct(sorted_values, q):
    if not sorted_values:
        return 0.0
    i = min(int(len(sorted_values) * q), len(sorted_values) - 1)
    return sorted_values[i]


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iters", type=int, default=200)
    ap.add_argument("--context-chars", type=int, default=64,
                    help="context tokens; one token per Han character for this vocab")
    ap.add_argument("--candidates", type=int, default=4, help="copilot/rerank/llm/top_n")
    ap.add_argument("--decode-tokens", type=int, default=1,
                    help="tokens submitted PER candidate, i.e. len-1. 0 reproduces the "
                         "decode-free mode, which costs ~0.18 ms in the C++ path")
    ap.add_argument("--idle-ms", type=int, default=0,
                    help="idle inserted BETWEEN the prefill and the score -- see "
                         "tools/bench_scorer.cc's header for why it goes there")
    ap.add_argument("--quantize", choices=("none", "8", "4"), default="8",
                    help="8 matches the deployed Q8_0 (default)")
    ap.add_argument("--weights",
                    help="an .npz of the dequantized gguf tensors. Without it the "
                         "weights are RANDOM, which is sound for latency (same shapes, "
                         "same forward) and sound for nothing else.")
    ap.add_argument("--compile", action="store_true",
                    help="wrap the score step in mx.compile. This is the mechanism the "
                         "whole prototype exists to price: fusion reduces the number of "
                         "Metal dispatches, and dispatch overhead is what the C++ path's "
                         "residual ~7 ms was attributed to")
    ap.add_argument("--warmup", type=int, default=10,
                    help="untimed iterations first (default %(default)s). MLX compiles "
                         "its Metal pipelines on first use, which costs hundreds of "
                         "milliseconds once -- measured, prefill p99 was 621 ms over 30 "
                         "iterations without this and 2.8 ms with it. That is a startup "
                         "cost the deployed scorer would pay once per process, not per "
                         "scoring, so it does not belong in these percentiles; it is "
                         "printed separately instead.")
    ap.add_argument("--compile-prefill", action="store_true",
                    help="also wrap the prefill in mx.compile -- see prefill_step")
    ap.add_argument("--json", action="store_true")
    args = ap.parse_args(argv)

    mx.random.seed(0)
    model = Model()
    if args.weights:
        import numpy as np
        raw = np.load(args.weights)
        # gguf name -> this module's parameter tree. Written out rather than
        # derived from a regex so a tensor this model does not consume is a
        # KeyError here instead of a silently untrained layer.
        p = {"embed": {"weight": mx.array(raw["token_embd.weight"])},
             "norm": {"weight": mx.array(raw["output_norm.weight"])},
             "out": {"weight": mx.array(raw["output.weight"])},
             "blocks": []}
        for i in range(N_LAYERS):
            g = f"blk.{i}."
            p["blocks"].append({
                "attn": {"q_proj": {"weight": mx.array(raw[g + "attn_q.weight"])},
                         "k_proj": {"weight": mx.array(raw[g + "attn_k.weight"])},
                         "v_proj": {"weight": mx.array(raw[g + "attn_v.weight"])},
                         "o_proj": {"weight": mx.array(raw[g + "attn_output.weight"])}},
                "attn_norm": {"weight": mx.array(raw[g + "attn_norm.weight"])},
                "ffn_norm": {"weight": mx.array(raw[g + "ffn_norm.weight"])},
                "gate_proj": {"weight": mx.array(raw[g + "ffn_gate.weight"])},
                "up_proj": {"weight": mx.array(raw[g + "ffn_up.weight"])},
                "down_proj": {"weight": mx.array(raw[g + "ffn_down.weight"])}})
        model.update(p)
    if args.quantize != "none":
        nn.quantize(model, group_size=64, bits=int(args.quantize))
    mx.eval(model.parameters())

    ctx_tokens = mx.array([[i % VOCAB for i in range(args.context_chars)]])
    cand_tokens = mx.array([[(i * 7 + 3) % VOCAB] for i in range(args.candidates)])
    targets = mx.array([(i * 11 + 5) % VOCAB for i in range(args.candidates)])

    def score_step(cache, ctx_last_logits):
        """One ScoreGroup: every candidate's first token is free off the
        context's own last row, the rest go in ONE forward."""
        ctx_lse = mx.logsumexp(ctx_last_logits, axis=-1)
        first = ctx_last_logits[cand_tokens[:, 0]] - ctx_lse
        if args.decode_tokens == 0:
            return first
        logits, _ = model(cand_tokens, cache=cache)
        lse = mx.logsumexp(logits[:, -1, :], axis=-1)
        rest = mx.take_along_axis(logits[:, -1, :], targets[:, None], axis=-1)[:, 0] - lse
        return first + rest

    def prefill_step(tokens):
        return model(tokens, mask=causal)

    scorer = mx.compile(score_step) if args.compile else score_step
    # Compiled separately and on its own flag. The prefill runs on a background
    # worker and never blocks a keystroke, so its latency is not the number
    # under study -- but ~100 Python-level MLX calls per prefill DO land in the
    # process CPU time this tool reports, and that number is the energy
    # argument. Separating the two is what says whether MLX's CPU disadvantage
    # is MLX or is this prototype being written in Python.
    prefiller = mx.compile(prefill_step) if args.compile_prefill else prefill_step

    prefill_ms, score_ms = [], []
    slept = 0.0
    causal = nn.MultiHeadAttention.create_additive_causal_mask(args.context_chars)
    # Untimed, and measured on its own: the first forward compiles pipelines.
    first_call_ms = None
    for w in range(args.warmup):
        t = now_ms()
        logits, cache = prefiller(ctx_tokens)
        ctx_last = logits[0, -1, :]
        mx.eval(cache)
        _ = ctx_last[0].item()
        _ = float(scorer(cache, ctx_last).sum().item())
        if w == 0:
            first_call_ms = now_ms() - t

    cpu0, wall0 = cpu_ms(), now_ms()
    checksum = 0.0
    for _ in range(args.iters):
        t0 = now_ms()
        logits, cache = prefiller(ctx_tokens)
        ctx_last = logits[0, -1, :]
        # MLX is lazy, so the phase must end on a real host read or the work
        # lands in whatever is timed next -- the same trap tools/bench_scorer.cc
        # got wrong twice on Metal, in both directions. `.item()` both forces
        # evaluation and copies to the host.
        mx.eval(cache)
        _ = ctx_last[0].item()
        t1 = now_ms()

        if args.idle_ms > 0:
            s = now_ms()
            time.sleep(args.idle_ms / 1000.0)
            slept += now_ms() - s
        t2 = now_ms()

        scores = scorer(cache, ctx_last)
        checksum += float(scores.sum().item())  # forces and reads to host
        t3 = now_ms()

        prefill_ms.append(t1 - t0)
        score_ms.append(t3 - t2)
    wall = (now_ms() - wall0) - slept
    cpu = cpu_ms() - cpu0

    prefill_ms.sort()
    score_ms.sort()
    result = {
        "framework": "mlx",
        "iters": args.iters,
        "context_chars": args.context_chars,
        "candidates": args.candidates,
        "decode_tokens_per_candidate": args.decode_tokens,
        "decoded_per_iter": float(args.candidates * args.decode_tokens),
        "idle_ms": args.idle_ms,
        "quantize": args.quantize,
        "compiled": bool(args.compile),
        "compiled_prefill": bool(args.compile_prefill),
        "score_p50_ms": pct(score_ms, 0.50),
        "score_p99_ms": pct(score_ms, 0.99),
        "prefill_p50_ms": pct(prefill_ms, 0.50),
        "prefill_p99_ms": pct(prefill_ms, 0.99),
        "cpu_ms_per_iter": cpu / args.iters,
        "cpu_per_wall": cpu / wall if wall else 0.0,
        "real_weights": bool(args.weights),
        "first_call_ms": first_call_ms,
        "warmup": args.warmup,
    }
    if args.json:
        print(json.dumps(result))
        return 0
    print("mlx %s  iters %d  context %d  candidates %d x %d token(s)  idle %d ms  q%s%s"
          % (mx.default_device(), args.iters, args.context_chars, args.candidates,
             args.decode_tokens, args.idle_ms, args.quantize,
             "  compiled" if args.compile else ""))
    print("tokens decoded/iter: %.2f  (%s)"
          % (result["decoded_per_iter"],
             "the decode-bearing mode" if args.decode_tokens else "the decode-free mode"))
    print("score   (the p99<10ms budget): p50 %.2f ms  p99 %.2f ms"
          % (result["score_p50_ms"], result["score_p99_ms"]))
    print("prefill (background warm-up):  p50 %.2f ms  p99 %.2f ms"
          % (result["prefill_p50_ms"], result["prefill_p99_ms"]))
    print("cpu time: %.2f ms per scoring  (cpu/busy-wall %.2f)"
          % (result["cpu_ms_per_iter"], result["cpu_per_wall"]))
    if first_call_ms is not None:
        print("first call (pipeline compile, once per process): %.0f ms" % first_call_ms)
    print("checksum %.4f  (%s)" % (
        checksum / args.iters,
        "real weights, but the tokens are synthetic -- proof the arithmetic ran, "
        "not a logprob" if args.weights
        else "RANDOM weights -- proof the arithmetic ran, and nothing else"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
