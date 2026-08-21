"""A minimal Llama, laid out to match the GGUF this project writes.

Not a general modelling library: it exists so that a checkpoint trained here
loads into llama.cpp unchanged, which is what lets `score_candidates`, the
plugin's `LlmScorer`, and every measurement already built keep working without
a line of new inference code.

Two conventions are load-bearing and silent when wrong:

  * Linear weights are `[out, in]`, the same order GGUF stores them, so export
    is a rename rather than a transpose.
  * RoPE rotates ADJACENT pairs (x0,x1), (x2,x3), ... because llama.cpp's LLAMA
    architecture uses GGML_ROPE_TYPE_NORM. HuggingFace's Llama rotates halves
    instead, which is exactly why `convert_hf_to_gguf.py` permutes Q and K on
    the way out. Writing GGUF directly means matching NORM here, or the export
    produces a model that runs, produces plausible-looking numbers, and is
    wrong -- with training the natural thing to blame.

`export.py`'s agreement check is what keeps that second point honest.
"""
from __future__ import annotations

import math
from dataclasses import dataclass

import torch
import torch.nn as nn
import torch.nn.functional as F

from . import recipe


@dataclass
class Config:
    vocab_size: int
    dim: int = 512
    layers: int = 12
    heads: int = 8
    kv_heads: int = 8
    ffn: int = 1408
    max_seq: int = 512
    rope_theta: float = 10000.0
    norm_eps: float = 1e-5

    @property
    def head_dim(self) -> int:
        return self.dim // self.heads

    def parameters(self) -> int:
        per_layer = (2 * self.dim * self.dim
                     + 2 * self.kv_heads * self.head_dim * self.dim
                     + 3 * self.dim * self.ffn)
        return self.vocab_size * self.dim * 2 + self.layers * per_layer


class RMSNorm(nn.Module):
    def __init__(self, dim: int, eps: float):
        super().__init__()
        self.weight = nn.Parameter(torch.ones(dim))
        self.eps = eps

    def forward(self, x):
        norm = x.float().pow(2).mean(-1, keepdim=True).add(self.eps).rsqrt()
        return (x.float() * norm).type_as(x) * self.weight


def rope_tables(head_dim: int, max_seq: int, theta: float, device=None):
    freqs = 1.0 / (theta ** (torch.arange(0, head_dim, 2, device=device).float() / head_dim))
    t = torch.arange(max_seq, device=device).float()
    angles = torch.outer(t, freqs)
    return angles.cos(), angles.sin()


def apply_rope(x, cos, sin):
    """NORM-style: rotate adjacent pairs. See the module docstring."""
    even, odd = x[..., 0::2], x[..., 1::2]
    cos = cos[None, :, None, :]
    sin = sin[None, :, None, :]
    out = torch.stack((even * cos - odd * sin, even * sin + odd * cos), dim=-1)
    return out.flatten(-2)


class Block(nn.Module):
    def __init__(self, cfg: Config):
        super().__init__()
        self.cfg = cfg
        d, hd = cfg.dim, cfg.head_dim
        self.attn_norm = RMSNorm(d, cfg.norm_eps)
        self.wq = nn.Linear(d, cfg.heads * hd, bias=False)
        self.wk = nn.Linear(d, cfg.kv_heads * hd, bias=False)
        self.wv = nn.Linear(d, cfg.kv_heads * hd, bias=False)
        self.wo = nn.Linear(cfg.heads * hd, d, bias=False)
        self.ffn_norm = RMSNorm(d, cfg.norm_eps)
        self.gate = nn.Linear(d, cfg.ffn, bias=False)
        self.up = nn.Linear(d, cfg.ffn, bias=False)
        self.down = nn.Linear(cfg.ffn, d, bias=False)

    def forward(self, x, cos, sin):
        cfg = self.cfg
        b, t, _ = x.shape
        h = self.attn_norm(x)
        q = self.wq(h).view(b, t, cfg.heads, cfg.head_dim)
        k = self.wk(h).view(b, t, cfg.kv_heads, cfg.head_dim)
        v = self.wv(h).view(b, t, cfg.kv_heads, cfg.head_dim)
        q, k = apply_rope(q, cos, sin), apply_rope(k, cos, sin)
        if cfg.kv_heads != cfg.heads:
            repeat = cfg.heads // cfg.kv_heads
            k = k.repeat_interleave(repeat, dim=2)
            v = v.repeat_interleave(repeat, dim=2)
        q, k, v = (z.transpose(1, 2) for z in (q, k, v))
        a = F.scaled_dot_product_attention(q, k, v, is_causal=True)
        x = x + self.wo(a.transpose(1, 2).reshape(b, t, -1))
        h = self.ffn_norm(x)
        return x + self.down(F.silu(self.gate(h)) * self.up(h))


class Model(nn.Module):
    def __init__(self, cfg: Config):
        super().__init__()
        self.cfg = cfg
        self.embed = nn.Embedding(cfg.vocab_size, cfg.dim)
        self.blocks = nn.ModuleList(Block(cfg) for _ in range(cfg.layers))
        self.norm = RMSNorm(cfg.dim, cfg.norm_eps)
        self.output = nn.Linear(cfg.dim, cfg.vocab_size, bias=False)
        cos, sin = rope_tables(cfg.head_dim, cfg.max_seq, cfg.rope_theta)
        self.register_buffer("cos", cos, persistent=False)
        self.register_buffer("sin", sin, persistent=False)
        self.apply(self._init)
        # The two projections that write into the residual stream once per
        # layer, so their variance compounds with depth. Applied after the flat
        # init above, which has already touched them.
        std = recipe.residual_init_std(0.02, cfg.layers)
        for block in self.blocks:
            nn.init.normal_(block.wo.weight, mean=0.0, std=std)
            nn.init.normal_(block.down.weight, mean=0.0, std=std)

    @staticmethod
    def _init(module):
        if isinstance(module, (nn.Linear, nn.Embedding)):
            nn.init.normal_(module.weight, mean=0.0, std=0.02)

    def forward(self, idx, targets=None):
        t = idx.shape[1]
        cos, sin = self.cos[:t], self.sin[:t]
        x = self.embed(idx)
        for block in self.blocks:
            x = block(x, cos, sin)
        logits = self.output(self.norm(x))
        if targets is None:
            return logits, None
        loss = F.cross_entropy(logits.view(-1, logits.size(-1)), targets.reshape(-1))
        return logits, loss
