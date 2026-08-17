"""Typed prompts from Claude Code transcripts.

The filter is narrower than it looks and every clause was measured against a
real transcript directory:

  * `type == "user"` also covers TOOL RESULTS. They are distinguished only by
    `message.content` being a list rather than a string -- 9298 of them against
    816 strings on the machine this was written on. Getting this wrong fills the
    corpus with command output.
  * `promptSource` separates what the user typed from what the harness injected.
    "system" and an absent value are /compact markers, continuation summaries
    and <local-command-caveat> wrappers. "suggestion_accepted" was offered by
    the tool, not composed by the user.
  * Prompts starting with "<" or carrying a <system-reminder> are harness
    injections that survived the checks above.
"""
from __future__ import annotations

import json
from pathlib import Path
from typing import Iterator

SOURCE = "claude"

DEFAULT_ROOT = Path.home() / ".claude" / "projects"
_TYPED = {"typed", "queued"}


def iter_utterances(root: Path | None = None) -> Iterator[tuple[str, str]]:
    """Yield (ts_iso8601, text) for every prompt the user actually typed."""
    root = Path(root) if root is not None else DEFAULT_ROOT
    for path in sorted(root.glob("*/*.jsonl")):
        with open(path, encoding="utf-8", errors="replace") as handle:
            for line in handle:
                record = _parse(line)
                if record is None:
                    continue
                yield record


def _parse(line: str) -> tuple[str, str] | None:
    try:
        data = json.loads(line)
    except (json.JSONDecodeError, ValueError):
        return None
    if data.get("type") != "user":
        return None
    if data.get("promptSource") not in _TYPED:
        return None
    content = data.get("message", {}).get("content")
    if not isinstance(content, str):
        return None
    text = content.strip()
    if not text or text.startswith("<") or "<system-reminder>" in text:
        return None
    return data.get("timestamp", ""), text
