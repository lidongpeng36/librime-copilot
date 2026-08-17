"""The `rime-corpus` command.

Orchestration only: every decision lives in a module that can be tested without
a network, an input method, ~/Library/Rime, or a real corpus directory. Same
split as tools/rime_copilot/cli.py.
"""
from __future__ import annotations

import argparse
import json
import re
import sys
from collections import Counter
from pathlib import Path
from typing import Sequence

from . import corpus
from .adapters import REGISTRY
from .replay import DEFAULT_PRISTINE_DIR

# Residue worth eyeballing after regex redaction. These are NOT deleted --
# regex redaction cannot catch internal project names, personal names or
# customer names, and auto-deleting would manufacture a false sense that the
# corpus is clean. Listing is the honest option.
_SUSPECT = [
    ("long alphanumeric run", re.compile(r"\b[A-Za-z0-9_-]{20,}\b")),
    ("path-shaped", re.compile(r"(?:^|\s)/[\w./-]{6,}")),
    ("uppercase acronym", re.compile(r"\b[A-Z]{3,}\b")),
]


def _ingest(args: argparse.Namespace) -> int:
    directory = corpus.corpus_dir(args.corpus_dir)
    total = 0
    for name in args.source:
        adapter = REGISTRY[name]
        records = (
            corpus.make_record(adapter.SOURCE, ts, text)
            for ts, text in adapter.iter_utterances()
        )
        written = corpus.append(directory / f"{adapter.SOURCE}.jsonl", records)
        print(f"{adapter.SOURCE}: +{written}")
        total += written
    print(f"corpus: {directory}")
    # Always 0: an empty harvest is a legitimate result (re-running ingest
    # after the corpus is built adds nothing), and `stats` is where an empty
    # corpus is reported as a problem.
    return 0


def _stats(args: argparse.Namespace) -> int:
    directory = corpus.corpus_dir(args.corpus_dir)
    files = sorted(directory.glob("*.jsonl"))
    if not files:
        print(f"no corpus at {directory}", file=sys.stderr)
        return 1
    suspects: dict[str, Counter] = {label: Counter() for label, _ in _SUSPECT}
    for path in files:
        count = han = 0
        redacted: Counter = Counter()
        for record in corpus.iter_records(path):
            count += 1
            han += corpus.han_chars(record["text"])
            redacted.update(record["redacted"])
            for label, pattern in _SUSPECT:
                suspects[label].update(pattern.findall(record["text"]))
        print(f"{path.name}: {count} utterances, {han} Han chars, redacted={dict(redacted)}")
    print("\nresidue after redaction (listed, never deleted):")
    for label, counter in suspects.items():
        top = ", ".join(f"{value}({n})" for value, n in counter.most_common(10))
        print(f"  {label}: {top or '-'}")
    return 0


def _verify_speller(args: argparse.Namespace) -> int:
    """Check every syllable's keystrokes against real Rime.

    Not a unit test: it needs a real schema and dictionary, so it cannot run in
    CI. A green CI is not evidence that the keystroke table is right; this is.
    """
    import subprocess

    from . import speller as _speller

    sp = _speller.Speller(_speller.load_rules(_speller.FLYPY_RULES))
    pairs = []
    missing = []
    for syllable in _all_syllables():
        keys = sp.keys(syllable)
        if keys is None:
            # A couple of monosyllables (呣/嗯: "m", "n") have no two-key
            # spelling under this schema's algebra at all -- expected (see
            # task-6-brief.md's "facts already established"), not a
            # disambiguation failure. There is no keystroke pair to send Rime
            # for them, so they are excluded from the count rather than
            # aborting a run that would otherwise check the other ~408.
            missing.append(syllable)
            continue
        pairs.append((syllable, keys))
    if missing:
        print(
            f"{len(missing)} syllable(s) have no two-key spelling, excluded from "
            f"the check: {', '.join(missing)}",
            file=sys.stderr,
        )

    stdin = "".join(f"{s}\t{k}\n" for s, k in pairs)
    result = subprocess.run(
        [args.replayer, "--rime-dir", args.rime_dir, "--verify-speller"],
        input=stdin, capture_output=True, text=True, check=True,
    )
    bad = 0
    for line in result.stdout.splitlines():
        record = json.loads(line)
        if not record["ok"]:
            bad += 1
            print(
                f"MISMATCH {record['syllable']} -> {record['keys']} "
                f"but Rime read it as {record['preedit']!r}",
                file=sys.stderr,
            )
    print(f"{len(pairs) - bad}/{len(pairs)} syllables verified")
    return 1 if bad else 0


def _load_corpus(args: argparse.Namespace) -> tuple[Path, list[dict]] | None:
    """Shared by every subcommand that replays the whole corpus. Returns
    None (having already printed the error) when there is nothing to do."""
    directory = corpus.corpus_dir(args.corpus_dir)
    records = [
        r for path in sorted(directory.glob("*.jsonl")) for r in corpus.iter_records(path)
    ]
    if not records:
        print(f"no corpus at {directory}", file=sys.stderr)
        return None
    return directory, records


def _build_requests(records: list[dict]) -> list[dict]:
    from . import replay, speller as _speller

    sp = _speller.Speller(_speller.load_rules(_speller.FLYPY_RULES))
    return replay.build_requests(records, sp)


def _coverage_report(name: str, rows: list[tuple[str, "object"]]) -> None:
    """One register's R1/R2/R3 block. `rows` is (unit_text, Coverage)."""
    from . import lattice

    total = len(rows)
    if not total:
        return
    reachable = [c for _, c in rows if c.reachable]
    chars = sum(len(t) for t, _ in rows)

    print(f"\n=== {name} ===")
    print(f"units (Han runs): {total}    characters: {chars}")

    # R1
    print(f"R1 reachable: {len(reachable)}/{total} ({100 * len(reachable) / total:.1f}%)")

    # R2 -- only meaningful for what is NOT reachable
    causes: Counter = Counter()
    samples: dict[str, dict[tuple, "object"]] = {}
    for _, cov in rows:
        for gap in cov.gaps:
            causes[gap.cause] += 1
            # Keyed by (character, wanted reading): the same 多音字 read the
            # same wrong way is one finding however many times it recurs, and
            # eight copies of it would hide a second, rarer cause.
            seen = samples.setdefault(gap.cause, {})
            if len(seen) < 8:
                seen.setdefault((gap.char, gap.want), gap)
    if causes:
        print("R2 why not reachable:")
        for cause, count in causes.most_common():
            print(f"    {cause}: {count} ({len(samples[cause])} distinct shown)")
            for gap in samples[cause].values():
                if cause == lattice.MISSING_READING:
                    print(
                        f"        {gap.char}  wanted {gap.want!r}, "
                        f"lexicon has {', '.join(gap.have)}"
                    )
                elif cause == lattice.MISSING_CHAR:
                    print(f"        {gap.char}  ({gap.want})")
                else:
                    print(f"        {gap.char!r}")
    else:
        print("R2 why not reachable: (nothing unreachable)")

    # R3 -- the number this whole command exists for
    if not reachable:
        return
    words = sum(c.min_words for c in reachable)
    singles = sum(c.singles for c in reachable)
    reach_chars = sum(len(t) for t, c in rows if c.reachable)
    print(
        f"R3 shortest path: {words / len(reachable):.2f} entries per run, "
        f"{reach_chars / words:.2f} characters per entry"
    )
    print(
        f"   single-character fallback: {singles}/{words} entries "
        f"({100 * singles / words:.1f}%), "
        f"{100 * singles / reach_chars:.1f}% of characters"
    )
    spelled_out = sum(1 for c in reachable if c.min_words == c.singles)
    print(
        f"   runs the lexicon can only spell one character at a time: "
        f"{spelled_out}/{len(reachable)} ({100 * spelled_out / len(reachable):.1f}%)"
    )


def _coverage(args: argparse.Namespace) -> int:
    """S0-a: can the lexicon produce what the user wrote, and in how few
    pieces? The ceiling any lattice decoder is built under, measured before
    any model exists. See lattice.py for why reachability alone is not the
    question."""
    from . import lattice, replay, speller as _speller

    loaded = _load_corpus(args)
    if loaded is None:
        return 1
    _, records = loaded

    units: list[tuple[str, str]] = []  # (source, Han run)
    for record in records:
        for _, run in replay.han_runs(record["text"]):
            units.append((record["src"], run))
    print(f"{len(records)} utterances -> {len(units)} Han runs")

    syllables = {run: _speller.syllables(run) for _, run in units}

    # Only entries whose text occurs somewhere in the corpus can ever span
    # gold. Filtering here rather than after loading is what keeps
    # cn_dicts/tencent (981k entries, every one needing pypinyin) affordable.
    wanted: set[str] = set()
    for _, run in units:
        wanted.update(lattice.substrings(run))
    print(f"{len(wanted)} distinct substrings to match against")

    dict_path = Path(args.rime_dir) / f"{args.dict}.dict.yaml"
    tables = lattice.import_tables(dict_path)
    print(f"\nlexicon: {dict_path}")
    for table in tables:
        print(f"    {table.relative_to(dict_path.parent)}")

    def run_arm(paths: list[Path], label: str) -> None:
        lex, per_source = lattice.load(paths, keep=lambda w: w in wanted)
        print(f"\n### {label}")
        print(
            "entries kept: "
            + ", ".join(f"{name} {count}" for name, count in per_source.items())
        )
        print(f"distinct (text, reading) spans: {len(lex)}    misaligned: {lex.misaligned}")
        by_source: dict[str, list] = {}
        for source, run in units:
            cov = lattice.analyze(run, syllables[run], lex)
            by_source.setdefault(source, []).append((run, cov))
        for source in sorted(by_source):
            _coverage_report(source, by_source[source])
        _coverage_report("all", [row for rows in by_source.values() for row in rows])

    run_arm(tables, "full lexicon")

    # Two sources are worth measuring out, for opposite reasons.
    #
    # tencent carries no readings at all (`columns: [text, weight]`), so this
    # harness gives it pypinyin's single guess where librime derives possibly
    # several by re-encoding. The bias only ever understates coverage, but how
    # much of the answer rests on the approximated source is invisible until
    # it is removed.
    #
    # private/custom is 46,699 entries exported from the user's own Sogou
    # Pinyin, i.e. a lexicon learned from years of this same person's writing
    # (2026-08-14-rime-copilot-toolchain-design.md). Including it is correct
    # for "can the deployed system spell this" -- it IS part of the deployed
    # system. It is not correct for "what would a model have to learn", since
    # that lexicon is exactly the personal knowledge a general model would
    # not have. Both questions are real; only reporting both distinguishes
    # them.
    for stem, why in (
        ("tencent", "reading-approximated source"),
        ("custom", "the user's own Sogou export"),
    ):
        without = [p for p in tables if lattice.table_name(p) != stem]
        if len(without) == len(tables):
            raise SystemExit(f"{stem} is not one of {dict_path}'s import_tables")
        run_arm(without, f"without {stem} ({why})")

    return 0


def _oracle(args: argparse.Namespace) -> int:
    """The four-way bucket split (metrics.bucket) and the oracle bound --
    the share of segments where re-ranking has a real, non-policy target
    (bucket C only; see metrics.py for why bucket B is excluded)."""
    from . import metrics, replay

    loaded = _load_corpus(args)
    if loaded is None:
        return 1
    _, records = loaded

    requests = _build_requests(records)
    print(f"{len(records)} utterances -> {len(requests)} runs")

    # Sentinel 4 (replay.assert_deterministic), before the number is trusted.
    # A harness that cheats in its own favour reports a positive result,
    # which nobody questions -- see replay.py's module docstring. The
    # pristine-userdb restore happens INSIDE this call, before and after
    # each of its two passes (Correction 3: without it, replay trains the
    # user dictionary on the very corpus it is measuring).
    print("\nrunning the corpus twice for the determinism check...")
    try:
        responses = replay.assert_deterministic(
            requests, args.replayer, args.rime_dir, args.pristine, window=args.window
        )
    except AssertionError as exc:
        print(f"determinism: MISMATCH\n{exc}", file=sys.stderr)
        return 3
    print("determinism: ok (timings stripped, byte-identical across two full passes)")

    summary = metrics.summarize(responses, requests, args.window)
    b = summary["buckets"]
    total = b["total"] or 1
    print(f"\nsegments ({summary['errors']} error response(s) excluded): {b['total']}")
    print(f"  A first (already correct)     : {b['A']:6}  ({b['A']/total:.1%})")
    print(f"  B raw-first-by-design          : {b['B']:6}  ({b['B']/total:.1%})")
    print(f"  C real re-ranking opportunity  : {b['C']:6}  ({b['C']/total:.1%})")
    print(f"  D unreachable                  : {b['D']:6}  ({b['D']/total:.1%})")
    print(f"\nORACLE BOUND (bucket C -- B is policy, not headroom): {b['oracle_bound']:.1%}")
    trustworthy = summary["responses"] - summary["errors"]
    print(
        f"divergence rate: {summary['divergence_rate']:.1%} "
        f"({summary['diverged']} of {trustworthy} trustworthy responses)"
    )
    if b["C"]:
        print("\nrank of the correct answer within bucket C:")
        for rank, count in summary["gold_rank_in_c"].items():
            print(f"  rank {rank:3}: {count:6}  ({count/b['C']:.1%})")
    print(
        "\nReconstruction bias: this replays text the user WROTE, not input "
        "the user PERFORMED. Treat as a go/no-go threshold, not a live rate."
    )
    return 0


def _export_evalset(args: argparse.Namespace) -> int:
    """Write the score_candidates.cc input set: buckets A and C, restricted
    to segments with non-empty trailing-Han context (see evalset.py)."""
    from . import evalset as _evalset, replay

    loaded = _load_corpus(args)
    if loaded is None:
        return 1
    _, records = loaded

    requests = _build_requests(records)
    print(f"{len(records)} utterances -> {len(requests)} runs")

    responses = replay.run_arm(
        requests, args.replayer, args.rime_dir, args.pristine, window=args.window
    )
    rows = _evalset.build_evalset(requests, responses, args.window)
    with open(args.output, "w", encoding="utf-8") as handle:
        for row in rows:
            handle.write(json.dumps(row, ensure_ascii=False) + "\n")

    n_a = sum(1 for r in rows if r["bucket"] == "A")
    n_c = sum(1 for r in rows if r["bucket"] == "C")
    print(f"wrote {len(rows)} records to {args.output} (A={n_a}, C={n_c})")
    return 0


def _all_syllables() -> list[str]:
    """Every 全拼 syllable pypinyin can produce, from the full Han range."""
    from pypinyin import Style, lazy_pinyin

    found = set()
    for codepoint in range(0x4E00, 0xA000):
        char = chr(codepoint)
        for syllable in lazy_pinyin(char, style=Style.NORMAL, errors="ignore"):
            if syllable.isascii() and syllable.isalpha():
                found.add(syllable)
    return sorted(found)


def main(argv: Sequence[str] | None = None) -> int:
    parser = argparse.ArgumentParser(prog="rime-corpus")
    parser.add_argument("--corpus-dir", default=None)
    sub = parser.add_subparsers(dest="command", required=True)

    ingest = sub.add_parser("ingest", help="harvest utterances into the corpus")
    ingest.add_argument("source", nargs="*", default=list(REGISTRY), choices=list(REGISTRY))
    ingest.set_defaults(func=_ingest)

    stats = sub.add_parser("stats", help="corpus size and redaction residue")
    stats.set_defaults(func=_stats)

    verify = sub.add_parser("verify-speller", help="check keystrokes against real Rime")
    verify.add_argument("--replayer", required=True)
    verify.add_argument("--rime-dir", required=True)
    verify.set_defaults(func=_verify_speller)

    coverage = sub.add_parser(
        "coverage", help="S0-a: can the lexicon spell what the user wrote, in how few pieces"
    )
    # No replayer and no Rime session: this asks only what the dictionary
    # files contain, so it runs in seconds and needs nothing built.
    coverage.add_argument("--rime-dir", required=True)
    # Must match the schema's translator/dictionary -- `private` for
    # double_pinyin_flypy (build/double_pinyin_flypy.schema.yaml). Measuring a
    # dictionary the schema does not load would be a measurement of nothing.
    coverage.add_argument("--dict", default="private")
    coverage.set_defaults(func=_coverage)

    oracle = sub.add_parser("oracle", help="the four-way bucket split and the oracle bound")
    oracle.add_argument("--replayer", required=True)
    oracle.add_argument("--rime-dir", required=True)
    oracle.add_argument("--pristine", default=str(DEFAULT_PRISTINE_DIR))
    # Must equal copilot/rerank/window in the replay schema -- re-ranking can
    # only promote a candidate from inside its own window, so this is exactly
    # the line between "opportunity" (bucket B/C) and "unreachable" (D). The
    # user's real setting is window: 32 (double_pinyin_flypy.custom.yaml).
    oracle.add_argument("--window", type=int, default=32)
    oracle.set_defaults(func=_oracle)

    export = sub.add_parser(
        "export-evalset", help="export the LLM scoring set (score_candidates.cc's input)"
    )
    export.add_argument("--replayer", required=True)
    export.add_argument("--rime-dir", required=True)
    export.add_argument("--pristine", default=str(DEFAULT_PRISTINE_DIR))
    export.add_argument("--window", type=int, default=32)
    export.add_argument("--output", required=True)
    export.set_defaults(func=_export_evalset)

    # compare-rerank (paired on/off comparison of the DB re-ranker) is
    # deliberately NOT wired in here as a subcommand: it needs a second
    # rime-dir (`--rime-dir-norerank`) that the other subcommands have no use
    # for, and argparse's subparsers + `nargs=REMAINDER` do not compose
    # cleanly enough to forward arbitrary flags through to a second
    # ArgumentParser without surprises. It stays reachable as its own module
    # (`compare_rerank.py`'s `main()`), which is how it is invoked today; see
    # that module's docstring for how it reuses replay.py/metrics.py.

    args = parser.parse_args(argv)
    return args.func(args)
