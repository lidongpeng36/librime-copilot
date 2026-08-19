"""The only stage that touches the network.

Isolated so every other stage's tests need nothing, and so a re-run costs
nothing: a completed download is left alone unless `force` says otherwise, and
an interrupted one lands in a `.part` file that is never mistaken for the real
thing -- the same staged-rename rule the rest of this repo's downloads follow.
"""
from __future__ import annotations

import sys
from pathlib import Path

from .sources import Source

# huggingface.co is reachable from the training host but serves at about
# 1 kB/s there, which is not a download; the same file over hf-mirror.com
# measured 6.9 MB/s. The mirror is therefore not a convenience, it is the
# difference between an hour and a month, and it is a host swap rather than a
# different API -- the paths are identical.
DEFAULT_MIRROR = "hf-mirror.com"


def mirrored(url: str, mirror: str | None) -> str:
    return url.replace("huggingface.co", mirror, 1) if mirror else url


def fetch(source: Source, cache: Path, force: bool = False,
          mirror: str | None = None) -> Path:
    import requests  # lazy: every other module imports on a stock interpreter

    cache.mkdir(parents=True, exist_ok=True)
    target = cache / Path(source.url).name
    if target.is_file() and not force:
        print(f"already present: {target} ({target.stat().st_size} bytes)")
        return target

    url = mirrored(source.url, mirror)
    if url != source.url:
        print(f"  via {mirror}", file=sys.stderr)
    partial = target.with_name(target.name + ".part")
    written = 0
    with requests.get(url, stream=True, timeout=60) as response:
        response.raise_for_status()
        total = int(response.headers.get("content-length", 0))
        with open(partial, "wb") as handle:
            for chunk in response.iter_content(chunk_size=1 << 22):
                handle.write(chunk)
                written += len(chunk)
                if total:
                    pct = 100 * written / total
                    print(f"\r  {source.name}: {written >> 20} / {total >> 20} MB "
                          f"({pct:.0f}%)", end="", file=sys.stderr)
    print(file=sys.stderr)
    partial.replace(target)
    return target
