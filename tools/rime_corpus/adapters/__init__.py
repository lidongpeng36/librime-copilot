"""Source adapters. Each one harvests; none of them redacts, filters or writes.

A third-party adapter is about thirty lines:

    SOURCE = "wechat"

    def iter_utterances(config) -> Iterator[tuple[str, str]]:
        \"\"\"Yield (ts_iso8601, text).\"\"\"

Keeping redaction out of the adapter is deliberate: it is the one thing that
must not vary in strength between sources.
"""
from . import claude, dingtalk

REGISTRY = {claude.SOURCE: claude, dingtalk.SOURCE: dingtalk}
