"""The training recipe's pure decisions, so they can be tested at all.

torch is not installed on the dev laptop and is not in CI -- tools/test runs
under `unittest discover` on a stock interpreter, and rime_train_test.py imports
only the torch-free modules for exactly this reason. Every judgement that can be
made without a tensor lives here; train.py and model.py only wire it up. Same
move as ComputeSpaceCommitText and BailOnEmptyDbContext on the C++ side.
"""
from __future__ import annotations

import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from rime_train import recipe


class ValidationBlocksTest(unittest.TestCase):
    """Held-out tokens are every `block` out of every `period`, NOT a
    contiguous tail. The corpus is written source by source -- LCCC first, then
    SkyPile -- so a tail slice would be pure SkyPile and a head slice pure
    Weibo chat. Neither measures the corpus that was trained on.
    """

    def test_a_window_starting_inside_a_block_overlaps(self):
        self.assertTrue(recipe.overlaps_validation(0, 256, block=4096, period=4096000))

    def test_a_window_clear_of_every_block_does_not(self):
        self.assertFalse(recipe.overlaps_validation(100000, 256, block=4096, period=4096000))

    def test_a_window_running_into_the_next_block_overlaps(self):
        """The last 10 tokens of this window fall in the next period's block."""
        self.assertTrue(recipe.overlaps_validation(4096000 - 10, 256,
                                                   block=4096, period=4096000))

    def test_a_window_ending_exactly_at_a_block_does_not_overlap(self):
        self.assertFalse(recipe.overlaps_validation(4096000 - 256, 256,
                                                    block=4096, period=4096000))

    def test_validation_starts_land_inside_blocks(self):
        starts = recipe.validation_starts(n_tokens=40_960_000, block=4096,
                                          period=4_096_000, seq=256, limit=8)
        self.assertEqual(len(starts), 8)
        for s in starts:
            self.assertTrue(recipe.overlaps_validation(s, 256, 4096, 4_096_000))

    def test_validation_starts_are_deterministic(self):
        a = recipe.validation_starts(40_960_000, 4096, 4_096_000, 256, 8)
        b = recipe.validation_starts(40_960_000, 4096, 4_096_000, 256, 8)
        self.assertEqual(a, b)

    def test_validation_starts_refuses_a_window_wider_than_its_block(self):
        """The leak this closes: every window is anchored at a block start and
        runs seq + 1 tokens, so with seq + 1 > block its tail lies past the
        block, in tokens `batches` was free to draw. The "held-out" loss would
        then include data the model trained on, and nothing in the curve would
        say so. Both numbers must appear in the message -- the relation is what
        is wrong, not either value alone.
        """
        with self.assertRaises(ValueError) as caught:
            recipe.validation_starts(n_tokens=40_960_000, block=256,
                                     period=4_096_000, seq=256, limit=8)
        message = str(caught.exception)
        self.assertIn("256", message)
        self.assertIn("257", message)

    def test_validation_starts_accepts_a_window_exactly_filling_its_block(self):
        """seq + 1 == block is the boundary and is fine: the last label token
        is the block's last token, still held out.
        """
        starts = recipe.validation_starts(n_tokens=40_960_000, block=257,
                                          period=4_096_000, seq=256, limit=4)
        self.assertEqual(len(starts), 4)

    def test_validation_starts_never_run_past_the_token_file(self):
        starts = recipe.validation_starts(n_tokens=10_000, block=4096,
                                          period=4_096_000, seq=256, limit=8)
        for s in starts:
            self.assertLessEqual(s + 256 + 1, 10_000)


class WeightDecayGroupTest(unittest.TestCase):
    """AdamW's weight_decay=0.1 was applied to every parameter, including
    RMSNorm gains and embeddings. Decaying a norm's gain is a different
    operation from decaying a projection: it pulls the gain toward zero, which
    the norm then has to fight with its own scale.
    """

    def test_a_projection_matrix_decays(self):
        self.assertTrue(recipe.decays_weight("blocks.0.wq.weight", ndim=2))

    def test_a_norm_gain_does_not(self):
        self.assertFalse(recipe.decays_weight("blocks.0.attn_norm.weight", ndim=1))

    def test_the_final_norm_does_not(self):
        self.assertFalse(recipe.decays_weight("norm.weight", ndim=1))

    def test_the_embedding_table_decays(self):
        """2-D, and it is a real projection into the residual stream. Excluded
        only 1-D parameters, which is the standard rule."""
        self.assertTrue(recipe.decays_weight("embed.weight", ndim=2))


class TrainingFootprintTest(unittest.TestCase):
    """A training window of `seq` inputs actually touches seq + 1 tokens: x is
    tokens[i:i+seq], y is tokens[i+1:i+1+seq], so the last LABEL is
    tokens[i+seq]. Guarding the held-out check with `seq` instead of `seq + 1`
    lets that label land at offset 0 of the next period -- inside a held-out
    block -- and be trained on.
    """

    def test_footprint_is_seq_plus_one(self):
        self.assertEqual(recipe.training_footprint(256), 257)

    def test_seq_below_one_is_rejected(self):
        with self.assertRaises(ValueError):
            recipe.training_footprint(0)

    def test_a_window_ending_exactly_at_a_block_overlaps_once_the_label_is_covered(self):
        """The regression this guards: overlaps_validation(period - seq, seq, ...)
        says False -- the x-span alone clears the block -- but the window's
        last label, tokens[start + seq], is offset 0 of the next block. Passing
        the real footprint (seq + 1) is what turns this into a rejection.
        """
        period, seq = 4_096_000, 256
        start = period - seq
        self.assertFalse(recipe.overlaps_validation(start, seq, block=4096, period=period))
        self.assertTrue(recipe.overlaps_validation(
            start, recipe.training_footprint(seq), block=4096, period=period))


class ResidualInitTest(unittest.TestCase):
    """_init used std=0.02 for every Linear. The residual projections (wo,
    down) are the two that write into the residual stream once per layer, so
    their variance compounds with depth; the standard correction is 1/sqrt(2L).
    """

    def test_ten_layers_scales_by_one_over_sqrt_twenty(self):
        self.assertAlmostEqual(recipe.residual_init_std(0.02, 10),
                               0.02 / (20 ** 0.5), places=9)

    def test_one_layer_still_scales(self):
        self.assertAlmostEqual(recipe.residual_init_std(0.02, 1),
                               0.02 / (2 ** 0.5), places=9)

    def test_zero_layers_is_rejected_rather_than_dividing_by_zero(self):
        with self.assertRaises(ValueError):
            recipe.residual_init_std(0.02, 0)


if __name__ == "__main__":
    unittest.main()
