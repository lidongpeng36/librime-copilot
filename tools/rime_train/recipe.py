"""The training recipe's decisions, separated from the tensors that apply them.

torch is not installed on the dev laptop and is not in CI: tools/test runs under
`unittest discover` on a stock interpreter, and rime_train_test.py imports only
the torch-free modules for that reason. Anything decidable without a tensor
lives here so it can be tested at all; train.py and model.py wire it up. Same
move the C++ side made for ComputeSpaceCommitText and BailOnEmptyDbContext.
"""
from __future__ import annotations

# Held-out tokens: `block` out of every `period`. 4096 of every 4096000 is 0.1%
# of the corpus -- about 4.5M tokens at this scale, plenty for a stable
# validation loss and small enough that rejecting the windows that touch it
# costs the training sampler nothing measurable.
DEFAULT_BLOCK = 4096
DEFAULT_PERIOD = 4_096_000


def overlaps_validation(start: int, length: int, block: int, period: int) -> bool:
    """True when [start, start+length) touches any held-out block.

    Blocks are [k*period, k*period + block) for every k >= 0 -- striped through
    the corpus rather than taken as a contiguous slice. The corpus is written
    source by source (LCCC first, then SkyPile), so a tail would be pure SkyPile
    and a head pure Weibo chat; neither measures what was trained on.
    """
    offset = start % period
    if offset < block:
        return True
    return offset + length > period


def validation_starts(n_tokens: int, block: int, period: int, seq: int,
                      limit: int) -> list[int]:
    """Deterministic window starts inside the held-out blocks.

    Deterministic on purpose: a validation loss that moves because its own
    sample moved cannot rank two recipes, which is the whole reason this exists.

    `block` is not decoration. Each window is anchored at a block's start and
    runs `seq + 1` tokens (see training_footprint), so with `seq + 1 > block`
    the tail of every "held-out" window sits past the block, in tokens the
    sampler was free to train on -- and the validation loss would quietly
    include data the model has seen, which is the one thing it exists to
    exclude. Nothing else enforces the relation, and at the shipped constants
    (block 4096, seq 256) it holds with room to spare, so a future smaller
    block would break it silently. Hence a hard failure rather than a clamp:
    there is no correct window to return.
    """
    footprint = training_footprint(seq)
    if footprint > block:
        raise ValueError(
            f"validation window of {footprint} tokens (seq {seq} + 1 label) does not fit "
            f"in a held-out block of {block}; it would extend past the block into "
            f"trained tokens"
        )
    starts: list[int] = []
    k = 0
    while len(starts) < limit:
        start = k * period
        if start + seq + 1 > n_tokens:
            break
        starts.append(start)
        k += 1
    return starts


def decays_weight(name: str, ndim: int) -> bool:
    """Whether AdamW's weight decay should apply to this parameter.

    Excludes 1-D parameters -- RMSNorm gains here -- and nothing else. Decaying
    a norm's gain pulls it toward zero, which the norm then fights with its own
    scale; it is a different operation from decaying a projection. `name` is
    taken for the error messages and for future rules, not used by this one.
    """
    del name
    return ndim >= 2


def residual_init_std(base_std: float, layers: int) -> float:
    """Init std for the projections that write into the residual stream.

    `wo` and `down` each add to the residual once per layer, so their variance
    compounds with depth; the standard correction is 1/sqrt(2L). Every Linear
    was initialised at a flat 0.02.
    """
    if layers < 1:
        raise ValueError(f"layers must be >= 1, got {layers}")
    return base_std / ((2 * layers) ** 0.5)


def training_footprint(seq: int) -> int:
    """Tokens a training window of `seq` inputs actually touches.

    seq + 1, not seq: the window at index i feeds x = tokens[i:i+seq] and
    supervises against y = tokens[i+1:i+1+seq], so its last LABEL is
    tokens[i+seq]. Guarding only the x-span lets that label sit at offset 0 of
    the next period -- inside a held-out block -- and be trained on. The leak
    is a couple of tokens per run and would never show up in a loss curve,
    which is why it is worth a named function rather than a bare `+ 1`.
    """
    if seq < 1:
        raise ValueError(f"seq must be >= 1, got {seq}")
    return seq + 1
