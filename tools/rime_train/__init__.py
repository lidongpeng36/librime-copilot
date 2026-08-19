"""Training-corpus pipeline.

Deliberately a separate package from `rime_corpus`, which owns the EVALUATION
corpus harvested from the user's own writing. They are the two halves of the
same project and the one invariant that must never break is that they do not
mix -- see `docs/superpowers/specs/2026-08-19-corpus-pipeline-design.md`.
Physical separation is what keeps that legible: inside one package, an
accidental import of the eval set into a training path would look ordinary.
"""
