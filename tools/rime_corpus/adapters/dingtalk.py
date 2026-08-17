"""The user's own 1:1 DingTalk messages, via the `dws` CLI.

Colloquial counterweight to adapters.claude: every number the harness has
produced so far (27.5% wrong-first, +14% from re-ranking) comes from
technical Chinese typed at Claude Code. This harvests ordinary chat instead,
so the harness's conclusions can be checked against a different register.

Scouted and measured before this was written (do not re-derive):

  * GROUPS ARE OUT. `--types p2p` on `+chat-list` returns 54 conversations;
    the group route was tried and abandoned because `--sender-query` filters
    CLIENT-SIDE over whatever page was read, not server-side -- reading 97
    groups' full histories to keep <5% of it does not pay for itself. For
    1:1 the same filter is cheap: 6 conversations over 3 months yielded 407
    of the user's own messages, i.e. the full 54 project to roughly 3600.
  * `dws` can exit non-zero (e.g. "分页未完成") while stdout still holds a
    complete, valid JSON payload -- `+chat-list` did exactly that while this
    adapter was written (54 conversations came back on a run whose exit code
    was 1). So the return code is not the signal; parseable JSON on stdout
    is. Only a stdout that does not even parse is treated as a hard failure.
  * `+chat-messages` never exposes a `msgtype` field. Non-text messages
    (image, file, share/link card, voice-call notice, sticker) all come back
    as a normal `text` string that dws itself has already collapsed into a
    "[<label>] ..." placeholder ("[图片消息]", "[文件] ...", "[分享] ...",
    "[语音通话] ...", "[赞]", with no attachment info beyond that). Only
    image/file attachments carry a structural tether (`resourceRefs`); cards
    and system notices do not, so the placeholder-tag shape is the only
    signal left for them. Real typed Chinese was never observed to start
    that way in the messages sampled while writing this -- the risk (a
    typed message that starts with its own "[tag]") is accepted as a false
    negative, on the instruction to skip placeholder junk rather than
    emit it as if it were spoken/typed content.
"""
from __future__ import annotations

import json
import re
import subprocess
import sys
from datetime import datetime, timedelta, timezone
from typing import Iterator

SOURCE = "dingtalk"

DEFAULT_SENDER = "李东鹏"
DEFAULT_WINDOW_DAYS = 90
DEFAULT_MAX_ITEMS = 1000
_DWS = "dws"

# dws's own non-text placeholder shape: a short bracketed label, optionally
# followed by a description ("[分享] <title>"), with nothing before it. Real
# messages the user typed were never observed to open this way -- see the
# module docstring for the tradeoff.
_PLACEHOLDER_TAG = re.compile(r"^\[[^\[\]]{1,8}\]")

# "2026-05-19 11:08:42" -- naive, no offset. Confirmed +08:00 for this
# account: dws's own queryRange.endTime for "now" came back with an explicit
# +08:00 suffix on the same run, so that offset is applied rather than left
# naive (which real ISO 8601 does not allow).
_CREATE_TIME = re.compile(r"^\d{4}-\d{2}-\d{2} \d{2}:\d{2}:\d{2}$")


def iter_utterances(
    start: str | None = None,
    sender: str = DEFAULT_SENDER,
    max_items: int = DEFAULT_MAX_ITEMS,
    dws_path: str = _DWS,
) -> Iterator[tuple[str, str]]:
    """Yield (ts_iso8601, text) for the user's own text messages in every
    1:1 DingTalk conversation, no group chats, going back to `start`
    (default: 90 days ago)."""
    if start is None:
        start = (datetime.now(timezone.utc) - timedelta(days=DEFAULT_WINDOW_DAYS)).isoformat()

    listing = _run(
        [dws_path, "chat", "+chat-list", "--types", "p2p",
         "--page-all", "--page-limit", "100", "--format", "json"],
    )
    for chat in listing.get("chats", []):
        # Belt-and-suspenders: --types p2p already asked the source for this,
        # but a group message in the corpus is exactly the mistake this
        # adapter exists to avoid, so it is checked again here.
        if chat.get("chatMode") != "p2p":
            continue
        conversation_id = chat.get("openConversationId")
        if not conversation_id:
            continue
        yield from _iter_conversation(dws_path, conversation_id, start, sender, max_items)


def _iter_conversation(
    dws_path: str, conversation_id: str, start: str, sender: str, max_items: int,
) -> Iterator[tuple[str, str]]:
    try:
        data = _run([
            dws_path, "chat", "+chat-messages",
            "--conversation-id", conversation_id,
            "--start", start, "--order", "asc",
            "--page-all", "--page-limit", "100", "--max-items", str(max_items),
            "--sender-query", sender, "--format", "json",
        ])
    except _DwsError as exc:
        # One conversation's transport hiccup should not sink the harvest of
        # the other 53.
        print(f"dingtalk: {conversation_id}: {exc}", file=sys.stderr)
        return

    # An unresolved sender means the tool "保留全部消息" -- keeps everyone's
    # messages rather than filtering. Silently harvesting that would pull in
    # the other party's text, which is exactly what --sender-query exists to
    # prevent. Abort THIS conversation (yield nothing from it) rather than
    # trust an unfiltered read; the rest of the harvest still runs.
    senders = data.get("resolvedFilters", {}).get("senders", [])
    if not senders or any(s.get("status") != "resolved" for s in senders):
        print(
            f"dingtalk: {conversation_id}: sender {sender!r} did not resolve "
            f"({senders!r}) -- skipping, not harvesting unfiltered", file=sys.stderr,
        )
        return

    if not data.get("complete") or data.get("partial"):
        print(
            f"dingtalk: {conversation_id}: partial read "
            f"(complete={data.get('complete')}, failures={data.get('failures')})",
            file=sys.stderr,
        )

    for message in data.get("messages", []):
        # Image/file attachments carry this key structurally; see the module
        # docstring for why cards and system notices need the text-shape check
        # below instead.
        if "resourceRefs" in message:
            continue
        text = message.get("text")
        if not isinstance(text, str):
            continue
        text = text.strip()
        if not text or _PLACEHOLDER_TAG.match(text):
            continue
        yield _to_iso8601(message.get("createTime", "")), text


class _DwsError(RuntimeError):
    pass


def _run(argv: list[str]) -> dict:
    """Run a dws subcommand and parse its stdout as JSON.

    The exit code is NOT the signal: dws can exit non-zero while stdout
    still holds a complete, valid payload (measured on `+chat-list` while
    writing this -- see the module docstring). Only stdout that fails to
    parse is treated as a hard failure.
    """
    result = subprocess.run(argv, capture_output=True, text=True)
    try:
        return json.loads(result.stdout)
    except (json.JSONDecodeError, ValueError) as exc:
        raise _DwsError(
            f"{' '.join(argv[:3])}...: unparseable output (exit {result.returncode}): "
            f"{result.stderr.strip()[:500]}"
        ) from exc


def _to_iso8601(create_time: str) -> str:
    if not _CREATE_TIME.match(create_time):
        # Unrecognized shape: pass it through rather than raise. Nothing else
        # in the pipeline parses `ts` today (corpus.py just stores it), so a
        # malformed-but-present timestamp is preferable to losing the message.
        return create_time
    return create_time.replace(" ", "T", 1) + "+08:00"
