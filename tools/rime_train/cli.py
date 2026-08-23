"""The `rime-train` command.

Orchestration only; every decision lives in a module testable without a
network or a corpus. Same split as rime_copilot/cli.py and rime_corpus/cli.py.
"""
from __future__ import annotations

import argparse
import sys
from collections import Counter
from pathlib import Path
from typing import Sequence

from . import build as _build, charset, isolation, ngram
from .sources import SOURCES

DEFAULT_CACHE = Path.home() / ".local/share/rime-train"
# The 通用规范汉字表 the schema imports as its character table. A collocation
# containing anything outside it cannot be typed under this schema at all.
DEFAULT_CHARSET = Path.home() / ".local/share/rime-corpus/rime-dir/cn_dicts/8105.dict.yaml"


def _fetch_default_mirror() -> str:
    from .fetch import DEFAULT_MIRROR

    return DEFAULT_MIRROR


def _typeable(args) -> frozenset[str]:
    path = Path(args.charset)
    if not path.is_file():
        print(f"no character table at {path}", file=sys.stderr)
        raise SystemExit(1)
    chars = charset.load_charset(path)
    print(f"character table: {len(chars)} characters from {path}")
    return chars


def cmd_fetch(args) -> int:
    from . import fetch as _fetch

    for name in args.source:
        path = _fetch.fetch(SOURCES[name], Path(args.cache), mirror=args.mirror)
        print(f"{name} -> {path}")
    return 0


def cmd_build(args) -> int:
    typeable = _typeable(args)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    per_source: Counter = Counter()
    with open(out, "w", encoding="utf-8") as handle:
        for name in args.source:
            source = SOURCES[name]
            path = Path(args.cache) / Path(source.url).name
            if not path.is_file():
                print(f"{name}: not fetched ({path})", file=sys.stderr)
                return 1
            # Upsampling wraps build() rather than living inside it: build()
            # ends in dedup.unique(), so a repeat emitted within that stream
            # would be deduplicated away and do nothing at all. Each repetition
            # is its own pass over the file.
            for pass_index in range(source.repeat):
                for sentence in _build.build(path, source, typeable, args.limit_lines,
                                             han_only=not args.keep_punctuation):
                    handle.write(sentence + "\n")
                    per_source[name] += 1
            suffix = f" x{source.repeat}" if source.repeat > 1 else ""
            print(f"  {name} ({source.register}){suffix}: {per_source[name]} sentences")
    print(f"{sum(per_source.values())} sentences -> {out}")
    return 0


def cmd_isolate(args) -> int:
    """Check the built corpus against the evaluation set. Reports; never trims.

    A handful of hits and thousands of them mean completely different things --
    the second means the source is not what it claims to be -- and silently
    filtering would erase that distinction along with its evidence.
    """
    sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
    from rime_corpus import corpus, replay

    evals = []
    eval_dir = corpus.corpus_dir(getattr(args, "eval_corpus_dir", None))
    print(f"evaluation corpus dir: {eval_dir}")
    for path in sorted(eval_dir.glob("*.jsonl")):
        for record in corpus.iter_records(path):
            evals.extend(run for _, run in replay.han_runs(record["text"]))
    marks = isolation.fingerprints(evals, args.span)
    print(f"evaluation corpus: {len(evals)} Han runs -> {len(marks)} fingerprints")

    with open(args.corpus, encoding="utf-8") as handle:
        hits = isolation.overlaps((l.strip() for l in handle), marks, args.span)
    print(f"overlapping training sentences: {len(hits)}")
    for hit in hits[:20]:
        print(f"    {hit}")
    if len(hits) > 20:
        print(f"    ... and {len(hits) - 20} more")
    return 0


def cmd_count(args) -> int:
    """One pass per shard, each writing its own thresholded slice.

    Sharding is not optional at corpus scale: counting LCCC in one pass would
    reach an estimated 10-15 GB resident (measured: 1M of its 17.5M sentences
    give 5.9M distinct keys at 1.06 GB). Every shard's keys are disjoint by
    construction -- they are partitioned on the key's first character -- so
    appending the slices is the complete table, in no particular order, which
    is what build_grammar wants anyway since it sorts.
    """
    distinct = written = 0
    with open(args.output, "w", encoding="utf-8") as out:
        for shard in range(args.shards):
            with open(args.corpus, encoding="utf-8") as handle:
                # Conditional values need each key's prefix counted too, so
                # counting starts one character shorter than octagram looks up.
                counts = ngram.count((line.strip() for line in handle),
                                     min_length=(ngram.MIN_LENGTH - 1
                                                 if args.conditional else ngram.MIN_LENGTH),
                                     shard=shard, shards=args.shards)
            distinct += len(counts)
            emitter = (ngram.emit_conditional if args.conditional else ngram.emit)
            for line in emitter(counts, args.min_count):
                out.write(line + "\n")
                written += 1
            print(f"  shard {shard + 1}/{args.shards}: {len(counts)} distinct, "
                  f"{written} kept so far")
    print(f"{distinct} distinct collocations, "
          f"{written} with count >= {args.min_count} -> {args.output}")
    return 0


def cmd_tokenize(args) -> int:
    """Text -> uint16 token ids on disk, once, for the training loader."""
    from .train import tokenize_corpus
    from .vocab import Vocab, build as build_vocab

    vocab = Vocab(build_vocab(Path(args.chars)))
    total = tokenize_corpus(Path(args.corpus), vocab, Path(args.output), args.limit)
    print(f"{total:,} tokens ({total/1e9:.3f}B, vocab {len(vocab)}) -> {args.output}")
    return 0


def cmd_scoring_form(args) -> int:
    from . import goldens

    if args.out:
        path = Path(args.out)
        path.parent.mkdir(parents=True, exist_ok=True)
        with open(path, "w", encoding="utf-8") as sink:
            n = goldens.emit_scoring_form(sink)
        print(f"wrote {n} cases to {path}")
    else:
        goldens.emit_scoring_form(sys.stdout)
    return 0


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="rime-train")
    parser.add_argument("--cache", default=str(DEFAULT_CACHE))
    sub = parser.add_subparsers(dest="command", required=True)

    fetch = sub.add_parser("fetch", help="download a source (the only network step)")
    fetch.add_argument("source", nargs="+", choices=list(SOURCES))
    # Takes its value explicitly rather than as an optional one: with
    # `nargs="?"` argparse swallows the first positional after it, so
    # `--mirror lccc-base skypile-0` set the mirror host to "lccc-base" and
    # then tried to resolve it. Required wherever the origin is throttled --
    # huggingface.co serves the training host at ~1 kB/s, hf-mirror.com at
    # 6.9 MB/s.
    fetch.add_argument("--mirror", default=None, metavar="HOST",
                       help=f"swap huggingface.co for this host, e.g. {_fetch_default_mirror()}")
    fetch.set_defaults(func=cmd_fetch)

    build = sub.add_parser("build", help="normalize, split, filter and dedup into sentences")
    build.add_argument("source", nargs="+", choices=list(SOURCES))
    build.add_argument("--output", required=True)
    build.add_argument("--charset", default=str(DEFAULT_CHARSET))
    build.add_argument("--limit-lines", type=int, default=None,
                       help="stop after this many input lines per source")
    build.add_argument("--keep-punctuation", action="store_true",
                       help="keep punctuation, Latin and digits -- required for "
                            "language-model training, wrong for the n-gram")
    build.set_defaults(func=cmd_build)

    isolate = sub.add_parser("isolate", help="check a built corpus against the eval set")
    isolate.add_argument("--corpus", required=True)
    isolate.add_argument("--span", type=int, default=isolation.MIN_SHARED_SPAN)
    isolate.add_argument(
        "--eval-corpus-dir", default=None,
        help="which evaluation corpus to fingerprint against. Under a train/eval "
             "split this MUST be the eval half; the default directory holds both, "
             "and checking against both reports the training half as an overlap "
             "and means nothing.")
    isolate.set_defaults(func=cmd_isolate)

    count = sub.add_parser("count", help="count collocations into build_grammar's input")
    count.add_argument("--corpus", required=True)
    count.add_argument("--output", required=True)
    count.add_argument("--min-count", type=int, default=2)
    count.add_argument("--conditional", action="store_true",
                       help="emit P(last char | the rest) instead of raw counts")
    count.add_argument("--shards", type=int, default=8,
                       help="passes over the corpus; memory scales as 1/shards")
    count.set_defaults(func=cmd_count)

    tokenize = sub.add_parser("tokenize", help="corpus text -> uint16 token ids")
    tokenize.add_argument("--corpus", required=True)
    tokenize.add_argument("--chars", required=True)
    tokenize.add_argument("--output", required=True)
    tokenize.add_argument("--limit", type=int, default=None)
    tokenize.set_defaults(func=cmd_tokenize)

    scoring_form = sub.add_parser(
        "scoring-form",
        help="emit the golden fixture that holds C++ AlignToTrainingForm and "
             "normalize.scoring_form to one truth",
    )
    scoring_form.add_argument(
        "--out", default=None,
        help="file to write (default stdout); the committed fixture lives at "
             "test/data/scoring_form_golden.jsonl")
    scoring_form.set_defaults(func=cmd_scoring_form)

    args = parser.parse_args(argv)
    return args.func(args)
