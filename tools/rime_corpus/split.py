"""Partitioning the corpus into a training half and an evaluation half.

By time, not at random. The evaluation corpus is the user's own text and so is
the most valuable training data -- the same records -- and this is the only
thing standing between the two uses. Time is chosen because it is the setting
that actually obtains (a deployed model is trained on the past and used on the
future) and because it excludes contamination by construction rather than by a
seed: a random split puts adjacent messages of one conversation on both sides,
and topic and phrasing are strongly correlated within a conversation.

Pure. The CLI writes the files; this decides which record goes where.
"""
from __future__ import annotations

from datetime import date, datetime
from typing import Iterable


def partition_by_time(records: Iterable[dict], cut: str) -> tuple[list[dict], list[dict]]:
    """Split into (before the cut, on or after it), preserving input order.

    Half-open at the cut: a record stamped exactly `cut` is evaluation, so
    "trained on everything before" and "measured from here on" are exhaustive
    and cannot both claim it.

    Compared on the record's OWN local date. Timestamps carry +08:00, and a
    message at 00:30 on the cut day is still the previous day in UTC -- the
    user's calendar day is the boundary a human means by this cut.

    Raises ValueError on a timestamp that will not parse, rather than filing it
    on one side by default: an unreadable record silently landing in training is
    how an evaluation set gets trained on.
    """
    boundary = date.fromisoformat(cut)
    train: list[dict] = []
    evaluation: list[dict] = []
    for record in records:
        stamp = record.get("ts", "")
        try:
            when = datetime.fromisoformat(stamp).date()
        except (TypeError, ValueError) as exc:
            raise ValueError(f"unparseable timestamp {stamp!r} on record "
                             f"{record.get('id')!r}") from exc
        (evaluation if when >= boundary else train).append(record)
    return train, evaluation
