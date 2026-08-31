# librime-copilot

> from [librime-predict](https://github.com/rime/librime-predict.git)

librime plugin. Copilot next word prediction with LLM support.

## Features

- **Next Word Prediction** - DB-based n-gram prediction and LLM-based prediction
- **Auto Spacer** - Automatically add spaces between Chinese and English/numbers
- **IME Bridge** - Control `ascii_mode` via IPC from external editors (Neovim, Obsidian, VS Code)

## Building the db

`tools/rime-copilot build` turns Rime dictionaries into the prediction db. It
reads a JSON config listing the dictionaries to merge (start from
`tools/dict.example.json`) and writes the built db to a single output file.
Point it at your own paths with `--config` and `--output`:

```sh
tools/rime-copilot build --config path/to/dict.json --output copilot.db --force-build
```

The Configuration section below documents the plugin's own default —
`db: copilot.db` in your Rime *user* directory (not `private/`) — so
`--output` should generally point there, or wherever your schema's `db:`
setting names, or the plugin will not open what you just built.

Without `--config`/`--output`, `build` defaults to `<rime-dir>/private/dict.json`
→ `<rime-dir>/private/private.predict.db` — this repository author's own
layout, kept as the default because `fetch`, `update`, and "Restoring on a
new Mac" below all assume it. If you are only building the db by hand, the
explicit form above is the one to use.

`build` is one step of the full pipeline — `fetch` downloads dictionaries,
`build` compiles them, `deploy` reloads Squirrel, and `update` chains all
three (using the default paths, not the overrides above). See "Restoring on a
new Mac" below for what a fresh machine needs before any of that runs, and
for the `pypinyin` dependency `build` picks up when a dictionary has no
pinyin column.

Each word is split at every boundary into `prefix → suffix`, which is exactly
how the plugin queries it: the last 1..N characters before the caret are the
key, and the highest-weighted continuations are the answer.

**Weights mean "larger = more likely"** — the same convention the plugin sorts
by. A dictionary whose third column is a rank (1 = best) or a line number will
silently invert every ordering; `tools/dump_copilot` prints a key's
continuations with weights and ranks so you can check:

```sh
dump_copilot copilot.db --find 议 -- 建
```

Dictionaries differ in what they carry. Only some ship real frequencies; others
use a constant filler, which is a membership signal ("this word exists"), not a
ranking signal. Don't rescale the frequency-bearing ones into a narrow band —
word frequency is long-tailed, and a linear min-max squeeze collapses almost
everything onto the same value. Mark a personal dictionary with `"top": true`
to lift all of its entries above the others while keeping their relative order.

A `top` source is stacked additively, not swapped in: `ceiling + existing +
own`, so every one of its entries outranks every ordinary entry while its own
frequency order survives (replacing outright was measured to invert real
frequencies). Left at that, a personal commit count is dwarfed by the public
weight it is stacked on — `确定是 = ceiling + 14925 + 7`, the commit count
moving the result by 0.00004% — so the whole personal band ends up ordered by
public frequency, and any candidate-ranking gate downstream (the copilot db's)
has nothing of the personal signal left to measure. Add `"boost": "log"` to a
`top` source in `dict.json` to fix that: it rescales that source's own weight
onto the *same* 0..ceiling range as the public term (log rather than linear,
because commit counts are long-tailed — over a third of a typical personal
lexicon is a single commit, and a linear rescale would flatten everything
below the top few hundred). It does not make the personal term win outright;
a public weight near the ceiling can still outrank it.

```json
{ "dict": "~/Library/Rime/private/custom.dict.yaml", "top": true, "boost": "log" }
```

`boost` is only valid on a `top` source, and `"log"` is currently the only mode.

### Pruning a Sogou-exported personal lexicon

`custom.dict.yaml` exported from Sogou is mostly cross-word-boundary
fragments learned from sentence input (`的问题`, `编译的`, `这个我`), which is
what gives a key like `的` well over a thousand continuations once that
dictionary is marked `top`. `rime-copilot clean` prunes it in three steps:

```sh
rime-copilot clean            # classify every entry, write the review file
$EDITOR ~/Library/Rime/private/clean_out/review.tsv
rime-copilot clean --apply    # read the review file back, rewrite custom.dict.yaml
```

The first run classifies `private/custom.dict.yaml` against a rule chain
(oracled by `jieba`'s segmentation dictionary — see `CLAUDE.md`'s "lexicon
oracle" note) into `keep`, `review`, and `drop`, and writes two files under
`private/clean_out/`: `review.tsv`, grouped and pre-filled with the default
action each rule suggests, and `drop.tsv`, an audit trail of every entry
being removed and why (nothing reads it back). Editing `review.tsv` means
changing the first column (`keep` or `drop`) on the rows the chain got
wrong — the rest of each row is context, not input. `clean --apply` then
reads that file, rewrites `custom.dict.yaml` to only what survived, and
prints how many entries did.

Two thresholds tune the chain, both in commit counts (the third column of
`custom.dict.yaml`): `--threshold-high` (default 100) is how many of the
user's own commits are enough to overrule a structural verdict — the escape
hatch that keeps real words like `好了` and `是的` that no dictionary-based
rule alone can distinguish from a fragment; `--threshold-low` (default 3) is
the point below which a pin carries no evidence at all.

Measured on one real lexicon: 46,699 entries became 8,231, and the key `的`
went from 1,326 continuations to 549 (`了`, from 486 to 249).

`clean --apply` preserves the untouched export at
`private/custom.dict.yaml.raw` the first time it runs — never again, since a
second overwrite there would replace the true original with an
already-cleaned copy — because nothing regenerates a Sogou export. Both
writes are staged onto a sibling and renamed into place, so an interrupted
run cannot leave a truncated `.raw` that the never-again guard would read as
complete. `backup` carries that file to the vault alongside the live
`custom.dict.yaml` **and `private/.copilot_clean_stamp.json`**, so a restore
on a new machine gets the cleaned dictionary, the pristine original it was
cleaned from, and the record that it was already cleaned.

That stamp is not bookkeeping. Without it a second machine reports
`lexicon: not cleaned`, and the obvious response — run `clean --apply` there
— would copy the already-cleaned file to `.raw` and then `backup` would push
that over the genuine export in the vault, losing it everywhere at once.

Both commands refuse rather than guess:

| Situation | What happens |
| --- | --- |
| `review.tsv` already exists | `clean` refuses; pass `--force` to regenerate and lose the annotations. `--dry-run` is exempt — it writes nothing |
| thresholds differ from the ones `review.tsv` was generated with | `--apply` refuses, naming both sets; the header comment in `review.tsv` records them |
| a clean stamp exists but `.raw` does not | `--apply` refuses — the pristine original has not arrived on this machine yet |
| `.raw` disagrees with the stamp's `raw_sha256` | `--apply` refuses |
| entry count disagrees with the file's vocabulary lines | `--apply` refuses rather than erase the rows `read_entries` skipped |
| a word is given two conflicting decisions in `review.tsv` | `--apply` refuses, naming both line numbers |

### On a second machine

The lexicon, the pristine export, the stamp and `dict.json` all travel in the
vault; the prediction database is rebuilt locally; the code and `jieba` do
not travel at all. Order matters:

```sh
git pull                  # code first -- see the warning below
rime-copilot install      # refresh private/bin
rime-copilot status       # names any missing dependency and the pip command
rime-copilot restore      # lexicon, .raw, stamp, dict.json from the vault
rime-copilot update       # rebuild the db and deploy
```

**Update the code before running `update`.** A `dict.json` carrying
`"boost": "log"` restored onto an older `load_sources` is silently ignored —
unknown keys are not an error there — so that machine builds a database with
different weights, reports it up to date, and nothing anywhere says the two
machines disagree.

To measure what a cleanup (or any lexicon/config change) actually did to
prediction quality, use the `tools/rime_corpus/` harness (`rime-corpus` CLI)
rather than eyeballing candidates by hand: it replays a recorded corpus
through a real Rime build (`tools/replay_copilot.cc`) and reports a bucket
split with an oracle bound (`rime-corpus oracle`), or exports a scoring set
for `tools/score_candidates.cc`.

**How big that corpus is decides what the harness can resolve**, and it is the
one input every tuning question waits on: at 3287 scoring units and ~70%
accuracy the 95% interval is about ±1.6 points, which cannot tell two settings
three points apart from each other. `rime-corpus ingest --since-days N` is the
lever — sources with a time window (dingtalk: 90 days by default) harvest as
far back as asked, and sources without one (claude reads every local
transcript) say on stderr that the flag did not apply rather than silently
dropping it. Widen `--max-items` with it; the per-conversation cap that suits
90 days will truncate 730.

```sh
rime-corpus ingest                                    # incremental, adapter defaults
rime-corpus ingest dingtalk --since-days 730 --max-items 5000
rime-corpus stats                                     # size, and redaction residue
```

`dump_copilot <db> --find <key> -- <cont>`
remains the quick single-key spot-check — useful to confirm a key like `的`
lost its flat plateau of fragments after a rebuild, but not a substitute for
running the corpus harness before and after a change.

Neither of those measures what one re-ranking *costs*. `tools/bench_scorer.cc`
does: it replicates `LlmScorer`'s batched shape (which lives inside the plugin,
behind a Rime engine, and so cannot be timed through it) and reports the prefill
and the scoring separately, with the process's CPU time beside them. That split
is the point — the scoring is what the p99 < 10 ms budget covers, the prefill
runs on a background worker, and the CPU/wall ratio is what says whether the
work is on the GPU at all. Use it before touching `n_gpu_layers` or `n_threads`,
and on any new machine, rather than carrying this one's numbers over:

```sh
bench_scorer --model ~/Library/Rime/private/rime40m-q8.gguf --iters 1000
bench_scorer --model ~/Library/Rime/private/rime40m-q8.gguf --iters 1000 \
             --gpu-layers 0 --threads 4
```

## Usage

* Put the db file (by default `copilot.db`) in rime user directory.
* In `*.schema.yaml`, add `copilot` to the list of `engine/processors` **before
`ascii_composer`** — not merely before `key_binder`. `AsciiComposer` returns
`kRejected` for letter keys while `ascii_mode` is true (librime
`ascii_composer.cc:129-140`) and the engine breaks the processor loop on
`kRejected` (`engine.cc:104-105`), so a `copilot` ordered after it never runs
in English mode at all. Context memory is then dead in the English-to-Chinese
direction only, which reads as flakiness rather than as misconfiguration; the
constructor logs a `LOG(WARNING)` when it detects this.
Also add `copilot_translator` to the list of `engine/translators`;
or patch the schema with:
```yaml
patch:
  'engine/processors/@before 0': copilot
  'engine/translators/@before 0': copilot_translator
```

* Add the `copilot` switch:
```yaml
switches:
  - name: copilot
    states: [ 關閉預測, 開啓預測 ]
    reset: 1
```

* Deploy and enjoy.

## Configuration

```yaml
copilot:
  # copilot db file in user directory/shared directory
  # default to 'copilot.db'
  db: copilot.db
  # max prediction candidates every time
  # default to 0, which means showing all candidates
  # you may set it the same with page_size so that period doesn't trigger next page
  max_candidates: 5
  # max continuous prediction times
  # default to 0, which means no limitation
  max_iterations: 1
  # llm model file in user directory/shared directory
  model: Qwen-3-0.6B-q4_K_M.gguf
  # max predict tokens
  n_predict: 8

  # Predict from the real text before the caret (IMK / IME Bridge) instead of
  # this session's commit history. default: true
  use_surrounding_context: true
  # How many characters before the caret to use as the lookup context.
  # default: 8, clamped to 1..64
  surrounding_context_chars: 8
  # Log which surrounding-text source answered (IMK / ImeBridge / tmux) and
  # why tmux refused, if it did. Uses LOG(INFO), not DLOG, so it is visible in
  # a release build -- see "Finding the log output" under "tmux Source".
  # default: false
  surrounding_debug: false

  # Contextual candidate re-ranking, as our own filter. Also add
  # `copilot_rerank_filter` to engine/filters. See "Contextual Re-ranking".
  rerank:
    enable: true
    max_context_chars: 8
    window: 32
    max_rank: 10

  # Disable specific sub-plugins (optional)
  disabled_plugins:
    # - ime_bridge
    # - auto_spacer
    # - select_character

  # IME Bridge configuration (for Vim mode support)
  ime_bridge:
    enable: true
    socket_path: /tmp/rime_copilot_ime.sock
    client_timeout_minutes: 30  # auto-cleanup stale clients
    context_ttl_seconds: 60  # age past which the active client's surrounding
                              # context is treated as absent; 0 disables the check
    host_id: ""  # identity announced to every client on connect, so a client
                 # reached over an ssh tunnel can tell which machine it is about
                 # to drive. Empty derives it from the hostname, truncated at the
                 # first dot -- what ssh's %L expands to. Set only if they differ.
    debug: false

  # Auto Spacer configuration
  auto_spacer:
    # Whether to add right-side space when committing text in the middle,
    # e.g. "测|试" + "test" -> "测 test 试".
    # default: true
    enable_right_space: true

  # Per-tmux-pane memory of ascii_mode (macOS only). A terminal is ONE IMK
  # client, so Squirrel gives it one Rime session and one ascii_mode shared by
  # every pane in it; this splits that variable into N. See "Context memory"
  # below, including how the plugin learns which pane you are in.
  context_memory:
    enable: false          # default false; the feature ships off
    use_pane_command: true # default true; pane_current_command is part of the
                           # identity, so a shell and `claude` in one pane hold
                           # independent modes. The cost is that a short-lived
                           # command (`git commit`, `less`) briefly makes the
                           # pane a different key.
    max_entries: 256       # default 256; LRU bound. pane ids grow monotonically
                           # as panes come and go, so on a machine that is never
                           # rebooted an unbounded table grows without limit.
    debug: false           # default false; one LOG(INFO) line per identity
                           # change. Every way this feature can fail is silent,
                           # so this line is how "broken" is told from "off".

  # tmux pane scrape (macOS only, for terminal emulators IMK can never answer
  # for; see "tmux Source" below). default: disabled
  tmux_source:
    enabled: true                    # default false; opt-in. Also the
                                     # prerequisite for context_memory's polled
                                     # rung -- without it only the tmux hook
                                     # can say which pane you are in.
    binary: /opt/homebrew/bin/tmux   # optional; probed if empty
    socket: ""                       # optional; a full socket *path* for
                                     # `tmux -S`, not a `-L` name. Empty uses
                                     # tmux's own default socket. Not the
                                     # context_memory key: that comes from
                                     # tmux's own `#{socket_path}`, and this is
                                     # only the fallback for a tmux too old to
                                     # report it.
    app_bundle_ids: []               # optional; empty = the built-in terminal
                                     # list. A non-empty list *replaces* it.
    timeout_ms: 50                   # clamped to [5, 500]
```

### Prediction Context

The db n-gram lookup keys are the last few characters before the caret:

- **Surrounding path** (`use_surrounding_context: true`, default): the text is
  read from the frontend — IMK on macOS, else an IME Bridge client, else the
  active tmux pane if `tmux_source` is on — and the word just committed is
  appended to it (the snapshot is taken before the key is handled). Because it
  follows the caret, moving to another paragraph or app immediately predicts
  from *there*, instead of from whatever was typed earlier in the session.
- **History fallback**: when no source can answer (Chrome/Electron and
  terminals often return nothing, and there is no IMK on Linux), the plugin
  falls back to its own commit history — exactly the previous behavior. Setting
  `use_surrounding_context: false` forces this path.

`surrounding_context_chars` bounds how much is read; keys longer than ~6
characters essentially never hit the db, so the default of 8 is ample. The text
is only ever used in-process. The LLM provider still prompts from commit
history and is unaffected by these settings.

### Contextual Re-ranking (filter)

Reorders the candidates of the composition you are typing so the one the db
expects to follow the text before the caret comes first: typing the syllable
for 瓴 right after 高屋建 puts 瓴 ahead of 令. Separate from the prediction
popup, which only appears with an empty input.

Add the filter to the schema, ahead of any pinning filter so pinned candidates
keep winning:

```yaml
patch:
  "engine/filters/@after 0": copilot_rerank_filter   # after copilot_filter
```

| key (`copilot/rerank/…`) | default | meaning |
|-----|---------|---------|
| `enable` | `true` | kill switch; the db is not opened when false |
| `max_context_chars` | `8` | longest context key: Han characters before the caret |
| `window` | `32` | only the first N candidates are considered |
| `max_rank` | `10` | a continuation ranked below this among its key's continuations never promotes. Measured over 8324 segments of real typing: at 50 re-ranking is net **negative** on colloquial text (24 gained, 30 lost) while positive on technical text; at 10 both are positive. Function words are the reason — 吧/的/和 follow almost every verb, so they sit in every key's continuation list at high rank, and a wide threshold lets them take first place. See `docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md`. |

How it decides:

- **Context** is the run of Han characters *touching* the caret, taken from IMK
  or the IME Bridge. Anything else ends it — CJK or ASCII punctuation, spaces,
  newlines, latin, digits, emoji — so `高屋建。` and `高屋建 ` re-rank nothing
  while `see 高屋建` still yields `高屋建`.
- **Matching** is bidirectional: a candidate matches a continuation when it
  equals it, starts it (`高屋 -> 建瓴` while only 建 is typed), or is started by
  it. Exact matches outrank partial ones; ties go to the likelier continuation.
- **The quality floor is a rank**, not a share of the key's total weight: the db
  merges dictionaries on different scales (a personal dictionary is deliberately
  lifted above the frequency-bearing one), which leaves the order meaningful but
  the ratios not. A key like 建 has ~2900 continuations, so a share threshold
  could never be met either. The db's weights must therefore be real
  frequencies, ordered "larger = more likely".
- **Promotion** moves the winner to the front, but only among candidates
  covering the same input span, so how much input Space commits never changes.
- **No surrounding text, no re-ranking.** With only commit history the plugin
  cannot tell that the caret was moved by a mouse click, and a wrong promotion
  is worse than none — so Chrome/Electron, terminals and Linux keep today's
  order.

This deliberately does not use librime's own `contextual_suggestions` /
`grammar` mechanism. That path resolves its language model through a component
registered under the name `grammar`, and librime-octagram — bundled with
Squirrel — loads after this plugin and replaces it (`Registry::Register`
overwrites on a name collision; look for `replacing previously registered
component: grammar` in Rime's WARNING log). Owning the filter avoids that
fight, and lets the context come from the real text before the caret rather
than librime's "last commit only".

### LLM Re-ranking

Runs inside the same `copilot_rerank_filter` above, as its primary scoring
source rather than a second filter: when a model is configured, the filter
scores candidates by summed log-probability instead of looking up db
continuations, and the db loop above runs only as the fallback for when the
LLM path can't (disabled, on battery, model missing, or the warm cache is
cold — see `skip` in Telemetry below). The two never both act on one
segment.

**Off by default, and why:** the model sits resident at roughly **1 GB** and
runs continuous CPU work while the input method is open, not just per
keystroke. Turning it on is a deliberate trade a user makes for their own
machine, not a default this plugin should set for everyone.

To turn it on (`enable` defaults to `false` in code — this block is what you
write to opt in, not what you get):

```yaml
copilot:
  rerank:
    llm:
      enable: true   # the default is false
      model: private/Qwen3-0.6B-q4_K_M.gguf
      battery_active: true
      top_n: 4
      margin: 2.0
      length_exponent: 0.7
      # n_gpu_layers: 99   # 0 runs on the CPU instead; measure, do not guess
      # n_threads: 0       # 0 = hardware_concurrency(); only bites if the above is 0
```

Fetch the exact build the numbers below were measured against with:

```sh
tools/rime-copilot fetch-model
```

It downloads into `<rime_dir>/private/`, checks the download's size against a
sanity floor (so a truncated connection or an HTML error page can't silently
become "the model"), and prints the config block to paste. `--url`/`--name`
point it at a different quant or destination filename; `--force` re-downloads
over an existing file.

| key (`copilot/rerank/llm/…`) | default | meaning |
|-----|---------|---------|
| `enable` | `false` | kill switch; off by default (see above) |
| `model` | *(empty)* | path to a `.gguf`, relative to the Rime user directory — `fetch-model` prints the right value. Nothing loads, and the LLM path silently falls back to the db, until this is set to an existing file. |
| `battery_active` | `false` | run on battery too. The default was set on the assumption that "the model's CPU cost is exactly the kind of thing a laptop should shed on battery", mirroring `copilot/llm/battery_active`. **Measured, that assumption does not hold for this model** — one prefill-plus-scoring costs ~3.9 ms of GPU time and ~1.9 ms of CPU on an M4 (`tools/bench_scorer.cc`), so even a dense 1000 commits an hour is ~0.012 Wh/h against a laptop's 8-15 W: about 0.1% of consumption, and the GPU is already awake rendering the candidate window at exactly those moments. The default stays `false` only because it is the shipped behaviour; `true` is the recommendation. |
| `n_gpu_layers` | `99` | layers offloaded to Metal/CUDA; `0` runs entirely on the CPU. Measured on an M4 the default is right on **core time, not latency**: 1.93 ms of CPU per prefill-plus-scoring against 14.79 on four CPU threads, 7.7x. Latency does not choose — CPU-only is actually a shade faster on score (p50 1.89 ms against 2.55, p99 4.36 against 5.47) and the background prefill is a tie (2.57 against 2.76), with both arms well inside the p99 < 10 ms budget. So CPU-only is the right setting on a machine with no usable GPU and not otherwise. Re-measure with `bench_scorer` rather than guessing; the table is in that file's header. |
| `n_threads` | `0` (= `hardware_concurrency()`) | measured **inert** while the layers are on the GPU (10, 4 and 2 all land on score p50 2.53-2.55 ms), and decisive once they are not. On a 4-P-core M4, `n_gpu_layers: 0` with 8 threads is worse than with 4 on *both* latency and core time (29.34 ms/iteration against 14.79), because the extra threads land on efficiency cores — and `hardware_concurrency()` there is 10. Leave at `0` unless `n_gpu_layers` is `0`, then set the performance-core count. |
| `top_n` | `4` | how many candidates are scored. Measured over 8324 segments: **4 beats both fewer and more, on speed *and* accuracy** — 58.4% hit / 8.1% harmful false-promotion / 14.4 ms at top_n=4, vs 56.9% / 13.1% / 114.9 ms at top_n=32. A longer list gives the model more ways to be wrong; 72.6% of correct answers sit at rank 2-3 anyway. See `docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md`. |
| `margin` | `2.0` | a challenger must beat the incumbent's score by at least this much to promote. Sweeping this threshold (same doc): at 2.0, harmful false promotion drops to roughly a quarter of the no-threshold rate (8.1%→3.2% technical, 12.7%→6.5% colloquial) for a net-gain cost of under a point. Above 5 the curve collapses — the threshold starts rejecting correct promotions faster than wrong ones. |
| `require_han_context` | `true` | whether a segment needs a trailing **Han** context before the model may be consulted. That is a db constraint — the trailing Han run is the only thing an n-gram can key on — and the model does not share it: it reads punctuation and Latin. Measured over 27245 segments (2026-08-21): the gate blocks **42.9%** of them, `noctx` being 16322 of 27245 (59.9%) — exactly the request count, i.e. it blocks every request's segment 0 and nothing else. Lifting it is **safe** (the 2026-08 revert's bucket B is now 4 segments, `b64ef1c` having moved the raw-input head out of the way) and **changes almost nothing**: GAIN 44 / LOSS 31 of 27245, McNemar p=0.17. The default stays `true` on **cost** — lifting it is 2.07x the scorings for no measurable gain. Settled, not deferred. See `docs/superpowers/specs/2026-08-21-rerank-cost-and-gate-results.md` and `BailOnEmptyDbContext` in `src/rerank_llm.h`. |
| `length_exponent` | `0.7` | candidates are scored by `logprob / n_tokens^length_exponent`, not raw summed log-probability. This one setting dominates every other tuning knob measured: 56.9% hit / 13.1% harmful FP at 0.7 vs 43.3% / 15.4% for the raw sum and 52.4% / 12.1% for dividing by the mean. |

**Latency: measured once, small sample, telemetry will settle it.** The
design targets keeping this off the keystroke path — warm the context ahead
of typing, and pay only the ~4-candidate scoring cost per keystroke. The
integration measurement (`docs/superpowers/specs/2026-08-16-llm-rerank-poc-results.md`,
n=20 scored segments) came in at **p50 ≈ 29 ms, p90 ≈ 56-70 ms** — roughly
2x the PoC's isolated 14.4 ms and over budget on this hardware. That is one
run at a small sample size, not a verdict either way: Telemetry below writes
`us_p50`/`us_p95` into the stats line specifically so this settles from real
use instead of staying an offline guess. Do not read either the PoC's 14 ms
or the integration's 29-70 ms as the number — check your own telemetry.

### Telemetry

Records contextual re-ranking decisions and non-first candidate selections, so
ranking changes can be driven by data rather than impressions. Defaults on.

```yaml
copilot:
  telemetry:
    enable: true
    top_n: 5                  # candidates recorded per event, 1-20
    max_file_bytes: 8388608   # rotate past this size
    keep_generations: 2       # counts the live file, so 2 = live + one archive
    sample_ok: 0              # keep 1 in N ordinary successes; 0 = none (default)
    auto_sync: false          # copy into sync_dir every 30 min; see Privacy below
```

`sample_ok` puts ordinary successes back into the log at a known rate — an
eval set built from real typing needs those as well as the hard cases the log
otherwise keeps exclusively. It does not affect first-candidate accuracy:
misses are recorded in full regardless of this setting, so that number needs
neither sampling nor scaling.

`auto_sync` does from inside the plugin what `tools/sync_telemetry.sh` does by
hand: every 30 minutes, and once at session end, this machine's file and its
archives are copied whole into `<sync_dir>/copilot_telemetry/`. It is off by
default, and it is the same privacy decision the script is — read Privacy
below before turning it on. It is not a thread and not a network call: a
whole-file copy of a file that measured 8.5 KB over two days, on the same
`OnCommit` the stats flush already piggybacks on. The append itself stays
local; only the periodic copy touches the sync directory, which is why
appending-inside-iCloud (the script's stated reason for not doing this) does
not apply. With `sync_dir` unset — Squirrel never writes one — it logs one
`ERROR` naming what to add to `installation.yaml`, rather than doing nothing
quietly.

One file per machine at
`<rime user dir>/private/copilot_telemetry/<installation_id>.jsonl`, created `0600`.
Because no two machines write the same file, merging several machines' data is
concatenation — no deduplication and no conflict resolution. These settings are
process-wide, taken from whichever schema loads first: setting them differently
per schema does not work, and a later schema whose settings are ignored gets a
one-time `WARNING` in the log rather than a silently empty file.

Telemetry is governed only by `telemetry/enable`, not by the `copilot` switch:
turning the plugin off in the switch bar stops prediction but recording
continues, because the re-ranking filter and the commit hook do not consult that
switch. Set `enable: false` to stop recording.

**What is in it.** Each line records the segment's input code, the selected
candidate, the first `top_n` candidates, and — when re-ranking fired — which
context key it used, what it promoted, from which position, and at what rank and
match level. Field-by-field meaning is in
`docs/superpowers/specs/2026-08-14-prediction-telemetry-design.md`.

When LLM re-ranking is on, an event also carries an `llm` object whenever the
model was actually consulted for that segment: what it promoted (if
anything), the incumbent it was compared against, the margin between them,
how many candidates were scored, and how long scoring took. Separately, once
per flush interval, a `{"type":"stats", ...}` line aggregates every segment
`StatsAccumulator` saw — not just the hard cases the per-event stream keeps —
so rates like warm-hit can be computed against the true denominator instead
of the hard-cases-only one. Its `skip_counts` names, for every segment the
LLM path didn't act on, which of `disabled`/`battery`/`nomodel`/`noctx`/
`cold`/`nohan`/`margin` stopped it. There is no stored `warm_hit` counter —
under the fallback chain as implemented, a segment is never scored while the
warm cache is cold, so warm-hit rate is derived as
`llm_acted / (llm_acted + skip_counts["cold"])`, which `analyze_telemetry.py`
computes for you.

The stats line also carries `trunc_counts` — why the surrounding fetch stopped
(`full`/`config`/`app`/`screen`/`unknown`), over every segment — and
`depth_p50`/`depth_p95`, how deep it got, over **only** the fetches the
environment cut short (`screen`/`app`). The other three truncations carry a
depth that is not a limit: `config`'s is the configured cap, `full`'s is an
input region that ended on its own, and `unknown`'s is whatever a source that
cannot say returned. The depth pair is omitted entirely when a window saw no
such fetch, so a reported `0` always means the source really reached nothing.

Every Event carries the same two facts per segment (`trunc`, `before_depth`),
and those are the ones to join back to a specific commit — but the event
stream is sampled, so a `screen` share computed from it is biased toward short
requests, which are exactly the ones most likely to be truncated. Quote the
stats line for the share; quote the events only for shape.

**Privacy.** The `ctx` field is Han characters only, by construction:
`TrailingCjkRun` stops at the first non-Han character, so an ASCII secret on
screen yields an empty context and re-ranking does not even run. But the
selected and candidate texts are the user's own Chinese input, which makes this
file a chronological transcript of it — unlike Rime's `*.userdb`, which is a
frequency table with no timeline. The plugin makes no network connections;
copying the file out of the machine's own directory is a separate, opt-in step
— `tools/sync_telemetry.sh` by hand, or `auto_sync: true` on a timer. Be clear
about what either does, though: it puts the transcript into the iCloud sync
directory, from where it is uploaded and lands on every machine signed into the
account. `auto_sync` keeps doing it until you turn it off, which is exactly its
value (a stale copy is a wasted week of typing) and exactly its cost — so turn
it on only when you accept that, and turn it off when you have finished
analysing. Set `enable: false` to stop
recording, and delete the directory to discard what was already recorded.

The local file lives under `private/` because a Rime user directory is
commonly a git repo of the user's own config layered on top of upstream, and
`private/` is the conventional gitignore line for such a repo — keeping the
transcript there means it cannot be committed by accident. The sync
directory is not a git repo, so `sync_telemetry.sh` copies it out flat.

**Analysing it.** On a single machine, read the live directory directly:

```sh
tools/analyze_telemetry.py ~/Library/Rime/private/copilot_telemetry/*.jsonl
```

Across machines, copy each machine's file into the shared directory first.
`sync_telemetry.sh` resolves `sync_dir` from `installation.yaml` and prints the
report command for it, path already filled in:

```sh
tools/sync_telemetry.sh                       # on each machine; prints the next command
```

Or set `auto_sync: true` on each machine and let the plugin keep the shared
directory current — the same destination, so the report command above is
unchanged.

The report is organised by the claim each section tests, from the design spec.
A rejection rate that is flat across a section's buckets refutes that section's
claim — which is the point. Python 3 standard library only; no dependencies.

Two more sections read the v2 fields above (both degrade gracefully on
older files that have neither):

- **LLM decision quality** — acceptance rate bucketed by margin, the live
  counterpart of the offline sweep that set `margin: 2.0` above. Every
  recorded promotion already cleared whatever margin the writing machine was
  configured with, so this says what *raising* the threshold further would
  cost or buy, not what a lower one would have done.
- **Skip-reason distribution** — from the stats lines: what share of
  segments the LLM path acted on, the derived warm-hit rate, and a
  breakdown of every skip reason. If `cold` dominates, the fix is the
  warming trigger, not the model.

A v1 line — no `llm`, no `type` — loads exactly as it always has; the two
new sections just report "no data yet" against a file that has none.

**Filters after `copilot_rerank_filter` invalidate events.** What re-ranking
promoted is captured inside the filter; what the user selected is read at
commit, from the candidate list as displayed. Any filter listed after
`copilot_rerank_filter` in `engine/filters` sits between those two moments, and
if it rewrites candidate text or reorders the list, the two are no longer
comparable — the event looks like a rejected promotion whether or not the user
rejected anything. Three common ones do exactly this:

| Filter | What it does to an event |
| --- | --- |
| `simplifier@traditionalize` | Rewrites every candidate while the 繁 switch is on, so *every* event looks rejected |
| `pin_cand_filter` | Reorders candidates, so `rr.from` and `sel_idx` index different lists |
| `uniquifier` | Removes candidates, shifting the indices below |

`analyze_telemetry.py` detects this without knowing any of their names: if
re-ranking put a candidate first, the displayed list must start with it, so
`top[0] != rr.text` means the head was altered downstream. Those events are
excluded from every claim table and counted in the OVERALL section as
`EXCLUDED, head altered downstream`, with a warning when they are more than 10%
of the data. Check that number before trusting a rejection rate — if it is
large, the tables rest on a small remainder of your typing.

The LLM decision-quality section applies the identical check against
`llm.text` instead of `rr.text`, tracked separately since the two paths never
both act on one segment — a `top[0] != llm.text` event is excluded from the
LLM table without affecting `EXCLUDED, head altered downstream` above, which
stays scoped to the db path exactly as it always was.

Note that the filter order is a deliberate choice, not a mistake: re-ranking is
placed ahead of pinning so that pinned candidates win. To collect a clean
sample, turn the offending filters off for the duration rather than reordering
the chain.

### Auto Spacer Notes

- Right-side spacing can be disabled with `copilot/auto_spacer/enable_right_space: false`.
- Backtick `` ` `` is excluded from punctuation-triggered spacing before Chinese.
- Number-key candidate commit supports ASCII detection: ASCII candidates use English spacing rules.

### Auto Spacer Logic

- Two processing paths:
  - `surrounding` path (preferred): uses real `before/after` context from IMK or IME Bridge.
  - `history` path (fallback): uses `commit_history` when surrounding context is unavailable.
- ASCII mode:
  - Typing ASCII letters/numbers after Chinese or selected right punctuation inserts a leading space.
- Non-ASCII mode:
  - During composition, it does not insert spaces.
  - On commit keys (`Space`/`Enter`/number selection), it decorates committed text using boundary context:
    - add left space when needed (`中文|English` boundary),
    - add right space when needed (`English|中文` boundary, controlled by `enable_right_space`).
- Chinese punctuation never gets auto-surrounded with spaces.

### tmux Source

Terminal emulators built on winit (Alacritty, and every other winit app)
hardcode `selectedRange = NSNotFound`, so the IMK query in `imk_client.mm` can
never answer for them. `copilot/tmux_source` scrapes the active tmux pane
instead, from outside any pane, so it works no matter which winit terminal
is in front — as long as you are inside a tmux session there.

**macOS only in practice.** The gate below keys on the frontmost
application's bundle id, which only exists on Apple; on Linux the source
compiles but the gate can never pass, by design. Enabling it there does
nothing.

It is opt-in (`enabled: false` by default) because it reaches into another
process and scrapes a screen.

#### The application gate

`app_bundle_ids` decides which frontmost applications may trigger the scrape
at all. Left empty it uses the built-in list, which covers ten terminals —
Alacritty, kitty, WezTerm, Apple Terminal, iTerm2, Ghostty, Hyper, Warp, Rio,
Tabby.

A non-empty `app_bundle_ids` **replaces** the built-in list rather than
intersecting with it. The built-in list is there so the *default* is safe, not
to forbid a deliberate override: editing your schema YAML is a considered act,
and intersecting would lock you out of terminals we simply never listed —
custom builds, forks, emulators released after this was written. Whatever you
put in the list is what gets the scrape, and you own that choice.

Note what that means in the other direction. VS Code and other Electron
editors are off the built-in list **on purpose**: their integrated terminals
would benefit, but their editor panes also answer `NSNotFound`, so adding
`com.microsoft.VSCode` hands your Monaco buffer the tmux pane's text too.

#### Multiple attached clients need `focus-events on`

With more than one client attached to the same tmux server, tmux answers for
the most recently active one. That tracks which macOS window you are actually
typing in only because `focus-events on` makes tmux enable DECSET 1004, so
focusing a window writes a focus sequence to that client's tty and bumps its
`client_activity`. **`focus-events` is off by default.** Without it the "most
recent" client is merely whichever one was last typed into, and tmux would
answer confidently from the wrong pane.

So the source reads `focus-events` (in the same single `tmux` invocation, no
extra process) and refuses whenever more than one client is attached without
it. If you use more than one attached client, put this in your `~/.tmux.conf`:

```tmux
set -g focus-events on
```

A single attached client is unaffected — there is nothing for tmux to pick
between.

#### Priority and refusals

This source ranks below ImeBridge in the surrounding-text chain (see
`surrounding_source.h`), so the Neovim client keeps winning while it is in
insert mode, with no nvim-side configuration change needed.

The source refuses to answer — falling back to `commit_history` — whenever the
answer would be a guess:

- the frontmost app is not on the gate list;
- **no** client is attached to the tmux server (a detached session would still
  answer, from a pane nobody is looking at);
- more than one client is attached and `focus-events` is off;
- two attached clients are tied on `client_activity`, which has one-second
  granularity;
- tmux cannot be reached, times out, or answers something unparseable.

#### Ghost text is not the buffer

A TUI draws things at the caret that are not in its edit buffer at all: codex
paints a dim suggestion into an empty composer, fish and zsh-autosuggestions
paint an inline completion, and both vanish the moment you type. A screen
scrape sees only characters, so those used to arrive as the `after` boundary —
and every first CJK commit into an empty codex composer came out with a
trailing space, because `after` was the placeholder's first letter.

So the pane is captured with `capture-pane -e`, which keeps the SGR sequences,
and `after` is dropped when the caret cell is drawn **dimmer** than the cell
immediately left of it (on a wrapped line, the last cell of the row above — the
same cell `before` ends with). Ghost text starts a new, muted run of styling at
exactly the caret, and that is the one signal available that those characters
are not the user's.

"Dimmer" is measured, not guessed:

| | at the caret |
| --- | --- |
| codex placeholder | `ESC[2m` — faint |
| zsh-autosuggestions / fish | `fg=8`, a 256-colour grey |
| vim syntax highlighting | `31`, `34`, `35`, `38;5;130` — saturated hues, never faint, never grey |

Refusing on *any* style change would also refuse at every syntax-highlighting
boundary that happened to fall under the caret, costing a trailing space in an
editor for nothing. Refusing only on de-emphasis — the faint attribute, or a
switch to bright black or a greyscale foreground — separates the two. It is a
rendering rule, not an app list: nothing here knows about codex or about any
particular shell.

What is left is narrow: a colourscheme that renders comments grey, with the
caret landing exactly on the comment's first character, loses one trailing
space. (vim's own default does not — it colours comments blue.) That direction
is the safe one; trusting instead would insert a space into text the user never
wrote.

It costs nothing measurable. `-e` makes tmux's answer about 40% larger (a few
hundred bytes), and parsing a pane went from ~1.3µs to ~5.3µs worst case — a
full screen of highlighted code. The `tmux` fork/exec around it is ~3ms, so the
whole check is under 0.2% of one query, and the query runs once per key event.
Getting there needed two things that are easy to undo, so: escapes are decoded
only for the caret row and the row above it (`want_styles`), and a row with no
escape byte at all skips the parse entirely. Without those the same parse takes
~24µs, mostly in per-character allocation.

#### Finding the log output

Set `copilot/surrounding_debug: true` to see which source actually answered
(`Using IMK context` / `Using ImeBridge context` / `Using tmux context`) and,
when tmux refused, why (no tmux binary found, query timed out, output
unparseable, refused per the list above). It uses `LOG(INFO)`, not `DLOG`, so —
unlike the plugin's other debug logging — it is visible in the release build
Squirrel actually ships: `DLOG` compiles out entirely under `-DNDEBUG`.

This plugin statically links its own copy of glog, separate from librime's —
librime doesn't export its glog symbols, so the plugin can't share it — and
initializes that copy itself (`rime_copilot_initialize()` in
`src/copilot_module.cc`). As a result, **plugin log output never lands in
Squirrel's `rime.squirrel.*` log**, no matter what you set `min_log_level` or
any other Squirrel logging setting to. Look for the plugin's own file instead:
`$TMPDIR/rime_copilot.*.log.INFO.*.log`.

#### Two things to know about the socket

`socket` is passed to `tmux -S`, so it is a full filesystem **path** to the
server socket, not a `-L` short name. Leave it empty to use tmux's default.

And tmux's default socket lives under `$TMUX_TMPDIR`. If you set that in your
shell rc, your servers are somewhere the IME cannot find: the IME process
inherits Squirrel's environment, not your login shell's. Set `socket` to the
absolute path in that case (`tmux display-message -p '#{socket_path}'` from
inside a session prints it).

#### Known limits

- **Soft-wrapped context reaches back only one row.** When the caret sits on a
  continuation row, `before` is the text on that row (plus the row above only
  when that row fills the pane exactly). So with `surrounding_context_chars: 8`
  and the caret at column 3 of a continuation row, prediction sees 3 columns,
  not 8. AutoSpacer only ever needs the boundary character, so this costs
  prediction depth, not spacing correctness.
- **An allow-listed terminal that is not running tmux still reads the active
  tmux pane.** Focus kitty at a bare shell while Alacritty holds the tmux
  session and kitty gets Alacritty's context. Telling them apart needs the
  window title, which costs a Screen Recording or Accessibility permission.
  Narrow `app_bundle_ids` to the one terminal you actually run tmux in if this
  bothers you.

## IME Bridge

IME Bridge allows external editors to control Rime's `ascii_mode` via Unix Domain Socket.

### Client Libraries

- **Neovim**: [clients/neovim](clients/neovim/)

### Protocol

JSON Lines format:
```json
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"set","ascii":true}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"restore"}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"reset","restore":true}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"unregister"}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"activate"}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"context","before":"测","after":"试"}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"clear_context"}}
{"v":1,"ns":"rime.ime","type":"ascii","src":{"app":"nvim","instance":"12345"},"data":{"action":"deactivate"}}
```

### The greeting

The server speaks first, and only once: the instant it accepts a connection it
writes one line naming the machine it is running on, before the client has sent
anything.

```json
{"v":1,"ns":"rime.ime","type":"hello","data":{"host":"my-laptop"}}
```

Past that line the protocol is one-way as before, and a client is free to ignore
it — everything below works exactly the same whether or not it is read.

It exists for clients reached over an ssh tunnel. Sign into one remote account
from two laptops and each has a tunnel to a *different* IME, both equally
reachable and indistinguishable from the far end; the greeting is what lets a
client keep the one that leads home and close the rest unread. `host` defaults to
this machine's hostname truncated at the first dot — the value ssh's `%L`
expands to, so a remote client can compare it against `$LC_RIME_IME_HOST` — and
is overridable via `copilot/ime_bridge/host_id`.

Reading the greeting and hanging up registers nothing: client state is created
only by the actions below, so probing a connection cannot disturb whoever is
using it.

### Actions

| Action | Description |
|--------|-------------|
| `set` | Set `ascii_mode`. Params: `ascii` (bool), `stack` (bool, default true). `stack=false` sets mode without affecting restore stack. |
| `restore` | Restore to previous state (supports nested calls) |
| `reset` | Clear state and optionally restore original mode |
| `unregister` | Remove client registration (on exit) |
| `activate` | Mark this client as active context owner |
| `deactivate` | Clear active ownership for this client |
| `context` | Push surrounding text. `before` may hold several characters (its last one is the spacing boundary, the whole run is the prediction context); `after` is a single character |
| `clear_context` | Clear stored surrounding text for this client |
| `ping` | Health check |

#### Context memory: telling the plugin which pane you are in

`copilot/context_memory` can remember `ascii_mode` per tmux pane. **It ships
off** — set `copilot/context_memory/enable: true` to turn it on (see
[Configuration](#configuration) for the rest of the keys). Switched on, it
learns which pane you are in two ways.

**The hooks buy no speed, and that is worth settling before you install
them.** With `copilot/tmux_source/enabled` on — and it has to be for
auto-spacing to work in a winit terminal like Alacritty — `AutoSpacer::Process`
queries tmux on every non-composing keystroke regardless of this feature. The
polled rung therefore already costs **zero** extra, and the pushed rung removes
no `posix_spawn` at all. Measured on the machine where the hooks were first
installed: the pushed rung took over (`via bridge` in the `ctxmem` log) and the
per-keystroke spawn was still there, because `GetSurroundingContext`'s third
priority is unconditional. An earlier draft of this section said the hooks
"buy nothing"; that was right about cost and wrong about the reason to want
them.

What they buy is **correctness in two places the polled rung cannot reach**:

- The pushed value is exact and immediate. The polled one reads a memoized
  snapshot behind a frontmost-app gate and declines outright when several tmux
  clients are attached indistinguishably.
- It is the only rung that could ever carry an identity from a **remote** tmux.
  The polled one sees `ssh` as the pane's command and nothing behind it, so
  every pane on a given remote collapses into one memory slot.

They would additionally become the only way to keep this feature without a
per-keystroke query if AutoSpacer ever stopped fetching unconditionally.

**Pushed.** tmux already knows when the pane changed, so it does not have to be
asked:

```tmux
set-hook -ga after-select-pane   'run-shell -b "~/Library/Rime/private/bin/rime_ctx_report.sh \"#{pane_id}\" \"#{pane_current_command}\" \"#{socket_path}\""'
set-hook -ga after-select-window 'run-shell -b "~/Library/Rime/private/bin/rime_ctx_report.sh \"#{pane_id}\" \"#{pane_current_command}\" \"#{socket_path}\""'
```

`-ga`, not `-g`: **`-g` replaces** any existing hook of that name, so a config
that already hooks `after-select-pane` loses it silently. `-ga` appends.

**The three format arguments are not optional decoration.** tmux expands
`#{pane_id}` in a hook's command against the *hook's own target*. The script's
fallback — `tmux display-message -p` with no `-t` — resolves against the
**invoking client** instead, which is a different thing whenever a pane switch
is issued from somewhere else. Measured on tmux 3.7c, 2026-08-31: a
`select-window -t copilot:3` run from a pane in another session fired the hook,
and the hook's own `display-message` answered `librime:1 %3 claude` rather than
`%5`. Plain `run-shell` did it too, so it is not a race with `-b`. Switching
panes with the prefix key is correct only by accident — there the invoking
client *is* the client being switched.

Without the arguments the reporter still works and is still right for every
interactive switch, so the failure is invisible: a plausible key, no error, no
log line. `rime-copilot status` names a hook that omits them.

`#{socket_path}` is the least necessary of the three — the hook's environment
carries the hook target's own `$TMUX`, from which the script derives the same
path, verified in the same session. It is passed anyway so that both identity
rungs read literally the same tmux format, and the `$TMUX` derivation stays as
the fallback for a tmux too old to report `#{socket_path}`.

That path is where `rime-copilot install` puts the script, and naming it
rather than a path inside a git checkout is the point: move or delete the
checkout and a hook naming it stops firing, silently. `rime-copilot status`
checks the script the hook names is actually there.

The script writes one line and exits:

```json
{"v":1,"ns":"rime.ime","type":"identity","data":{"socket":"/tmp/tmux-501/default","pane":"%7","command":"claude"}}
```

`socket` is the whole socket path (`${TMUX%%,*}`), because the polled rung
below keys on tmux's own `#{socket_path}` and the two must build the same key
for the same pane — otherwise a pane gets two memory slots and its mode comes
back from whichever rung answered last.

`type: "identity"` is **not** an `ascii` message and deliberately registers no
bridge client. The reporter connects and disconnects on every pane switch; a
registered client would have a reset synthesized on each disconnect and flip
`ascii_mode` on a machine nobody is typing on.

**Polled (needs `copilot/tmux_source/enabled: true`).** Without the hooks the
plugin reads the pane from the tmux snapshot it takes for surrounding text —
but that snapshot is itself opt-in, and with stock config there is none, so
this rung is dead and the feature never acts.

With it on, the cost is **not** a tmux query per keystroke, which an earlier
draft of this section claimed. `AutoSpacer::Process` already calls
`GetSurroundingContext()` unconditionally, so that query happens anyway, and
the identity rides the same memoized snapshot — one `posix_spawn` per key
event, exactly as before this feature existed. `IdentityAndSurroundingShareOneSpawn`
in `test/tmux_source_test.cc` is what pins that, and it is the whole cost
argument for this rung: if it ever goes red, this paragraph is wrong.

That zero is inherited, not structural. It holds only while AutoSpacer fetches
unconditionally. `rime-copilot status` reports which rung is in effect.

**Known limit: with the hooks installed, a macOS window switch is invisible.**
This is the one case where adding the hooks makes behaviour *less* correct
than polling, and it is worth knowing before you add them.

The pushed value answers whenever a terminal is frontmost and the cell is
non-empty, so it bypasses every refusal the polled rung makes — including its
refusal when several tmux clients are attached indistinguishably. Two terminal
*windows* attached to one tmux server is exactly that case: selecting a pane in
window A fires the hook, and then switching macOS windows to B **fires no tmux
hook at all**, so the cell still names A's pane and B's mode is recorded into
A's slot.

Not fixed rather than not noticed. Validating the pushed value needs the very
query rung 1 exists to avoid, and a staleness window does not help either — the
cell is fresh, it is simply wrong.

**A pane whose program drives `ascii_mode` itself is excluded, by design.**
Run Neovim with the `rime_ime` client in a pane and that client sets the mode
on every normal/insert transition. On such a keystroke the tail step sees
`ImeBridgeServer::applied_mode_writes()` move and records **nothing** —
`[ctxmem] tail %1|nvim NOT recorded (bridge wrote)` with `debug` on. The
bridge still wins the mode; it just does not get to name a pane. Recording
there would memorise a value that belongs to the editor's current mode rather
than to the pane, turning a one-keystroke misapplication into a remembered
one — and the pane would then fight the editor on every return to it.

The practical consequence is that such a pane is the wrong place to test this
feature from. Use two panes running a plain shell.

### Multi-Client Behavior

- IME Bridge handles multiple clients concurrently.
- Surrounding context is resolved from the explicitly active client (`activate/deactivate`), not by timeout heuristics.

Neovim on a remote host can drive the local IME over an ssh reverse tunnel —
see [clients/neovim/README.md](clients/neovim/README.md#remote-neovim-over-ssh).
The wire protocol is unchanged; the client just dials a forwarded socket.

## Restoring on a new Mac

`rime-copilot install` makes the pipeline run from `~/Library/Rime` alone,
with no librime checkout present — that is a hard requirement, not a
convenience: restoring a machine must not require cloning and building
librime first just to run `restore`. Three steps still come first, once per
machine, because `build_copilot` is a compiled, per-architecture artifact and
can only come from a build tree — it is deliberately *not* in the vault:

1. Install Squirrel.
2. Clone librime, put this repository at `plugins/copilot`, and build it —
   this produces `build/plugins/copilot/bin/build_copilot`.
3. Clone your Rime configuration into `~/Library/Rime` and let Squirrel deploy
   once, so `installation.yaml` exists. Then **add `sync_dir` to it by
   hand** — Squirrel never writes that key itself, it only ever reads one if
   present (verified against librime's `deployment_tasks.cc`: the write-back
   after deploy sets `installation_id`/`install_time`/`update_time`/
   `distribution_*`/`rime_version`, never `sync_dir`). Add a line such as:
   ```yaml
   sync_dir: "/Users/you/Library/Mobile Documents/com~apple~CloudDocs/RimeSync"
   ```
   pointing at wherever you keep, or want to create, the iCloud-synced vault
   parent directory. **Write that path down somewhere durable outside this
   repo** — a password manager note, a text file in your dotfiles, anything
   that survives a wiped disk. `installation.yaml` is deliberately never
   itself vaulted (it carries machine identity, `installation_id`), so
   nothing else records where the vault lives; if you lose this path on a
   fresh Mac there is nothing to grep for it. If you forget, `backup` and
   `restore` fail with a message that reprints this same instruction — but
   only once you already know the vault's *contents* are still sitting in
   iCloud somewhere.

Then, from the checkout, install the CLI once:

```sh
tools/rime-copilot install   # copies the CLI + build_copilot into private/bin
```

It pins an interpreter into the installed entry point's shebang, taking it
from `private/.python-version` if that names a pyenv environment. Check the
`python …` line it prints against the environment that actually has
`pypinyin`, `jieba`, `requests`, and `beautifulsoup4`; pass `--python` to
override it. See "Which interpreter the installed copy runs under" below.

After that, `~/Library/Rime/private/bin/rime-copilot` works on its own —
`restore` and `update` no longer need the librime checkout at all:

```sh
~/Library/Rime/private/bin/rime-copilot restore   # iCloud vault -> ~/Library/Rime
~/Library/Rime/private/bin/rime-copilot update     # fetch dictionaries, rebuild the db, reload
```

`restore` never overwrites a local file whose content the vault does not know
about — that is a `conflict`, and it is the only thing that exits non-zero;
`--force` resolves it in the vault's favour. A file the vault does not have at
all (`missing-in-vault`) is reported but does not block: `backup` silently
skips a file you don't have locally, so the vault legitimately never getting
it is not an error, and — on a new Mac — it is also what an unmaterialized
iCloud file looks like before it has been pulled down; either way there is
nothing `--force` could fix, so `restore` says so and moves on rather than
failing forever.

What is *not* in the vault, deliberately: `installation.yaml` and `user.yaml`
(machine identity — Rime's sync keys on `installation_id`), the userdb
directories (Rime syncs those itself), `build_copilot` (per-architecture,
see above), and anything the pipeline rebuilds.

### Keeping the installed copy honest

An installed copy is a second copy of `tools/rime_copilot/`, and copies are
exactly what rotted the original, unversioned `private/bin/` this replaced —
a hand-copied script, a year-old binary, a rewrite nobody remembered made.
`install` cannot make people careful instead; it records what it installed
(`private/bin/.installed.json`: source commit, source path, content hash per
file) so `rime-copilot status` can report drift every time it runs — a file
edited in place, deleted, or one the repo has since changed. Re-run `install`
from the checkout to bring the installed copy back in sync; `install` itself
refuses to run from anything that is not a checkout, so an installed copy can
never become the source for another install.

### Which interpreter the installed copy runs under

The pipeline has four third-party dependencies, all imported lazily so that
the CLI starts on a stock interpreter: `pypinyin` (needed by `build` and
`update` for any dictionary with no pinyin column — `tencent.dict.yaml` is
the common case, `word⇥weight` only, so every entry needs a generated
pinyin), `jieba` (needed by `clean`, to tell a real word from a Sogou
sentence fragment), and `requests` + `beautifulsoup4` (needed by `fetch`).
Lazy imports are why `status`, `restore`, and `backup` work on anything; they
are also why a missing dependency shows up as an `ImportError` halfway through
a subcommand, on the machine whose whole point is to have no checkout to read.

`install` pins one interpreter for all of that: it rewrites
`private/bin/rime-copilot`'s shebang to an absolute interpreter path instead
of copying the checkout's `#!/usr/bin/env python3` through verbatim. That
rewrite is what makes the installed copy work standalone — a bare
`#!/usr/bin/env python3` would *not* have been enough even with
`private/.python-version` pinning a pyenv environment that has the
dependencies: pyenv resolves `.python-version` from the *caller's* current
working directory, not the script's location, so `rime-copilot` run from
anywhere other than `private/` would silently fall back to the global
interpreter and lose them.

Which interpreter it pins is decided in this order:

1. `--python`, when you pass it.
2. **The destination's own `.python-version`** — `install` reads the nearest
   one at or above `private/bin` (so `private/.python-version` is the natural
   place) and asks `pyenv which python3` to turn the name into a path.
3. The interpreter that ran `install` (`sys.executable`).

The third is the one that must not be first, and it used to be. Under pyenv,
`python3` is a shim resolving `.python-version` from the *caller's* current
directory — so running `install` from the checkout pinned whatever
environment some parent of the *checkout* happened to name, an environment
chosen for an unrelated project with no reason to have this pipeline's
dependencies. `private/.python-version` is the declaration that is actually
about this installation, it lives next to it, and it says the same thing no
matter where `install` was invoked from.

Two details that are easy to get backwards, both load-bearing:

- The interpreter path is taken **as given, never `resolve()`d**. A
  virtualenv's `bin/python3` is a symlink to its base interpreter — the one
  interpreter that does *not* have the environment's packages.
- pyenv searches from `${PYENV_DIR:-$PWD}`, and `$PWD` in a child process
  comes from the inherited environment variable, which setting a subprocess
  `cwd` does **not** update. `PYENV_DIR` is set explicitly for that call;
  without it the answer came from wherever `install` was run while the
  reported version came from the destination — a wrong interpreter wearing a
  correct-looking label.

A declaration naming a version pyenv cannot resolve warns and falls back to
`sys.executable`; it never refuses, since refusing would strand the machine
being set up.

The interpreter in force, and the file it came from, are printed as part of
the plan — so `--dry-run` shows a wrong choice *before* anything is written —
along with every missing dependency, what it breaks, and the `pip` command
that fixes it. `status` reprints all of it on every run, and says so when
`.python-version` has since changed to name something other than what is
pinned. An interpreter rots after you pin it — virtualenvs get rebuilt,
packages get pruned — and that rot is exactly as invisible as the file drift
above it.
