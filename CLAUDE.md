# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`rime-copilot` is a **librime plugin** (not a standalone project). It provides next-word
prediction (DB n-gram + LLM), an auto-spacer for CJK/Latin boundaries, and an IME bridge
that lets external editors control `ascii_mode` over a Unix socket. Originally forked from
[librime-predict](https://github.com/rime/librime-predict).

The repo lives at `librime/plugins/copilot` and is compiled *inside* a librime build tree —
it cannot be built on its own. `CMakeLists.txt` here is included by librime's plugin
mechanism and exports `plugin_objs`/`plugin_modules` to the parent scope.

### Where the design records live, and why they are not here

Every measurement in this file rests on a design or results document —
`2026-08-22-lexicon-phase1-results.md`, `2026-08-21-rerank-cost-and-gate-results.md`
and about thirty others. They are **deliberately not in this repository**, and
`docs/superpowers/` is gitignored.

Every one of them quotes the evaluation corpus, and that corpus is the
author's own private messages: real sentences, colleagues' names beside how
often each appears, internal project names. That is other people's data as
much as the author's, and this remote is public. Redacting them would have
gutted the arguments — "the coverage gap is colleagues' names and company
jargon no public dictionary can hold" is an assertion without its examples —
so they live on the machine that has the corpus instead.

What that costs, stated plainly: **the figures in this file cannot be
independently checked from this repository alone.** The tooling that produced
them is all here (`rime-corpus kbest`, `compare_rerank`, `compare_warmed`,
`replay_copilot`), so anyone with their own corpus can rerun the same
measurements; nobody can audit the author's. Treat the numbers as this
project's own records, not as published results.

They live in iCloud so every machine has them:

```
~/Library/Mobile Documents/com~apple~CloudDocs/config/rime-copilot/superpowers-docs/
```

`docs/superpowers` in a checkout is a **symlink** to that directory, and is
gitignored — one copy, reachable at the path every reference above uses. A
machine without it set up will find the reference dangling; recreate the
symlink rather than copying, or the two diverge.

## Build & lint

Mirror CI (`.github/workflows/ci.yml`) — check out librime and nest this repo under `plugins/copilot`:

```sh
# from a fresh librime checkout, with this repo at plugins/copilot
./action-install-linux.sh                                    # deps (Linux)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=ON
cmake --build build
```

- **That command mirrors CI, and CI does not need the deployable dylib.**
  `BUILD_MERGED_PLUGINS` is librime's own option and defaults to **ON**
  (librime `CMakeLists.txt:16`), which folds the plugin into `librime.dylib`
  and emits no `lib/rime-plugins/` at all. CI is fine that way — `copilot_test`
  links the plugin objects directly — but every deploy recipe below copies
  `build/lib/rime-plugins/librime-copilot.dylib`, so **a tree configured with
  the command above has nothing to copy.** For a tree you will deploy from:
  ```sh
  cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_MERGED_PLUGINS=OFF
  ```
  Measured 2026-09-05: a from-scratch configure following the documented line
  built cleanly, passed every test, and produced no dylib. Nothing said so —
  the missing artifact is only visible at the `sudo cp`.
- **llama.cpp** (tag `b10456`) and **nlohmann_json** are pulled via CMake `FetchContent` — the first configure downloads and builds llama.cpp, which is slow.
- **A Homebrew `ggml` or `llama.cpp` formula breaks the build**, and the error
  names neither. librime's root `include_directories()` calls for Boost, glog,
  yaml-cpp, leveldb, marisa and opencc all resolve to `/opt/homebrew/include`
  on macOS, and that lands FIRST on every compile line — ahead of the fetched
  ggml's own `../include`. Those formulae install `ggml.h`/`llama.h` there, so
  `ggml.c` compiles against a foreign header and dies on
  `static_assert(GGML_GLU_OP_COUNT == 6)`. `CMakeLists.txt` now detects this,
  drops the prefix for the vendored subtree only, and warns; the tidier fix is
  `brew uninstall llama.cpp ggml`, which nothing here needs. Same shape as the
  MLX header/library mixup recorded under "The MLX backend".
- Lint (CI gates on this, `clang-format -Werror`):
  ```sh
  find src tools -name '*.cc' -o -name '*.h' | xargs clang-format -i
  ```
- **Tests** use GoogleTest, following librime's convention. The `copilot_test` target
  (`test/`) links the plugin objects plus GTest and is registered with ctest; it builds when
  the librime root's `BUILD_TEST` option is ON (the default). Run just this suite with:
  ```sh
  ctest --test-dir build -R copilot_test --output-on-failure
  ```
  Coverage: the pure spacing helpers (`src/auto_spacer_util.h`), the `UTF8`/`History`
  utilities (`src/history.h`), and the Space-commit decision `ComputeSpaceCommitText`
  (`test/commit_text_test.cc`, which guards the multi-segment "commit the whole composition,
  not just the last segment" behavior with a hand-built Context). All tests are pure-logic —
  no Rime engine is stood up, since `Engine::Create()` pulls in Switcher/deployer init a bare
  test main can't provide. To test AutoSpacer logic without an engine, extract it into a free
  function / `auto_spacer_detail` helper and drive it directly (see `ComputeSpaceCommitText`).
  `test/ime_bridge_socket_test.cc` is the one exception to "pure logic": it stands up a real
  `ImeBridgeServer` on a Unix socket (no Rime engine) to cover the parts that only exist over
  a real connection — the greeting, connection refcounting, `Stop()` draining.
  CI runs lint + build-with-ASAN + the GTest suite. The Neovim client's Lua specs
  (`endpoint_spec.lua`, `verify_spec.lua`) moved with it to `rime-copilot-clients` and run
  there — this repo has no Lua tests of its own any more.

### macOS vs Linux

`src/imk_client.mm` (Objective-C++, IMK integration for surrounding text) is compiled
**only on Apple** — `CMakeLists.txt` explicitly `REMOVE_ITEM`s it from the auto-globbed
sources and re-adds it under `if(APPLE)`. When editing macOS-only code, keep it out of the
default source set so Linux CI (no ObjC++ toolchain) still builds.

## Architecture

### Module registration
`src/copilot_module.cc` is the entry point (`RIME_REGISTER_MODULE(copilot)`). It registers
every rime component this plugin exposes: the `copilot` processor, `copilot_translator`,
the sub-plugin processors (`auto_spacer`, `select_character`), and filters
(`auto_spacer_filter`, `raw_input_filter`, `copilot_filter`). Anything new that rime needs
to instantiate by name must be registered here.

### Processor composition (the CRTP plugin pattern)
The top-level `Copilot` processor (`src/copilot.cc`) owns an **internal chain of
sub-processors** (`ImeBridge`, `AutoSpacer`, `SelectCharacter`), built in its constructor
and gated by `copilot/disabled_plugins` config. It runs them via `RunProcessors` before its
own prediction logic.

Sub-plugins use the CRTP wrapper `CopilotPlugin<T>` in `src/copilot_plugin.h`. It
SFINAE-detects which `Process` overload `T` implements — `Process(KeyEvent&, string*)`
(with output, fires `on_accept`/`on_noop` callbacks) or `Process(KeyEvent&)` (no output) —
so sub-plugins just define a `Process` method and don't touch the `Processor` boilerplate.
`CopilotPluginComponent<T>` is the matching factory.

### Prediction engine & providers
`CopilotEngine` (`src/copilot_engine.{h,cc}`) is per-schema (cached in
`CopilotEngineComponent` via `weak<>` + a `DbPool<CopilotDb>`). It holds an ordered list of
`Provider`s and merges their candidates. Two providers implement the `Provider` interface
(`src/provider.h`, `Predict()` + `Retrive(timeout_us)` + `Rank()`):
- `DbProvider` (`src/db_provider.h`, backed by `CopilotDb`) — n-gram lookups.
- `LlmProvider` (`src/llm_provider.{h,cc}`, backed by `llm.{h,cc}`) — llama.cpp gguf inference.

`Entry` (`copilot::Entry` in `provider.h`) is the common candidate type carrying
`text`/`weight`/`type`. `src/history.{h,cc}` tracks commit history used as prediction context.

**`copilot/max_candidates: 0` costs more than it looks, and the fix is the
config, not the code.** The key caps `DBProvider::Lookup` itself, not just the
display, so unset (0 -> `INT_MAX`) a prediction round materialises every
continuation of every one of its `max_hints` keys. Measured 2026-09-06 against
the deployed db, timing `Predict` + `Retrive` + `MergeProviderCandidates`:

| context | total p50 | candidates |
| --- | --- | --- |
| `尝试进行代码` | 0.031 ms | 207 |
| `好的谢谢` | 0.14 ms | 846 |
| `高屋建` | 0.61 ms | 2850 |
| **`我`** | **2.43 ms** (p95 4.11) | **10,356** |

The tail is reached whenever the last committed character is a common one, and
it runs on the input thread after every commit.

**The obvious code fix is worth 11% and should not be made.** `Predict` builds
a `std::list` (a heap allocation per entry), `list::sort`s it, copies it into
`candidates_`, `Retrive` returns that BY VALUE, and `MergeProviderCandidates`
copies and sorts again -- four copies and two sorts, which reads like the
problem and is not it. Measured on `我`: the copies and both sorts together are
0.12 ms of the 2.43 ms, and a `std::vector` + `nth_element` rewrite lands at
2.16 ms. **92% of the cost is the lookup**, and `max_candidates` removes it at
the source: the same round with the key set to 100 costs **0.009 ms**, a 270x
cut with no code change at all.

**Capping is lossless, and the argument has a premise worth knowing.** Each
key's continuations are stored weight-descending (measured: 0 inversions over
`我`'s 10,356), so an entry in the true global top-K must also be in its own
key's top-K -- capping each key at K therefore preserves the global top-K
exactly. But that ordering is EMERGENT rather than asserted anywhere: it falls
out of `dictdb.merge` sorting by `(first character, -weight)`, `write_pairs`
walking that order into a plain dict whose iteration order is insertion order,
and `if e.weight > block.get(key, 0)` letting the first (heaviest) insertion
stick. Three independent steps, none of which mentions the others.

`tools/test/dictdb_test.py` now pins all three, and each was verified to have
teeth by breaking that step and watching the suite go red. The third one had
none until 2026-09-06: `test_duplicate_pairs_from_multiple_readings_keep_the_largest`
feeds ASCENDING input, and in ascending order "keep the largest" and "keep the
last" agree, so replacing that condition with a plain assignment left the whole
suite green.

`max_per_key` (`dictdb.write_pairs`, the build-time analogue) defaults to `-1`
-> `inf`, so both gates are open by default. That is why `我` carries 10,356
continuations at all.

### Sub-plugins
- **AutoSpacer** (`src/auto_spacer.{h,cc}`) — inserts spaces at CJK↔Latin/number boundaries.
  Two paths: a `surrounding` path using real before/after context (from IMK or IME Bridge),
  and a `history` fallback using `commit_history`. See README "Auto Spacer Logic" for the
  exact rules — the boundary/ASCII-mode behavior is subtle and spec'd there.
- **ImeBridge** (`src/ime_bridge.{h,cc}`) — Unix-socket server (JSON Lines protocol,
  default `/tmp/rime_copilot_ime.sock`) letting editors set/restore `ascii_mode` and push
  surrounding-text context. Protocol and actions are documented in README "IME Bridge".
- **SelectCharacter** (`src/select_character.{h,cc}`).

All three can be turned off via `copilot/disabled_plugins` in the schema config.

### The surrounding-text sources, and which one you actually pay for

`GetSurroundingContext()` (`src/surrounding_source.cc`) is IMK, then ImeBridge,
then a tmux pane scrape, first to answer wins. The first two are a mutex and a
struct copy. The third is a `posix_spawn`, and on a machine that lives in a
terminal it is the one that answers:

| source | share of live fetches |
| --- | --- |
| tmux | 1041 (72%) |
| IMK | 331 (23%) |
| ImeBridge | 71 (5%) |

**Measured 2026-09-06, the tmux query costs p50 3.86 ms / p95 4.23 ms** —
`BuildTmuxArgs`' exact five-command exec, 60 repetitions against a live server.
It runs on the input thread, synchronously, with `timeout_ms` (default 50) as
the ceiling.

The memo makes that **once per key event, not once per consumer** — three
callers ask independently — and `Copilot::ProcessKeyEvent` skips the
invalidation while composing, so a whole Chinese word costs one query rather
than one per keystroke. What is left is one query per NON-composing key event:
every keystroke in ASCII mode, every character typed at a shell prompt, and the
first key of each word. In a terminal that is 3.86 ms per key.

That is larger than everything remaining in the scorer. Do not quote the
3.86 ms as "measured overhead of the tmux source" without saying it is per
non-composing key event only.

**Where the 3.86 ms goes, and why "ask for less" buys nothing.** Measured
2026-09-06, 40-60 repetitions each:

| | p50 |
| --- | --- |
| `/usr/bin/true` (process-spawn floor) | 1.46 ms |
| `tmux display-message -p x` (the most trivial query there is) | 3.78 ms |
| the real five-command query, 5 KB of pane | 3.86 ms |

So it is ~1.46 ms of process spawn, ~2.3 ms of tmux client startup and
server-socket handshake, and **~0.25 ms of actually doing the work**. Capturing
fewer rows, dropping `-e`, asking for less — all of it is inside that 0.25 ms.
The cost is the invocation, not the content. Reducing how OFTEN we ask is the
only thing that would matter, and that is a question about AutoSpacer's trigger,
not about the query.

**Reading the pane's device file instead is not possible.** Recorded because it
is the natural first idea. `#{pane_tty}` is a character device of size 0 -- a
stream, not a stored screen -- and it is the SLAVE side, held by the pane's
shell as fds 0/1/2, so reading it would compete with that shell for the user's
keystrokes. The master side lives inside the tmux server with no filesystem
name at all (it does not even appear in `lsof` on the server by name). The grid
is reconstructed by the server's own VT parser into its heap; there is no file,
no shared memory and no mmap that exposes it. Speaking the server's unix-socket
protocol directly is the same dead end wearing a different hat: the 2.3 ms IS
that protocol's handshake, and reimplementing it buys nothing a persistent
connection does not already get, in exchange for an internal protocol tmux
version-checks and does not support third parties using.

**A persistent control-mode connection is worth 10x, and is deliberately not
built.** `tmux -C attach` keeps one client alive and takes commands on its
stdin, so the spawn and the handshake are paid once instead of per key event.
Measured over 60 repetitions of the identical five-command query:

| | p50 | p95 | p99 |
| --- | --- | --- | --- |
| today, `posix_spawn` per query | 3.86 ms | 4.23 ms | 4.74 ms |
| `tmux -C`, persistent | **0.39 ms** | 0.50 ms | 0.54 ms |

(The round-trip floor on that connection is 0.08 ms; the four metadata
commands are 0.28 ms; the pane capture takes it to 0.39 ms.)

It is not built because the win is unperceived -- typing in a terminal does not
feel slow -- and because the cost is larger than "a subprocess and a reader
thread". **A control-mode client is a real attached client**, and it collides
head-on with the arbitration this source depends on:

- It appears in `list-clients` as a second client, flagged
  `attached,focused,control-mode`, with its own `client_activity` that bumps on
  every query we make.
- `JudgeClients` (`tmux_source_util.h`) returns `kFocusEventsOff` whenever
  `activity.size() > 1` and `focus-events` is off -- **and `focus-events` is off
  by default in tmux**. So on a default machine a permanent control-mode client
  would make this source refuse every query, forever. The feature would not
  degrade; it would stop.
- With `focus-events on` it is worse rather than better: our own client is
  frequently the most recently active one, and tmux answers `display-message`
  for that client -- which is precisely the cross-talk `JudgeClients` exists to
  refuse, arrived at confidently.

Solvable, probably: filter control-mode clients out of the `CLI|` list by
`client_flags`, or attach the connection to a throwaway session and switch the
query to `list-clients -t <the user's session>`. But that means reopening the
one piece of logic in this file whose whole job is to refuse to answer rather
than answer wrongly, which is a much larger change than the connection itself.

Verified NOT a problem: a control-mode client does not resize the session
(window stayed 159x43 with the client reporting `80x`), and the existing
5-second backoff on a failed or timed-out query already bounds the tail from a
wedged server to one 50 ms keystroke.

**What would reopen this**: terminal typing actually feeling slow. The
measurement and the collision are both recorded above, so the work would start
from the arbitration question rather than from the connection.

### AutoSpacer's commits and Rime's user dictionary

AutoSpacer emits its own commits (`engine_->CommitText()`), so for a long time
none of them reached `Context::Commit()` — the only thing that fires
`commit_notifier_`, which is the only route to `Memory::OnCommit`, which is the
only thing that writes to `private.userdb`. **Rime's user dictionary therefore
learned nothing on any machine using the surrounding-text path**: measured, 220
bytes and `/tick 0` after a week of daily use, against 5,226 entries on the
machine that predates those sources (its `ProcessWithCommitHistory` has no
Space branch, so Space there falls through to Rime's own path and always
learned).

`NotifyForLearning` (`auto_spacer.h`) closes it: it sets the `dumb` option —
which is how Rime already says "notify, but do not commit anything"
(`switcher.cc:24`) — fires `ctx->commit_notifier()(ctx)` **directly**, and
restores `dumb`. Under `dumb`, `GetCommitText()` returns `""`, so
`ConcreteEngine::OnCommit`'s `sink_(text)` appends nothing and
`Session::HasCommit()` stays false; `Memory::OnCommit` reads
`ctx->composition()` and is unaffected.

Four things about it are load-bearing, and each is easy to get wrong:

- **It marks the last segment `kConfirmed` before notifying, and firing the
  notifier is useless without that.** `ScriptTranslator::ProcessSegmentOnCommit`
  (`script_translator.cc:273-287`) pushes each recognized phrase into a
  **member** `queue_` and flushes it only when
  `!recognized || seg.status >= Segment::kConfirmed`; that status is assigned in
  exactly one place in all of librime — `ConcreteEngine::OnSelect`
  (`engine.cc:264`), reached only through `select_notifier_` — and these paths
  go through neither `Context::Select()` nor `ConfirmCurrentSelection()`. Left
  unmarked the phrase is not merely unsaved: it waits in the queue until some
  later commit has an unrecognized candidate and is then written as **one entry
  spanning several unrelated commits**, which is the fragment class `clean.py`
  exists to prune — generated here rather than imported. Measured live: two
  single-segment Space commits produced two *empty* LevelDB WriteBatches, and a
  sentence typed over several commits became one 30-character entry. The flag,
  not `ConfirmCurrentSelection()` — that also runs `seg.Close()`,
  `composition().Forward()`, and under `_auto_commit` a second `ctx->Commit()`.

- **It fires at exactly two sites — Space and the number-key select — never on
  the two bail-outs.** `Memory::ProcessSegmentOnCommit` memorises
  `seg.GetSelectedCandidate()`, the *still-highlighted* candidate. On Enter's
  raw commit or the number-key fallback the user discarded every candidate, but
  the composition still shows one; learning there would memorise exactly what
  they rejected, and `Language::intelligible` does not save you — that
  candidate is usually a legitimate Han phrase. `on_commit_`'s existing
  `selection_commit` bool is precisely this predicate.
- **It deliberately stops short of `Clear()`.** `Context::Commit()` is the
  notifier followed by `Clear()`, and `Clear()` fires `update_notifier_`
  *synchronously* — reaching `Copilot::OnContextUpdate`, which reads
  `commit_history().back()`. If the caller's decorated record has not been
  pushed by then, `back()` is the undecorated per-segment record
  `ConcreteEngine::OnCommit` just pushed, the `type == "raw"` early return is
  missed, and every Space commit kicks off a spurious prediction cycle on the
  wrong text. So the order at both sites is: `CommitText` → `on_commit_` →
  `NotifyForLearning` → `push_back(decorated)` → `ctx->Clear()`.
- **`Copilot::OnCommit` returns early under `dumb`.** It subscribes to
  `commit_notifier_` too, and `on_commit_` has already done its two jobs by
  then; without the guard every commit warms the scorer with an empty
  `GetCommitText()` and writes a duplicate telemetry line.

One accepted consequence: each such commit now leaves **`2 + N`** records in
the bounded 20-entry `CommitHistory` — `engine_->CommitText()` pushes one
(`engine.cc:245`), `ConcreteEngine::OnCommit` pushes one *per segment* with
adjacent same-type runs joined (`commit_history.cc:32-58`), and AutoSpacer
pushes its own decorated one last. Every reader in this tree uses only
`.back()`/`.empty()`/`latest_text()` (`copilot.cc:409,413`, `filters.cc:58,61`,
`auto_spacer.cc:160,572`) and none iterates, so it is inert — it only shortens
the window's effective depth, and by more than "two" would suggest.

**How to tell whether it is working, and how not to.** Byte counts are not the
criterion: before this fix the same two commits produced *writes* (two empty
WriteBatches) and a 30-character concatenation that read like success. Decode
the WAL and count entries. One commit should produce one entry with its own
`c=` count and its own `t=` tick:

```
/tick 32799   ce shi  <TAB> 测试   c=74 d=2.53609 t=32799
/tick 32800   tian qi <TAB> 天气   c=5  d=1       t=32800
```

Expect a **one-commit lag**: `Memory::StartSession` opens a transaction that
`ScriptTranslator::Query`'s `FinishSession` closes on the next translation, so
commit N lands when commit N+1 is typed.

**The replay harness cannot verify any of this.** `replay_copilot` feeds only
double-pinyin letters to `process_key` and commits through
`rime->select_candidate` — Rime's own path — so the AutoSpacer branches above
never execute under it. Learning already worked there before this change, which
is why `replay.py`'s docstring records top-1 running 32.8% → 99.2% across two
consecutive passes and why `restore_pristine_userdb` exists. Verification is
the GTests in `test/learning_commit_test.cc` plus watching `private.userdb`
grow in live use — decisive, because it stayed at 0 bytes through a week of
daily use before.

**Every `replay_copilot` figure in this repository self-trains within its own
run.** `restore_pristine_userdb` runs before and after each *arm*, not between
requests, so request N benefits from what requests 1..N−1 just committed —
measured directly, a "pristine" arm wrote 10,209 Han runs into its own user
dictionary purely from the eval half it was measuring. `replay.run_warmed_arm`
is the one deliberate exception to the pristine rule (it warms on one request
list, then measures another without a reset in between) and it refuses when the
warm pass wrote nothing, because a warm pass that silently learned nothing
would report the unwarmed number as though it were warmed.

`rime_corpus/compare_warmed.py` is its driver, and the reason it exists as a
committed module rather than the ad-hoc script that first produced
`2026-08-22-lexicon-phase2-results.md` is that this file is largely a list of
measurements that later turned out to be wrong — an unreproducible one cannot
be re-examined when the next one contradicts it. Like `compare_rerank.py` it is
deliberately **not** a `cli.py` subcommand (it needs a second rime-dir the
other subcommands have no use for), so it runs the same way:

```sh
python3 -c "from rime_corpus import compare_warmed; raise SystemExit(compare_warmed.main())" \
    --rime-dir-warm  ~/.local/share/rime-corpus/p2-warm \
    --rime-dir-cold  ~/.local/share/rime-corpus/p1-both \
    --eval-corpus-dir /tmp/lexicon-split/eval \
    --warm-corpus-dir /tmp/lexicon-split/train
```

## Tools (`tools/`)

C++ tools, built when `BUILD_TOOLS=ON` (default):
- `build_copilot` — builds the prediction DB. Reads stdin lines of
  `key text weight`, writes the DB file (arg 1, default `copilot.db`).
  Lands at `<librime>/build/plugins/copilot/bin/build_copilot`.
- `bench_scorer` — what one re-ranking costs on THIS machine: prefill and
  scoring timed separately, with the process's CPU time beside them. It exists
  because the deployed batched path lives inside `LlmScorer`, behind a Rime
  engine, and could not be timed through it; the file replicates that batch
  geometry and must stay in step with `llm_scorer.cc`. Run it before touching
  `n_gpu_layers`/`n_threads`, and on any new machine, rather than carrying this
  one's numbers over. **Every phase must end on a `llama_get_logits_ith()`, not
  on the `llama_decode()` before it** — on Metal the decode returns before the
  GPU is done and reading the logits is what synchronizes. Two versions of this
  tool got that wrong in opposite directions and each produced a confident
  latency verdict that was an artifact of where the clock stopped; the score
  phase must therefore also run the full `n_vocab` log-softmax `ScoreGroup`
  runs, and submit `len - 1` tokens per candidate as it does. Corrected, the
  two arms are within a millisecond on latency and the GPU wins on **core
  time** (1.93 ms/iteration against 14.79), which is the real argument for the
  default.
- `dump_copilot` — read-only probe: prints a key's continuations with weights,
  ranks and duplicate counts. First stop when candidates come out in a
  surprising order (`dump_copilot <db> --find 议 -- 建`).
- `ax_poc.mm` — macOS Accessibility API proof-of-concept (Apple-only).

**`score_candidates` links llama.cpp and nlohmann_json only — no `rime_library`,
no glog — and that is a linkage invariant, not a build-standalone claim: the
tool is still configured from inside the librime tree like everything else
here (this repo "cannot be built on its own" — see "What this is" above).**
It is the scorer behind the checkpoint-selection ruler named below (`rime-corpus
export-evalset` into `score_candidates` — see "Held-out validation loss does
NOT rank scorer checkpoints"); that is a narrower job than the lexicon figures
(`rime-corpus kbest`) or the replay figures (`replay_copilot`) quoted
elsewhere in this file, neither of which touches this tool.
`src/scoring_form.h` (context alignment, `AlignToTrainingForm`) exists as a
header separate from `src/rerank.h` specifically so this target can reach it
without pulling in `rerank.h`'s `<rime/candidate.h>`; `src/utf8_index.h`
(`copilot::UTF8`, `SplitU8`, `CharCount`) was extracted out of
`src/history.{h,cc}` on the 2026-08-23 surrounding-context branch for the same
reason, after a real regression. That branch briefly added `../src/history.cc`
to `score_candidates`'s CMake target to reach `copilot::UTF8` for the new
alignment code, and that made the target's *link* silently depend on
`CMAKE_BUILD_TYPE`: `history.cc` calls glog's `DLOG(INFO)`, which expands to a
real, glog-linking `LOG()` whenever `NDEBUG` is undefined. This file's own
"Build & lint" command does not reproduce it, and neither does CI — both pass
`-DCMAKE_BUILD_TYPE=Release`, which defines `NDEBUG`. The failure needs a
configure with **no** `CMAKE_BUILD_TYPE` at all, which is what a plain
`cmake -B build` gives and nothing documented here ever passes — precisely why
it went unnoticed until someone deviated from the documented command. It was
caught not by running any documented build but by extracting the real compile
and link commands `cmake --build` had used and re-running them by hand with
`-UNDEBUG`. Keep `score_candidates` and `bench_scorer` off every `.cc` under
`../src` that is not itself header-only with no glog dependency; if one needs
a helper from `src/`, that is a sign the helper belongs in a header-only file
like `utf8_index.h`, not a reason to link `history.cc`.

`rime-copilot` — the data pipeline that feeds `build_copilot`. One CLI over the
package in `tools/rime_copilot/`:

| Module | Responsibility |
| --- | --- |
| `paths.py` | Rime dir, `sync_dir` from `installation.yaml`, builder resolution |
| `dictfile.py` | reading and writing Rime `*.dict.yaml` |
| `dictdb.py` | merging weights, `top` stacking, prefix/suffix splitting |
| `scel.py` | Sogou `.scel` unpacking and downloading |
| `vault.py` | backup/restore of the unversioned files, to the iCloud sync dir |
| `freshness.py` | the content-hash rebuild decision |
| `install.py` | copying the CLI + `build_copilot` into `<rime_dir>/private/bin` so it runs standalone, and detecting drift in that copy |
| `clean.py` | pruning the Sogou-exported personal lexicon; the rule chain and the review file |
| `personal.py` | generating `private/personal.dict.yaml` from the cleaned lexicon plus the harvested corpus |
| `cli.py` | orchestration |

Subcommands: `status`, `restore`, `backup`, `fetch`, `build`, `deploy`,
`update`, `install`, `clean`, `personal`. A new machine builds `build_copilot`
from a librime checkout, runs `install` once, then `restore` and `update` from
the installed `private/bin/rime-copilot` — no checkout needed after that.

**The personal dictionary, and why it is a second file rather than a rewrite
of `custom.dict.yaml`.** `private.dict.yaml` imports `private/personal`, which
`rime-copilot personal` generates (and `update` regenerates). It exists because
personal *frequency* had no way to reach candidate ordering at all:
`entry_collector.cc` de-duplicates only single-syllable entries and
`vocabulary.cc:61` sorts a code's homophones by weight descending, so for any
`(text, code)` a public table also carries, the larger weight wins — and
`custom.dict.yaml`'s weights are raw commit counts, 3..2877 with a median of 7,
against a flat 100 shared by 1.47M `ext`/`tencent`/`sogou` entries. 92% of the
personal lexicon was invisible; the rest ranked below the floor. Measured
held-out: **+1.45 points of first-candidate accuracy** (the Phase 1 results
record, kept locally -- see "Where the design records live").

`custom.dict.yaml` is not rewritten because it is vaulted, its sha256 is what
`.copilot_clean_stamp.json` describes, and `dict.json` reads it expecting
**commit counts**, which `boost: "log"` then compresses. Rewriting its weights
would silently change the predict db too. For the same reason
`personal.dict.yaml` is mounted in `private.dict.yaml` **only, never in
`dict.json`** — the predict db already reads `custom` as a `top` source, and a
file derived from it would stack that offset twice.

Two properties of the generator are load-bearing:

- **`version:` is a hash of the inputs, not a date.** The file is vaulted and
  regenerated on every `update`; a date would make it differ from the vault
  every new day and `status` would print a `conflict` on a file whose
  vocabulary is byte-identical — forever. In a repo whose discipline is "read
  every line of `status`", an always-on meaningless line is how that discipline
  dies.
- **`personal` refuses to regenerate when the corpus is absent and the file
  already exists.** The corpus travels by iCloud now, but "travels" is not
  "is here": a machine whose symlinks are not set up, or whose iCloud has not
  finished downloading, has none. Without the guard it would build from
  `custom.dict.yaml` alone, overwrite the ~8,800-entry file `restore` just
  brought down with an ~8,200-entry one, lose every corpus-mined word, and
  report success.

**Known limit of the mined half:** readings come from `pypinyin`, which has no
context for an out-of-vocabulary word and takes the commonest reading — `联调`
is annotated `lian tiao`, not `lian diao`. Such an entry is unreachable rather
than harmful, and the +1.45 was measured with these readings in place, so the
cost is already priced in. Fixing it needs a heteronym pass, not a bigger
corpus.

**Drift-detection contract:** `install` records a manifest
(`private/bin/.installed.json`: source commit, source path, content hash per
installed file) so `status` can report, every run, whether the installed copy
has been edited in place, is missing a file, or has fallen behind the repo it
came from — without that, an installed copy is just a second, unversioned
`private/bin/` waiting to rot the same way the original one did.

**The lexicon oracle:** `clean` decides whether an entry in the Sogou-exported
personal lexicon is a real word or a cross-word-boundary fragment Sogou
learned from sentence input (`的问题`, `编译的`). Neither dictionary already in
the tree can answer that — `ext`/`tencent` carry weight 100 for every entry,
and `base.dict.yaml` has artificial bands where `你们 501135` and `上的
502252` sit side by side. `jieba`'s segmentation dictionary is the oracle
instead, because it is free of such fragments by construction. It is a
**positive** oracle only: absence proves nothing (`是的`, `自动驾驶` are absent
and real), which is why rule R9 lets the user's own commit count overrule a
structural verdict — two earlier drafts of the chain lacked R9 and deleted
`好了`, `你们`, `那个`, `是的`. `jieba` is in `RUNTIME_REQUIREMENTS` alongside
`pypinyin`, and imported lazily for the same reason.

`dict.json` gained `boost: "log"` for `top` sources (`dictdb.py`): without it
a `top` entry's own commit count is swamped by the public weight it stacks on
(`确定是 = ceiling + 14925 + 7`), so the personal band is ordered by public
frequency and the copilot db's rank gate has nothing to measure. It gives the
personal term the same `0..ceiling` range as the public one — it does **not**
make personal frequency the primary sort key, and a near-ceiling public
weight still wins. That limit is pinned by a test rather than left to be
rediscovered.

**`boost` is invisible to older code.** `load_sources` reads named keys and
ignores the rest, so a `dict.json` carrying `"boost": "log"` restored onto a
checkout that predates it builds a different database, reports itself up to
date, and says nothing. On a second machine, `git pull` and `install` come
before `restore` and `update`. The same applies to any future `dict.json`
key: adding one is a silent-divergence hazard across machines until every
machine has the code that reads it.

**What travels where.** The lexicon, its pristine `.raw`, the clean stamp,
`dict.json`, `private.dict.yaml` and `private/personal.dict.yaml` are vaulted
(`vault.py:VAULTED_FILES`) and move by iCloud; `private.predict.db` and
`sogou.dict.yaml` are derived and rebuilt locally;
the CLI moves by git plus `install`; `jieba` and `pypinyin` move by neither —
`status` names any that are missing, what they break, and the `pip` command.
`.copilot_clean_stamp.json` is vaulted while `.copilot_build_stamp.json` is
not, and the asymmetry is deliberate: the build stamp describes a locally
built database, the clean stamp describes a shared file. Without the clean
stamp in the vault a second machine reports `lexicon: not cleaned`, and
acting on that report replaces the pristine export with an already-cleaned
copy and then pushes it over the genuine one.

**`personal.dict.yaml` is vaulted despite being derived, and it must travel
with `private.dict.yaml`.** Two files, one valid state. `private.dict.yaml`
names `private/personal` in `import_tables`, and a missing import table is not
a degradation: `get_dict_files_from_settings` (`dict_compiler.cc:47-62`)
returns false on the first path that does not exist, failing the **whole**
dictionary build — that machine stops converting. Regenerating locally is not
a reliable fallback either: it needs the corpus to be present, not merely
synced. This is the same exception `rime40m-q8.gguf` takes, on a stronger
argument — the pair, not the regeneration, is what carries it.

**Both directions of the vault refuse to clobber, and the clean stamp is
checked against the file it describes.** `restore` always protected local
content; `backup` protected nothing, so a machine holding an *older* file
pushed it over a newer one and reported success. That is how Mac-Mini's
June-2025 pristine export replaced the cleaned 8231-entry lexicon another
machine had vaulted eleven days earlier: `restore` correctly refused to
overwrite mini's local copy (conflict), then `backup` sent that same copy up.
`plan_backup` now takes the machine id and refuses when the vault's copy was
last written by *another* machine — replacing your own is still frictionless,
or the `--force` reflex becomes automatic. Reconciling goes either way
(`restore --force` to take theirs, `backup --force` to keep yours), so the
message names both.

The second half of that incident was silent for a different reason: `status`
printed the `lexicon:` line straight from the clean stamp, and the stamp is
vaulted, so mini reported `cleaned 2026-08-17T09:13:29Z, 8231 entries` while
holding the 1MB raw. It now compares the stamp's `result_sha256` against the
live `custom.dict.yaml`, and names the pristine-export case specifically —
that one has a different fix (`restore`, emphatically *not* `backup`) from a
generic mismatch. A stamp predating those hashes is reported as-is rather
than turned into a mismatch it cannot disprove.

**The weight convention lives in `dictdb.py` and in `src/db_provider.h` /
`src/rerank.h`, and must match: larger = more likely.** Writing a rank instead
of a real weight silently inverts every ordering in the plugin. `Entry.weight`
(`dictfile.py`) is a `float`, not an `int` — it becomes fractional once a
`scale` is applied.

Third-party imports (`pypinyin`, `jieba`, `requests`, `bs4`) are lazy, at the
point of use, so every module imports on a stock interpreter. `pypinyin` is
required at runtime for any dictionary with no pinyin column (e.g.
`tencent.dict.yaml`, which is `word⇥weight` only); `jieba` for `clean`, to
tell a real word from a Sogou sentence fragment; `requests`/`bs4` for
`fetch`. From a checkout, whether they are found depends on the ambient
interpreter (pyenv's per-directory `.python-version` resolves from the
*caller's* current working directory, not the script's location, so this is
easy to get wrong; a bare `#!/usr/bin/env python3` does not fix it, since
that resolution happens regardless of the shebang). `install`
(`tools/rime_copilot/install.py`) avoids this for the installed copy by
rewriting the entry point's shebang to an absolute interpreter path instead
of copying the source's shebang through. That path is chosen in order:
`--python`, then the **destination's own `.python-version`**
(`declared_interpreter()` — the nearest one at or above `private/bin`,
resolved by `pyenv which python3`), then `sys.executable`. The last is the
one that must not be first: it inherits the same pyenv-cwd trap, so running
`install` from the checkout pinned whatever env a parent of the *checkout*
named. The interpreter and the file it came from are printed as part of the
plan (visible under `--dry-run`, before anything is written). Two invariants
here are easy to break: (1) interpreter paths are taken **as given, never
`resolve()`d** — a virtualenv's `bin/python3` is a symlink to its base
interpreter, i.e. the one without the env's packages; (2) `PYENV_DIR` must
be set explicitly on the `pyenv which` call — pyenv searches from
`${PYENV_DIR:-$PWD}` and `$PWD` comes from the inherited env var, which
subprocess `cwd=` does not update, so cwd alone silently resolved against
the caller's directory while reporting the destination's version. The fake
`pyenv` in `install_test.py` is deliberately faithful to that (it reads
`${PYENV_DIR:-$PWD}/.python-version`); a stub echoing a fixed answer passed
while the code was wrong. Dependency checking covers **all** of `RUNTIME_REQUIREMENTS`
(`install.py`), not just `pypinyin` — an interpreter that had `pypinyin` and
no `bs4` once installed without a word — and both `install` and every
`status` name what is missing, what it breaks, and the `pip` command for it.
Tests are stdlib `unittest` and never touch `~/Library/Rime`:

```sh
python3 -m unittest discover -s tools/test -p '*_test.py'
```

Every test module puts `tools/` on `sys.path` itself, so each one also runs
alone. That is not decoration: nine of them once relied on discovery's
alphabetical order having already imported a module that patched the path, and
the one that sorts first (`claude_adapter_test`) had nothing ahead of it and
failed in CI. Keep the bootstrap when adding a module.

`.python-version` pins a pyenv virtualenv holding the runtime deps, so the
command above needs no activation. It is **machine-local and gitignored**: it
was tracked once, pinning the name `rime-copilot` for every checkout, and on a
machine whose venv is named anything else it resolved to nothing and the tests
quietly ran on the ambient interpreter — six of them erroring on a missing
`pypinyin` that was installed all along. Name the environment whatever you
like; declare what goes *in* it in `tools/requirements.txt`. On a new machine:

```sh
pyenv virtualenv system rime-copilot          # any name; put it in .python-version
~/.pyenv/versions/rime-copilot/bin/pip install -r tools/requirements.txt
```

**`tools/requirements.txt` is generated** from `RUNTIME_REQUIREMENTS`
(`install.py`) — regenerate it with `rime-copilot install --write-requirements`,
never by hand, and a test in `install_test.py` fails if the checked-in file
drifts. It is generated because the hand-kept version was wrong in both
available ways at once: stale (`jieba` reached `RUNTIME_REQUIREMENTS` and never
the file) and naming import names instead of pip names (`bs4`, a forwarding
shim, for `beautifulsoup4`). Nothing in the tree read it, so its only reader
was a human setting up a machine — who got an environment with no `clean` and
nothing saying why. `RUNTIME_REQUIREMENTS` is the list `status` actually
executes against the pinned interpreter on every run; the file is its
projection, and the test is what keeps the two one truth.

`RECIPE_VERSION` in `freshness.py` must be bumped by hand when a change to the
build algorithm alters the output for unchanged inputs. No input hash catches
that, and a stale database will otherwise be reported as fresh.

## Neural re-ranking: what it is and how a second machine gets it

`copilot/rerank/llm` orders the candidate list with a 40.9M-parameter model
trained for this input method rather than with the db's n-gram, using the real
text before the caret. Trained from scratch on 4.5B tokens of Chinese
(`tools/rime_train/`, and the corpus-pipeline design record kept locally);
42MB at Q8_0. The deployed file is `rime40m-v2-q8.gguf`, the 2026-08-22
retrain -- the vault carries both names on purpose, see `vault.VAULTED_FILES`.

**Its latency is bimodal, and a single p50 describes neither mode.** This file
used to say "p50 4.7ms / p99 11.0ms per scoring", which is a number from
`bench_scorer` and not what a machine actually experiences. Measured live over
524 v6 scorings on two machines, the split is on ONE thing -- whether the batch
needs a `llama_decode` at all:

| longest candidate in the batch | share | p50 | p95 |
| --- | --- | --- | --- |
| 1 character (every candidate one token) | 44% | **0.18 ms** | 0.31 ms |
| 2+ characters | 56% | **11.1 ms** | 22.7 ms |

`ScoreGroup` (`llm_scorer.cc:484`) scores every candidate's FIRST token off
`ctx_last_logits_` -- the prefill's own last row, no decode -- and only submits
`len - 1` tokens per candidate. A window of all single-character candidates
therefore reaches `n_tok == 0` and never calls `llama_decode`; that is the
0.18ms mode, and it is essentially free. Anything longer costs exactly one
decode, and that decode is the whole 11ms.

Two things follow, and both were got wrong before this was measured:

- **Context length is not the cost.** The slow mode is flat in it: p50 11.18ms
  at a fetched depth of 1-7 characters against 11.58ms at 24-32, and
  `bench_scorer` on an M4 Pro puts context 32 vs 64 at score p50 2.11 vs
  2.09ms with prefill (which is background anyway) at 1.86 vs 2.12ms. This is
  why `context_chars` went to 64 on 2026-09-04 for free.
- **It is not GPU wake-up either** -- but the bucketing that established this
  could not have seen the effect that IS there. Live gaps were bucketed at
  "under 2 seconds" (11.41ms) against "over 10 minutes" (10.06ms), and it is
  flat across them. The CPU-clock decay below completes within 50ms, so both
  buckets sit on the far side of it. See the next paragraph: the answer is a
  downclock, and every live gap is long enough to pay it in full.

**Refined 2026-09-06: "one decode or none" is the shape, but the decode is not
one price.** The bimodal table above is right about WHICH windows are cheap and
wrong to imply the expensive ones all cost the same. Measured on an M4 Pro at
the deployed geometry (64-character context, `--idle-ms 100`, 30-40 iterations
per point), varying the two things `ScoreGroup` controls independently:

| scratch sequences | batch tokens | score p50 |
| --- | --- | --- |
| 2 | 4 | 7.91 ms |
| 4 | 4 | 9.07 ms |
| 4 | 8 | 11.41 ms |
| 8 | 8 | 14.28 ms |
| 4 | 12 | 13.40 ms |

Those five points fit **`score_p50 ≈ 4.6 + 0.6·(scratch sequences) + 0.55·(batch
tokens)` ms**, and the pair at (2,4) vs (4,4) is what separates the two terms:
same token count, two more branched sequences, +1.16 ms. So **branching a
scratch sequence costs about as much as decoding a token** -- roughly 0.6 ms
each, both of them Metal command encoding and dispatch on a downclocked core.

Geometry, because this file has been burned by leaving it out: these were run
at `bench_scorer`'s default `--n-ctx 2304`, i.e. `n_ctx_seq` 256, which is what
the currently deployed dylib runs (recopied 2026-09-05 14:33, after `669e79f`).
The (4,4) point at 9.07 ms reproduces the 9.0 ms this file already records for
`n_ctx_seq` 256 after a 100 ms idle, so the model DECOMPOSES a number already
here rather than adding a new one.

**It has not been checked against live telemetry, and the reason is worth
knowing.** Splitting `work_us` by schema version: v7 is p50 10.29 ms over 71
decode-bearing scorings, but every v7 line was written at `n_ctx_seq` **512**
(the dylib predating `669e79f`), so it is not comparable. v8 — the first
version written at 256 — is p50 6.11 ms over **7** scorings, which is too few
to compare against anything. A first draft of this paragraph quoted "9.64 ms
live", which was those two populations pooled: exactly the mistake recorded
two paragraphs down, made again. Re-check once v8 has a few hundred lines.

Two things this makes measurable that the bimodal account could not:

- **`top_n` is a latency lever of about 1.2 ms per candidate** (0.6 for the
  sequence plus 0.55 for its token), which is 13% of a deployed scoring each.
  See "`top_n` stays at 4" below for why it is not taken.
- **The all-single-character mode is free for the right reason.** 4 sequences
  with no decode measures 0.19 ms against 1 sequence with no decode at 0.16 ms
  -- the per-sequence term is ~0.02 ms when no decode follows it, because
  nothing ever attends across those cells. The 0.6 ms is the sequence's share
  of a decode, not the `seq_cp` itself.

**A consequence that looks like an optimisation and is not.** `ScoreGroup`
does `seq_rm` + `seq_cp` for every candidate including single-token ones,
whose score comes free off `ctx_last_logits_` and which submit no batch token.
Skipping those branches should cut the sequence term in a mixed window. It
would, and mixed windows essentially do not occur: over 78 decode-bearing
scorings carrying `n_decoded` (telemetry v7+), **72 have `n_scored ==
n_decoded`**, i.e. every scored candidate is exactly two tokens. The
population is bimodal per WINDOW, not per candidate -- `(4, 0)` is 29.7% of
scorings and `(4, 4)` is 42.3%, and almost nothing sits between. Recorded
because the idea is the obvious one and the measurement is cheap: the joint
distribution of `llm.n_scored` and `llm.n_decoded` in the telemetry log.

**Settled 2026-09-05: production is ~5x `bench_scorer` because the CPU is
downclocked, and about 77% of the deployed scoring latency is that.** The
question stood open with two suspects. Telemetry v7 killed the first -- the
`model_mutex_` wait is now timed separately (`ScoreTiming::lock_us`) and is
**0 across 97 scorings, maximum 0**, so a background prefill has never once
blocked a scoring. `bench_scorer --idle-ms` kills the second by reproducing
production exactly (M4 Pro, 64-char context):

| idle before the score | 0ms | 20ms | 50ms | 100ms | 250ms |
| --- | --- | --- | --- | --- | --- |
| score p50 | **2.30** | 8.19 | 9.91 | 10.20 | 10.28 |
| prefill p50 | 2.93 | 6.48 | 9.37 | 9.49 | 9.59 |

The decay completes within 20-50ms and saturates. Real typing gaps are 150ms
and up, so **production is permanently in the saturated column.**

Matching that against live needs the geometry the deployment actually runs,
which is NOT the table above: the dylib in `Squirrel.app` was copied at
15:38 on 2026-09-04 and `669e79f` (kNCtx 4096 -> 2304) landed at 15:44, so
every v7 line in the log was written at `n_ctx_seq` **512**, not 256. Re-run
there: hot **2.17 / 2.36ms**, after a 100ms idle **12.54 / 12.61ms**. Live v7
`work_us` p50 for decode-bearing scorings is **10.3ms** -- between the two and
near the cold end, which is what a keystroke-driven mix should look like:
`Apply()` runs for every live segment on every keystroke, so a multi-segment
composition scores several times back to back and those later scorings are
hot. A first draft of this paragraph claimed live and bench agreed "to a tenth
of a millisecond"; they did, at two different `n_ctx_seq`, which is a
coincidence and not a validation.

Either way there is no gap left to explain -- 2.2ms hot against 12.5ms cold at
the deployed geometry is a 10.3ms effect, and the competing hypothesis
predicted 2.1ms flat. The tool's old figures were the hot column, and nothing
before `--idle-ms` could print the other one.

**Every latency figure in this file predating 2026-09-04 is a hot-column
number unless it says otherwise**, which is a much larger caveat than it
sounds. The `kNCtx` 4096 -> 2304 change was worth -22.7% of a number that is
three-quarters downclock; the MLX backend's -48.7% was measured at the
deployed 100ms idle and so is a comparison of two downclocked runs, which is
the right comparison but not what its number was thought to mean.

**Three mitigations were measured on 2026-09-05 and all three failed.**
Recorded because the idea is the obvious one: `--qos`
(`QOS_CLASS_USER_INTERACTIVE`) does nothing at all (10.02 vs 10.01), so this
is frequency and not core placement; `--idle-spin` recovers half (score p50
5.0ms, p99 11.6 -> 5.5) but costs a permanently occupied core; and
`--pre-spin-us` / `--pre-spin-threads` -- a short burst triggered by the
keystroke itself, which is the only shippable shape -- **loses at every point
in the grid**. `keystroke p50` (the burst plus the score, which is what a user
pays) rises monotonically from 10.26ms at no burst to 18.93ms at 10ms of
burst, and it cannot do otherwise: the total recoverable is 5.3ms, so any
burst longer than that loses by arithmetic, and at 5ms the measured recovery
is 0.5ms. Eight cores instead of one changes nothing (9.68 vs 10.09, inside
the 0.3ms spread) for 2.1x the CPU. The clock ramp needs tens of milliseconds
of load whatever the core count; a keystroke gives microseconds of warning.
Both flags are kept in `bench_scorer` -- they are the instrument that produced
the negative, and a measurement nobody can re-run cannot be re-examined.

What is left is not a scheduling trick but **less CPU-side work per scoring**:
the penalty is paid on Metal command encoding and dispatch, so it scales with
how much of that there is. That is also the most likely mechanism behind the
MLX backend's win, which raises what fixing its runtime failure is worth.

Three artifacts have to be present:

| artifact | how it travels | check |
| --- | --- | --- |
| `private/rime40m-v2-q8.gguf` | **vaulted** — `rime-copilot restore` brings it down; `install-model --from PATH` is the manual route. The pre-retrain `rime40m-q8.gguf` is vaulted alongside it and is what the schema named before 2026-08-22 | `rime-copilot status` → `model:` |
| `private/zh-hans-t-essay-bgw.gram` | `rime-copilot fetch-grammar` (public download) | `rime-copilot status` → `grammar:` |
| `librime-copilot.dylib` | built from a librime checkout, copied into `Squirrel.app` by hand | — |

`copilot/rerank/llm/context_chars` is a **consumer declaration**, not an
independent knob: `CopilotRerankFilter` truncates the scoring context with it and
`Copilot::WarmRerankContext` keys the warm cache with it, and the two must be the
same value or every warm lands on a string nobody asks about. Both now read the
same config key and assemble the string through the single
`BuildScoringContextFor` (`src/rerank.h`); it used to be a hard-coded 32 in
`copilot.h` against a filter that read config, agreeing only by coincidence.

**Since 2026-08-28 it is also a term in the fetch depth**, which is what makes
the declaration mean anything: `SurroundingPrefixChars`
(`src/surrounding_source.h`) takes the max over the declarations of the
consumers that are actually on, and `copilot.cc` hands the result to all three
surrounding sources. Before that the sources stopped at 8 whatever the schema
said, so every value above 8 was indistinguishable — telemetry measured the
consequence directly, **71% of all fetches ending in `config`** ("the source
HAD more and `prefix_chars` cut it") on a machine configured for 32. How the
deployed scorer responds to the longer context is measured in the
context-length results record, kept locally: 8 → 64 is worth +2.47 points
(p=4.6e-07), 64 → unlimited +0.00.

Two things about that function are load-bearing. **The LLM term is gated on
`copilot/rerank/llm/enable` nested inside `copilot/rerank/enable`**, because
both re-ranking consumers live in `CopilotRerankFilter`, which returns early on
`!options_.enable` — declaring 32 characters that nothing reads would buy a
deeper per-keystroke query for nothing. And **each term is clamped before the
max, not after**, so one out-of-range key cannot raise the fetch on behalf of a
consumer that would itself have clamped down. `context_chars` used to be
clamped in `copilot.cc` and in `rerank_filter.cc` both, which it was not while
it could only ever truncate a string already fetched; since 2026-09-06 it is
clamped once, inside the single reader — see "One reader per config key" below.

Any `trunc_counts` figure from before 2026-08-28 describes the old fetch and is
not comparable with one after it.

**`status` is the check, and it exists because none of these fail loudly.**
A schema naming a `.gram` that is absent decodes as though no grammar were
configured (`octagram.cc:110` returns a constant); a schema naming a model
that is absent logs one load failure, never retries, and re-ranking silently
falls back to the db. Both look exactly like a working install from outside.

On a new machine, in this order — **`install` before `restore` is not
stylistic**. The vault gained `private/rime40m-q8.gguf`, and an installed CLI
whose `vault.py` predates that entry restores everything else and simply does
not know the model exists. It reports success. Same shape as the `dict.json`
`boost` hazard above.

```sh
# 1. source and tools first
git clone <librime>; cd librime; git clone <copilot> plugins/copilot
# BUILD_MERGED_PLUGINS=OFF is what produces lib/rime-plugins/ for step 4;
# librime defaults it ON and then there is no dylib to copy -- see "Build & lint"
cmake -B build -DCMAKE_BUILD_TYPE=Release -DBUILD_MERGED_PLUGINS=OFF
cmake --build build
# 2. installation.yaml needs `sync_dir` by hand -- Squirrel never writes it
plugins/copilot/tools/rime-copilot install
# 3. only now
~/Library/Rime/private/bin/rime-copilot restore   # config, lexicon, model (42MB)
rime-copilot fetch-grammar                        # 41MB, public download
rime-copilot update                               # sogou lexicon + prediction db
# 4. the plugin itself
sudo cp build/lib/rime-plugins/librime-copilot.dylib \
  "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/"
killall Squirrel                                  # if it was already running: the copy
                                                  # alone never reaches a live process
rime-copilot status                               # the check, not a formality
```

### The MLX backend

`copilot/rerank/llm/backend: mlx` scores on Apple's MLX instead of llama.cpp.
Measured on an M4 Pro through the real Scorer seam, interleaved, at the
deployed 100 ms idle: score p50 **10.38 → 5.33 ms (−48.7%)**, the two backends
agreeing to 0.0187 nats. Energy is indistinguishable (5.06 vs 5.21 mJ per
scoring, against a 20% round-to-round spread), and the whole feature is
~0.0014 Wh/h against a laptop's 8–15 W, so energy is not a reason either way.
The full record is the 2026-09-04 scoring-latency results, kept locally.

**It is off by default and the default is not a hedge.** The prefill is 36–50%
*slower* on MLX, so total work per composition favours it only above ~1.5
scorings per warm; the score win is on ~40% of keystrokes (69.5% engage the
model, 58% of those decode); and nothing in this tree establishes that 10 ms of
keystroke latency is perceived at all. What it costs is a permanent second
inference backend — llama.cpp cannot go, it is what Linux builds and what the
prediction provider uses — plus ~197 MB of runtime artifacts against a 5.8 MB
plugin.

**STATUS: it does not run from a source build.** Everything below about the
build works -- it configures, compiles, loads the model and passes every test --
and then the first GPU operation throws `There is no Stream(gpu, 0) in current
thread.` from `metal/device.cpp`'s thread-local command-encoder lookup. Ruled
out by measurement: MLX version (0.32.1 and 0.32.2 fail identically), static
versus shared linking, `$<LINK_LIBRARY:WHOLE_ARCHIVE>`, `$<LINK_ONLY>`, and the
metallib's location (beside the executable and beside libmlx.dylib both). A
minimal program against the same built libmlx does a matmul on the main thread
AND on a spawned thread, so the library and threading are not it; what differs
in the failing binary is that llama.cpp (with ggml-metal) and librime are in the
same process. Cause unknown. Do not switch `backend` to `mlx` on a
source-built plugin.

**And every performance figure quoted for this backend was measured against
MLX 0.32.1, not the 0.32.2 the build asked for.** The rpath fix changed the
link from a full path to `-L`/`-l`, and `/opt/homebrew/lib` precedes everything
added here -- so a Homebrew `mlx` formula supplied the library while
`/opt/homebrew/include` supplied the headers. Both were 0.32.1 and internally
consistent, so the numbers are not noise; they are about a version this file
named wrongly. That Homebrew copy has since been removed, and the build now
warns when a second MLX is installed. Treat -48.7% as a 0.32.1 figure that no
longer has a running build behind it.

Building it needs a pip-installed MLX for its headers and prebuilt libraries:

```sh
python3 -m venv ~/.local/share/rime-corpus/mlx-venv
~/.local/share/rime-corpus/mlx-venv/bin/pip install mlx
cmake -B build -DCOPILOT_WITH_MLX=ON -DBUILD_MERGED_PLUGINS=OFF \
  -DMLX_ROOT=~/.local/share/rime-corpus/mlx-venv/lib/python3.12/site-packages/mlx
```

The source build (`FetchContent`, with `MLX_BUILD_GGUF=OFF`) is what the
committed `CMakeLists.txt` does, and it is what does not run. It needs Apple's
Metal toolchain, `xcodebuild -downloadComponent MetalToolchain`, ~700 MB.

**`MLX_BUILD_GGUF=OFF` is the part that must survive whatever fixes the
runtime.** The pip wheel's `libmlx.dylib` EXPORTS 31 `gguf_*`/`ggml_*` symbols
-- MLX vendors its own ggml to read gguf -- against llama.cpp's 1150 of the same
names. Linked together, the dynamic linker binds llama.cpp's own model loader to
MLX's incompatible implementation whenever libmlx precedes the ggml archives on
the link line, and `llama_model_loader` dies with SIGBUS inside `gguf_get_key`.
Which one wins is pure link order; the first version of this backend happened to
order them the other way and ran. It was never right, only lucky. Turning the
option off removes the symbols instead of arranging for them to lose, and costs
`mx::load_gguf` -- hence `MlxScorer::LoadGgufWeights`, which reads the same file
through llama.cpp's gguf API and converts ggml Q8_0 into MLX's packed affine
form (`q_mlx = q_ggml + 128`, `scale = d`, `bias = -128d`).

Two expectations of the source build that did NOT hold: the metallib is still
174 MB (kernels are built for every GPU family, not the local one), and static
linking does not work (MLX registers its Metal backend from a static
initializer, which a linker pulling only referenced objects drops).

**Three files travel, and where they end up is not where `restore` puts them.**

```sh
rime-copilot restore --mlx          # -> ~/Library/Rime/private/mlx/  (transport only)
sudo cp ~/Library/Rime/private/mlx/* \
  "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/"
killall Squirrel
rime-copilot status                 # the `mlx:` line
```

MLX finds `mlx.metallib` next to the **binary that loaded it**
(ml-explore/mlx#2061 searches the binary's directory, then `Resources/`), and
that binary is `librime-copilot.dylib` inside `Squirrel.app`. No rpath reaches
it — rpath applies to dylibs, not to the metallib — so the Rime user directory
cannot be the answer and the `sudo cp` is not optional.

`libmlx.dylib` is found through `@loader_path`, which the build puts **first**
in the rpath list. It used to be second, after the absolute path CMake derives
from a full-path link — so a deployed plugin loaded MLX out of the build
machine's pip virtualenv, complete with its username and a `python3.12` that a
`pip install --upgrade` renames. Linking with `-L`/`-l` rather than a full path
is what stops CMake deriving that rpath and makes the order controllable. If
this is ever changed, check it with `otool -l ... | grep -A2 LC_RPATH` and by
moving the virtualenv aside, not by reading the CMake.

**A missing or mismatched `mlx.metallib` does not degrade.** MLX throws
`std::out_of_range` from inside its Metal device setup and takes the process —
Squirrel — with it. `MlxScorer::EnsureLoaded` checks for the file with `dladdr`
before touching MLX and disables the backend with a log line instead, and
`rime-copilot status` reports the same thing before anyone switches the backend
on. All three files must match the build: libmlx and libjaccl are what the
plugin was linked against, and the metallib is what libmlx loads.

### And if that machine is also a development machine

Every machine here is. Four more things, none of which travel by git:

```sh
# 1. the pinned interpreter -- see "tools/requirements.txt is generated" above.
#    .python-version is gitignored and MACHINE-LOCAL; name the venv what you like.
pyenv virtualenv system rime-copilot
~/.pyenv/versions/rime-copilot/bin/pip install -r tools/requirements.txt
echo rime-copilot > plugins/copilot/.python-version

# 2. the design records, which are gitignored (see "Where the design records live")
ln -s ~/Library/Mobile\ Documents/com~apple~CloudDocs/config/rime-copilot/superpowers-docs \
      plugins/copilot/docs/superpowers

# 3. confirm the setup, both suites this repo owns (the clients' Lua specs
#    live and run in rime-copilot-clients now)
ctest --test-dir build -R copilot_test --output-on-failure
python3 -m unittest discover -s plugins/copilot/tools/test -p '*_test.py'
```

**Fourth: the corpus travels by iCloud, and only the corpus does.**

```sh
mkdir -p ~/.local/share/rime-corpus
cd ~/.local/share/rime-corpus
ln -s ~/Library/Mobile\ Documents/com~apple~CloudDocs/config/rime-copilot/corpus/claude.jsonl .
ln -s ~/Library/Mobile\ Documents/com~apple~CloudDocs/config/rime-copilot/corpus/dingtalk.jsonl .
```

The split is deliberate and worth keeping. `~/.local/share/rime-corpus` also
holds every replay arm ever built — **9.8 GB** of derived rime-dirs, models and
binaries — while the corpus itself is two files totalling **972 KB**. Only
those two are irreplaceable, and only those two are synced; the arms are
rebuilt from `p1-both` or `rime-dir` whenever a measurement needs them.
`rime-corpus ingest` appends through the symlink, so a harvest on any machine
reaches all of them.

It is in iCloud rather than in git for the same reason the design records are:
it is the author's private messages, and the remote is public. `.jsonl` under
`~/.local/share/` was never in danger of being committed, but keeping the two
private things in one place, under one decision, is what stops the next one
from being got wrong.

Without those symlinks a machine can still build, test and change anything,
but **cannot reproduce any measurement in this file** — `rime-corpus kbest`,
`compare_rerank`, `compare_warmed` and `replay_copilot` all need the corpus,
and `rime-copilot personal` deliberately refuses to regenerate without it
rather than mining whatever fragment happens to be local.

**And a hazard that only exists once there is more than one of these.** Two
machines — or two agent sessions in one checkout — committing to the same
branch will interleave, and a `git add -A` on either sweeps up the other's
uncommitted files. That happened on 2026-08-22: a concurrent session's commit
absorbed 144 lines of an unrelated document under a message about something
else. `git worktree list` does not reveal it, because the other party is a
session, not a worktree. Check who else is working in a checkout before
assuming it is yours alone.

**Run `status` and read every line.** A full review of this on 2026-08-20
found five gaps on a machine that looked fine, four of which were silent:
unpushed commits, an installed CLI stale enough to miss the model, a vault
holding a day-old config, and a missing clean stamp. The fifth (`build:
recipe version 1 -> 2`) was visible only because `status` says so.

The vault-config one is the most dangerous and now explains itself: a
`conflict` on a file THIS machine last backed up means the edit has not
travelled, and `status` says so along with the command that fixes it. A
`conflict` naming another machine is a real conflict. They used to print the
same sentence.

The model is **named explicitly** in `vault.VAULTED_FILES`, so a retrain under
a different filename does not travel until that list is updated. That is the
safe direction: the second machine reports `model: missing` and names the file
the schema asked for, rather than silently running whichever older model
happens to still be there. `install-model --from PATH` remains for a machine
with no vault, or for trying a model before vaulting it.

then patch the schema (both commands print the exact lines), build the plugin
from a librime checkout, and copy the dylib into `Squirrel.app`.

### Held-out validation loss does NOT rank scorer checkpoints

**Every figure in this section is from the pre-2026-08-23 régime**, the same
one annotated in "The switches, and why they are switches" below:
`score_candidates` used to read `ctx` verbatim (unlimited, mean 28.4
characters) and prepend BOS. It now truncates to `--context-chars` (default
64) and aligns to the training form, dropping BOS, so a fresh run of this
ruler is not directly comparable with `hit 49.8%`, `+0.27 points at p =
0.669`, or any other number below — though the incumbent-pairing method
itself is unaffected. The confound here is alignment/BOS, not length: the
same 2×2 factorial that dated this annotation measured 64-vs-unlimited
context at +0.00, p=1.0 (the context-length results record, kept locally).

Measured over ten trained arms on 2026-08-22, the correlation between held-out
validation loss and downstream re-ranking hit rate is **r = -0.081**. A perfect
proxy would be -1.00. The arm with the WORST validation loss placed second on
hit rate; the arm with the best placed fourth. Full table:
the scorer-retrain results record, kept locally.

So a validation loss is worth having -- it says a run converged, and it costs
nothing -- but **selecting a checkpoint by it is selecting on noise**. The only
ruler that counts is `rime-corpus export-evalset` into `score_candidates`,
scored on buckets A and C together, paired against the incumbent on the SAME
exported file. A hit rate without a false-promotion rate is not a result.

This cost a full sweep's worth of wrong conclusions: "size is worth +0.117
nats", "6.0B tokens is worth +0.058", "lr 6e-4 beats 3e-4". On the real metric
lr 3e-4 wins at both sizes, 6.0B tokens is catastrophic at 40.9M (hit 49.8%)
while helping at 83.9M, and doubling the parameters buys +0.27 points at
p = 0.669 for 1.8x the p99 latency.

### The evaluation corpus is the binding constraint, and it has a lever

Every question left open in this area — the `noctx` gate below, the two
switches, any corpus or recipe change — is measured on the same replay corpus,
and at 3287 scoring units near 70% accuracy its 95% interval is about **±1.6
points**. Three corpora landing between 65% and 69% (S1) could not be told
apart by it. So "inconclusive" here has usually meant "the eval set is too
small", not "the change did nothing".

`rime-corpus ingest --since-days N --max-items M` is the lever, and the numbers
are worth knowing before the next person re-derives them: the DingTalk adapter
defaults to a **90-day** window and 1000 items per conversation, which yielded
996 of the user's own messages. At 730 days it yields **5712**. Harvesting that
took the corpus from 1349 utterances to 5428 (4.0x) and 31k Han characters to
95k (3.0x), which should bring the interval to roughly ±0.9 — confirmed by a
replay run, not by this paragraph.

An adapter with no time window (`adapters.claude`, which reads every local
transcript) declares no `DEFAULT_WINDOW_DAYS`, and `_ingest` prints that the
flag did not apply rather than dropping it silently. That distinction is the
whole point: a run that reports success having never applied the window it was
given is the failure mode this repo keeps meeting in other forms.

### The switches, and why they are switches

**Every swept figure below is from the pre-2026-08-23 régime** — measured with a
BOS-prefixed, sentence-EOS-free context, the same way the pre-2026-08-21 replay
engagement figures are separately annotated as unusable in "Numbers from replay
before 2026-08-21 are biased, not merely noisy" below. The scoring form changed
on 2026-08-23 (`AlignToTrainingForm`); the relative order of these results is
unlikely to invert, the absolute numbers no longer hold.

Each answers a question the corpus cannot settle, so each defaults to today's
behaviour and is left for real use — or a better instrument — to decide:

- **`copilot/rerank/llm/margin`** (default 2.0) — how much better a candidate
  must score before it is promoted. Swept on this model: harmful promotion
  0.8% at margin 2 against 2.8% at margin 1, for 13.3% net against 14.0%.
  **The shipped schema moved to 1.0 on 2026-08-28**, on the sweep's own net
  number plus the first live evidence there has been: telemetry recorded 11
  declines whose cause was this threshold, and in **9 of them the blocked pick
  is what the user then chose** (`decline_split`, the `blocked` bucket).
  1.0 recovers 3 of those, 0.5 recovers 6; 0.5 was not taken because the sweep
  has no measurement below 1. The code default stays 2.0. Note the accept rate
  before and after the change is not one quantity — every promotion in the log
  cleared whatever margin its machine was running.

  **That live evidence was a one-sided count, and the move was right by luck.**
  `decline_split` reported only what a lower threshold would WIN. What it would
  LOSE — the blocked declines the user resolved by taking the head — it never
  counted, and those are exactly the events `ShouldRecord`
  (`telemetry_event.h:235`) samples at 1 in `sample_ok`: it keeps every
  promotion and every miss in full, and one plain success in 20. So the two
  sides of the question were not being counted at the same rate, and the half
  that argued for the change was the fully-recorded one. A count that can only
  go up is not evidence.

  The outcome vindicated it anyway. Promotions ARE recorded in full, so the
  band the change unlocked is measurable without correction: `[1.0, 2.0)`
  promoted 99 times at a 66% accept rate against 71% for the `>= 2.0` band that
  was already promoting — net +65 / −34. That 66% is the one back-test this
  question has, and it dates both bounds: over the pre-2026-08-28 log the same
  band read **78% naive and 38% sampling-weighted**. Both were wrong; only the
  naive one was wrong in the direction that argued for the change.

  `analyze_telemetry.py` now prints the pair (`_print_threshold_table`), never
  either half, and checks the `sample_ok` it was told against the factor the
  stats lines imply (`implied_sample_ok` — the stats stream counts every
  segment, so the weight does not have to be trusted). **Read the range against
  the accept rate the promotions already achieve.** On the post-2026-08-28 log
  that is why 0.5 has NOT been taken: it would promote 79 more at an accept
  range of 18–64%, against the 69% the current promotions manage.
- **`copilot/rerank/same_span_only`** (default true) — whether a promotion may
  take a candidate covering a *different amount of input* than the one it
  displaces. False is worth +4.5 points of segments and costs p99 11.0ms →
  15.1ms, and changes how much input Space commits. (Both latencies are
  `bench_scorer` figures — see the bimodality note under "Neural re-ranking"
  for why a live p99 is roughly 2x either of them, and why the difference is
  one `llama_decode`, not context length.)
- **`copilot/rerank/llm/require_han_context`** (default true) — the Han-context
  gate. See "What is NOT built" below: it blocks ~28% of live segments, lifting
  it is safe, and it stays true on cost. The 2026-08-21 replay that established
  that was run in the 8-character régime, but re-checking it live in 2026-09-05
  **confirmed the default rather than overturning it**: the blocked population
  has LESS headroom than the rest, not more.
- **`copilot/rerank/llm/n_gpu_layers`** (default 99) and **`.../n_threads`**
  (default 0 = `hardware_concurrency()`) — where the model runs. Both were
  hard-coded on no measurement; both defaults measure right on an M4, but on
  **core time, not latency**: 1.93ms of CPU per prefill-plus-scoring on the
  GPU against 14.79 on four CPU threads, while score p50 is 2.55ms against
  1.89 — i.e. CPU-only is a shade *faster* and both are well inside the
  p99 < 10ms budget.
  `n_threads` is **inert** while the layers are on the GPU and decisive once
  they are not — 8 threads is worse than 4 on a 4-P-core machine, on both
  latency and core time.

**`copilot/rerank/llm/top_n` stays at 4, and since 2026-09-06 that is a
measurement rather than an inherited default.** It was known to beat 32 on
accuracy AND speed; nothing had ever measured BELOW 4, and the cost model above
prices each candidate at ~1.2ms — 13% of a deployed scoring — which makes this
the largest latency lever left in the scorer.

What it would cost is countable without a replay arm, because `llm.best_from`
records which same-span position the model's own top pick came from and
`ShouldRecord` keeps every promotion in full. Over 564 live engaged scorings:

| best_from | share | cumulative |
| --- | --- | --- |
| 0 | 49.3% | 49.3% |
| 1 | 37.6% | 86.9% |
| 2 | 9.8% | 96.6% |
| 3 | 3.4% | 100% |

Of 248 actual promotions, 183 came from position 1, 51 from position 2 and 14
from position 3. So **4 → 2 saves ~2.3ms (25% of a scoring) and gives up 26% of
all promotions**; 4 → 3 saves ~1.15ms and gives up 5.6%. Against a promotion
accept rate of 66-71% (see `margin` above) neither trade is worth taking, so
the default is unchanged — but it is now unchanged for a reason, and the same
two queries re-run the question on a new corpus or a new model.

Note this cuts the opposite way from `margin`'s one-sided count: `best_from`
covers every engaged scoring, promoted or not, so both sides of THIS question
are recorded at the same rate.

**`copilot/rerank/llm/battery_active` (default false) is the one whose default
is now known to be wrong.** Its README justification was "the model's CPU cost
is exactly the kind of thing a laptop should shed on battery"; measured, one
prefill-plus-scoring is ~3.9ms of GPU and ~1.9ms of CPU (a `bench_scorer`
figure; live, 56% of scorings pay a decode and cost ~11ms), so a dense 1000
commits an hour is ~0.012 Wh/h against a laptop's 8-15W — about 0.1% of
consumption, and the GPU is already awake rendering the candidate window at
exactly those moments. `true` is the recommendation; the default stays `false`
only because it is the shipped behaviour.

### What is NOT built

Whole-sentence decoding (Path A of
the neural-integration design record, kept locally) is designed
and deliberately not implemented: the user's priority is candidate ordering
given existing context, not long-sentence correctness.

The Han-context gate (`copilot/rerank/llm/require_han_context`, default true)
ends any segment whose trailing **Han** run is empty — a db constraint the model
does not share, since it reads punctuation and Latin. It was re-measured on
2026-08-21; the full record is
the rerank cost-and-gate results record, kept locally.

**That measurement was made with the model seeing 8 characters** — on
2026-08-21 the fetch depth was
`max(copilot/surrounding_context_chars, copilot/rerank/max_context_chars)`,
both defaulting to 8 (`copilot.cc:100-108` at `66cea91`);
`copilot/rerank/llm/context_chars` did not become a term in it until
2026-08-28, and the deployed value did not reach 64 until 2026-09-04. That is
the same stale-régime problem the margin sweep and the pre-2026-08-21 replay
figures carry, and it looked like the largest un-annotated one in this file,
because `noctx` is a stable 27–30% of all live segments.

**Re-checked live on 2026-09-05, it confirms the default instead.** No replay
arm is needed and none would help — see below. A gated segment is identifiable
in the log by `ctx == ""` (`trace.ctx` is exactly the `TrailingCjkRun` the gate
tests), and `ShouldRecord` keeps every `sel_idx != 0` in FULL, so a miss rate
over that population needs no sampling correction. Over the 2026-08-31 →
2026-09-05 window, 6248 segments, `noctx` 1786 (28.6%):

| population | miss rate (user reached past the head) |
| --- | --- |
| gated by `require_han_context` (1786 segments, NOT re-ranked at all) | **2.58%** (46) |
| everything else (4462 segments, already re-ranked) | 3.18% (142) |

**The blocked population is the easier one.** Even un-re-ranked it misses less
often than the re-ranked remainder, and the ceiling is flat arithmetic: 46
misses in 6248 segments is **0.74% of all segments even if re-ranking fixed
every one of them**, against a realistic conversion well under half that. The
misses there are also the wrong shape for a language model — 38 of 46 are
`sel_idx: 1` and 29 of 46 have a two-character (one-syllable) input, i.e.
single-character homophone choices with no preceding text, which is the
population a context model has least to say about.

**And the replay harness could not have answered this anyway.** `evalset.py`
exports only segments whose context ends in Han — the gate-blocked population
is excluded from the ruler by construction, and for a good reason stated
there: `replay_copilot` re-pushes real surrounding context only for segment 2+
of a request, so in the corpus "no trailing Han run" is very nearly "no
context at all", which is a property of how requests are cut at maximal Han
runs rather than of the deployed 64-character fetch. Live telemetry is the
only instrument that sees this population with a real context behind it.

**Its safety question is settled: lifting it cannot repeat the 2026-08
revert.** That revert happened because bucket B+D fell from 43.8% to 13.4% —
promotions displaced the raw-input head `RawInputFilter` used to place first.
`b64ef1c` moved those keystrokes to the last slot of the page or nowhere, and
**bucket B is now 4 segments of 27245 (0.0%)**, identical in both arms across
two independent full runs. The head is not there to displace.

**What it costs is large, and what it buys is nothing.** With the gate on the
model is consulted on 10923 of 27245 segments (40.1%); lifted, 22604 (83.0%).
`noctx` with the gate on is **16322 — exactly the request count**: the gate
blocks every request's segment 0 and nothing else. Lifting it recovers 11681 of
those, exactly the requests that have preceding context; the other 4641 have
none, so the model could not have used them either. **The gate blocks 42.9% of
all segments.**

Doubling the model's opportunities changes the output on 75 segments of 27245 —
GAIN 44 / LOSS 31, net +13, McNemar p=0.17, every example at `pos: 0`. The
corpus resolves ±0.59 points at segment level, so this is not a resolution
problem: where the model newly gets to look, it agrees with the existing head.

So the default stays `true` on a **cost** argument, and that is the first real
reason it has ever had: lifting it costs 2.07x the scorings (10923 → 22604) at
~3.9 ms of GPU and ~1.9 ms of CPU each, for no measurable change in output.
Both earlier reasons are dead — "bucket B would collapse" is refuted above, and
"we cannot see where it acts" was fixed in `421bda3`.

### Numbers from replay before 2026-08-21 are biased, not merely noisy

`ObserveLlm` decided "a fresh trace landed" from a `RerankTraceStore::size()`
delta. That store is a bounded deque of `kMaxEntries = 16`, so `size()`
saturates and **every segment past the 16th trace was labelled `notrace`** —
measured, 100% of the 148 segments past 16 fed keys, against 57% below it. An
in-place re-record (which `Apply()` does on every keystroke) blinds it below 16
too. Fixed by a monotonic `records()` counter (`f54cec1`).

This moved the engagement rate from **11.9% to 40.1%**, so the figure this file
used to carry — "the model is consulted on 8.8% of segments" — was wrong by
more than 4x. Worse than wrong: the surviving sample was biased toward SHORT
requests, i.e. the ones carrying least context and where the model has least to
offer. Treat any pre-2026-08-21 replay engagement or `llm_skip` number as
unusable rather than approximate.

Segment 0 was blind for a second reason and was fixed separately (`421bda3`):
`ProcessRequest` feeds every key of a request before the loop that captures the
count, so segment 0's trace is already written when the window opens. Capturing
earlier is NOT the fix — during key feeding `Apply()` runs for every live
segment on every keystroke, so `Last()` would name a later segment. The query
that works needs no window: `RerankTraceStore::FindLatestByStart(start)`, valid
for segment 0 because its span starts at 0 under every segmentation, so it needs
no conversion between the tool's key positions and librime's input offsets and
no guess at `end` (both move as a segment is re-recorded; `start` does not).

**`notrace` is now zero** — every length bucket, both arms, the whole corpus.
Any future replay number that shows a `notrace` bucket at all is a regression in
this machinery, not a property of the data.

### The scoring form has two implementations and one truth

`rime_train/normalize.py`'s `scoring_form` is the Python side, checked by
`rime_train_test.py` against the real training chain (`normalize` +
`text_sentences`); `src/scoring_form.h`'s `AlignToTrainingForm` is the C++ side,
checked by `test/scoring_form_test.cc` against a committed golden fixture that
`rime-train scoring-form` emits. The two functions do not have the same shape —
one folds a string, the other splits and discards — so they cannot be diffed
directly, which is why the agreement is pinned in two hops rather than one.
**The fixture's inputs are hand-written in `tools/rime_train/goldens.py` and must
stay that way**: generating them from the evaluation corpus would commit the
author's private messages to a public remote.

Why `src/scoring_form.h` is a header separate from `rerank.h` — so that
`score_candidates` can reach it without pulling in `rerank.h`'s
`<rime/candidate.h>` — is explained in full in "Tools (`tools/`)" above,
alongside the linkage invariant that requires it.

## Propagating a change to a machine that already has this

A new machine follows the bootstrap above. An existing one is a different
problem, because a change lands on **three channels that do not talk to each
other**, and most changes touch more than one:

| what changed | how it travels |
| --- | --- |
| `tools/` (the CLI) | git, then `install` |
| `src/` (the plugin) | git, then a **rebuild and a hand-copied dylib** |
| `*.custom.yaml`, the lexicon, `dict.json`, `private.dict.yaml` + `private/personal.dict.yaml`, the model | the vault, via `backup` / `restore` |
| the **user dictionary** (`private.userdb`) | Rime's own user-data sync, from Squirrel's menu — NOT the vault |
| the design records (`docs/superpowers/`) | iCloud, plus a symlink made by hand |
| the **clients** (Neovim, the tmux reporter, including on a remote host reached over ssh) | `rime-copilot-clients`, via each ecosystem's own plugin manager (lazy.nvim, TPM) — not a channel this repo has any part in any more. **Order still matters across the boundary**, and it does not follow the plugin managers' own schedule: the dylib must reach the laptop before a remote's clients plugin is upgraded past the point where it started sending the identity message's `host` field, or an old handler reads it as an unknown key and ignores it, silently filing that remote pane's `ascii_mode` into the laptop's own local-pane memory. See "Remote tmux" in `README.md` |
| the **MLX backend's runtime** (`libmlx.dylib`, `libjaccl.dylib`, `mlx.metallib`) | the vault, via `backup --mlx` / `restore --mlx` — **then a `sudo cp` beside the plugin**, which `restore` cannot do. ~197 MB, and off by default, which is why it is a separate group from `VAULTED_FILES` rather than part of it. See "The MLX backend" below |
| the **telemetry** (`private/copilot_telemetry/`) | `<sync_dir>/copilot_telemetry/` — `tools/sync_telemetry.sh` by hand, or `copilot/telemetry/auto_sync: true` on a 30-min timer. NOT the vault, and never merged: one file per machine, so collecting is concatenation |

**The telemetry filename is `installation_id`, and it used to go stale.** The
name reaches the log twice: `CopilotEngineComponent::GetTelemetryWriter` reads
`deployer.user_id` **once**, when it builds the process-wide `telemetry::Writer`
(the path is derived from it in the constructor), while
`Copilot::EmitCommitTelemetry` re-reads it on **every commit** for the line's
own `machine` field. A comment claimed the two therefore "never disagree". They
disagreed for four days: a laptop whose `installation_id` was corrected
`MacBookPro-M1` → `MacBookAir-M4` kept appending to `MacBookPro-M1.jsonl` — a
redeploy rebuilds processors, not the process — while stamping every new line
with the new name. Found on 2026-08-28 because that file held 231 lines saying
one machine followed by 221 saying the other, timestamps monotonic across the
flip, ending 70 seconds before the correctly-named file began (the restart).

Both sites now derive it from one `telemetry::MachineName` (`src/telemetry.h`),
and `GetTelemetryWriter` **rebuilds** the writer when the deployer no longer
agrees, so the filename follows the config. The old writer stays alive in
whatever `Copilot` still holds it and flushes its tail to the old file, which
is where that data belongs.

Two consequences worth knowing. **A machine's local
`private/copilot_telemetry/` can hold another machine's file** — its own data
under the pre-rename name — so `sync_telemetry.sh` publishes **only**
`<installation_id>.jsonl`, matching what `SyncToDir` already did, and names
any others without copying them. It globbed `*.jsonl` until 2026-08-28, which
would have republished the stale name over a shared copy that had since been
corrected. And **the analyser falls back to the FILENAME for a line with no
`machine` field** (`stats` lines have none), so a wrong filename mislabels
those too — 126 stats lines rode along with the 162 events in that incident.

`tools/merge_renamed_telemetry.py` folds a stale-named file into the current
one, relabelling `machine` and keeping the old value in `machine_was` so the
correction is legible and reversible from the merged file alone. Three of its
refusals are the lessons that produced it, each learned by getting it wrong:

- **Repair the machine-LOCAL file, never the shared copy.** The shared copy is
  a *projection*: every sync, manual or `auto_sync`, overwrites it wholesale.
  The first attempt at this merge was made on the shared file and lasted 10
  minutes — the Air's next sync replaced 603 merged lines with its own local
  156 — so the tool refuses a destination that looks like a sync directory.
- **Squirrel must not be running.** `telemetry::Writer` holds an open fd and
  appends through it; replacing the file by rename leaves that fd on the
  orphaned inode, so every later commit is written where nothing will read it,
  silently, until the process restarts. The tool refuses rather than trusting
  the operator to remember, and says to restart afterwards.
- **The old name cannot always be read off the filename.** A file renamed out
  of the way no longer ends in `.jsonl`, so the derived name matches nothing,
  every line passes through still attributed to the machine that never typed
  them, and the line count looks right — the same failure the tool exists to
  repair, reintroduced by the repair. It refuses when nothing matched while
  the file plainly names another machine, and `--stale-name` is the override.

The shared `MacBookPro-M1.jsonl` was merged into `MacBookAir-M4.jsonl` on
2026-08-28; the original is kept beside it. **The real M1 has never reported.**

The user dictionary is the one people get wrong, because it has its own
channel and its own precondition: syncing brings the merged history down, but
a machine still running a dylib from before 2026-08-22 **will not add to it** —
AutoSpacer's commits never reach `Memory::OnCommit` there. Sync on such a
machine produces a dictionary that looks populated and then never grows, which
reads as "already working". Copy the dylib and restart Squirrel first, confirm
learning is live (see "AutoSpacer's commits and Rime's user dictionary"), and
sync after.

The dylib is the channel with no automation, and it is the one carrying every
C++ change. `git pull` and `restore` both succeeding says nothing about
whether the plugin's behaviour changed on that machine. This matters more now
than it used to: the clients (Neovim, the tmux reporter) update themselves
through their own plugin managers on their own schedule, so on a machine
where everything else keeps itself current, the dylib is the one thing left
that does not — and the one most likely to be forgotten precisely because
everything around it looks automatic.

**Before that: building the tests is not building the dylib.** `cmake --build
build --target copilot_test` is the fast loop and it is what you will run all
day. It does **not** produce `build/lib/rime-plugins/librime-copilot.dylib` —
that is a different target. So a session can end with a green suite, a clean
`status`, and a `cp` of a dylib that predates every change it just made. This
happened on 2026-08-30, to the same person who was writing the verification
checklist at the time.

**And the timestamp will not catch it.** The installed copy is newer than the
build artifact in exactly this case — you copied it more recently than you last
built it — so `stat` on both looks correct and `ps -o lstart` against the
install looks correct. The only check that fails is one that asks whether the
binary contains something only the new code has:

```sh
cmake --build build -j8          # ALL targets, not --target copilot_test
ls build/lib/rime-plugins/librime-copilot.dylib   # absent => BUILD_MERGED_PLUGINS
                                                  # was ON; see "Build & lint"
strings "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/librime-copilot.dylib" \
  | grep -c "<a string added by the change you are shipping>"
```

Pick a string the change introduces — a new log line, a new config key name.
Zero means the file predates your work, whatever its mtime says. This is the
same class as the three below, and it is the one that has no timestamp
signature at all.

**The plugin's own log is not a record until something forces a flush.** The
diagnostics in `/var/folders/.../rime_copilot.*.log.INFO.*.log` go through
glog, which buffers and drains only when the *next* message is written. A quiet
plugin therefore leaves its most recent lines invisible — and the tail you read
is routinely truncated mid-line, which looks like the end of the story. On
2026-08-31 that produced three reversed conclusions about one question in a
single session, because the missing lines were each time exactly the ones whose
absence was being read as evidence (`[ImeBridge] Server stopped.` was buffered;
its absence was taken to mean `Stop()` had not run). `killall Squirrel` makes it
worse: the buffer dies with the process, so investigating by restarting destroys
the record you were about to need.

Two ways out, and prefer the second:

- **Force a drain** by generating traffic the plugin logs — a redeploy emits
  several unconditional `LOG(INFO)` lines, and anything written to the IME
  bridge socket does too. Then re-read.
- **Corroborate outside glog.** What actually settled the 2026-08-31 question
  was `lsof -p "$(pgrep -x Squirrel)"` showing zero unix sockets held, which
  proves `server_fd_` was closed and hence that `Stop()` ran — no log required.

A corollary worth knowing on its own: **a redeploy leaves
`/tmp/rime_copilot_ime.sock` absent until the next keystroke.** The refcount
reaches zero, `Stop()` unlinks, and `Start()` binds again only when the next
Rime session is created. That window is normal. Every client must tolerate it;
`rime-copilot-clients`' tmux reporter (`scripts/report.sh`) does, with
`[ -S "$SOCK" ] || exit 0`.

**And copying the dylib is not loading it — the running Squirrel has to be
killed.** A process maps its dynamic libraries at launch and never re-reads the
files; Rime's redeploy re-reads *config* and re-creates the processors, not the
binary. So every line of the recipe below can succeed — `git pull`, `cmake
--build`, `sudo cp`, `deploy`, a clean `status` — while the machine goes on
running whatever C++ it started with, which on a laptop that sleeps rather than
shuts down is weeks old.

What makes this worse than a plain no-op is that a change spanning both the
dylib and the vault **half-lands, and the half that works argues the other half
did too.** Measured on 2026-08-27: `auto_sync` shipped as code (`74c0fe3`)
alongside `copilot/telemetry/auto_sync: true` as config. The deploy took the
config, so telemetry visibly started recording with the new `top_n: 10` — while
`Copilot::SyncTelemetry` was not in the running image at all, and
`<sync_dir>/copilot_telemetry/` held no file from this machine for nine hours.
Nothing anywhere said so; the local `.jsonl` growing is exactly what a working
install looks like.

The cheap check is the schema version every telemetry line carries:
`kSchemaVersion` (`src/telemetry_event.h`) against `"v":` in
`private/copilot_telemetry/<machine>.jsonl`. Read the **last** line, not any
line: the file is appended to for months, so every long-lived one carries a
spread of old versions — measured 2026-08-27, `{2: 3, 3: 26, 5: 3}` on a
machine that was running v5 the whole time, and a plain `grep '"v":'` there
reads as a stale image when nothing is stale. `tail -1` dates the running
image without guesswork. Failing that,
`ps -p "$(pgrep -x Squirrel)" -o lstart` beside the dylib's mtime — that is what
settled this one.

**One-off, for any checkout made before 2026-08-22: `git pull` will not work.**
The remote's history was rewritten and the repository deleted and recreated on
that date (`docs/superpowers/` carried the corpus verbatim into a public repo —
see "Where the design records live"). An old clone shares no commit with the
new remote, so `pull` refuses to merge unrelated histories.

Check for local work first, because the recovery discards it:

```sh
git log --oneline origin/master..HEAD   # anything here is about to be lost
git fetch origin && git reset --hard origin/master
```

Then recreate the docs symlink, which is gitignored and so does not travel:

```sh
ln -s ~/Library/Mobile\ Documents/com~apple~CloudDocs/config/rime-copilot/superpowers-docs \
      <librime>/plugins/copilot/docs/superpowers
```

After that, the ordinary recipe:

```sh
cd <librime>/plugins/copilot && git pull
cd <librime> && cmake --build build          # C++ changes live here and nowhere else
plugins/copilot/tools/rime-copilot install   # BEFORE restore -- see below
~/Library/Rime/private/bin/rime-copilot restore
                                             # prints `conflict` and SKIPS any vaulted
                                             # file this machine has edited -- which is
                                             # every propagated config change. Read
                                             # status, then `restore --force`.
sudo cp build/lib/rime-plugins/librime-copilot.dylib \
  "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/"
killall Squirrel                             # the cp above changes nothing until this
rime-copilot update                          # regenerates private/personal.dict.yaml,
                                             # then rebuilds and deploys
rime-copilot status                          # read every line
```

`update` rather than a bare `deploy` since `private.dict.yaml` began naming
`private/personal`: on a machine that restored the pair, `update` is a no-op
for that file (it refuses to regenerate without a corpus — see above) and
still rebuilds the predict db. On the machine that *has* the corpus it is what
keeps the vocabulary current.

`install` before `restore`, for the same reason it comes first on a new
machine: a restored file can name things an older CLI does not understand.
When the schema patch moved `copilot/rerank/llm/model` into nested form, a CLI
predating that read `model: unconfigured` on a machine where the model was
present and working — librime resolves either form, so the *plugin* was fine.
The report was the lie, and acting on a lie like that is the damage.

**`restore` will refuse on any vaulted file that machine has edited, and that
is correct.** Local content the vault does not know is a conflict, not
something to overwrite. Read `status` first to confirm the local copy really
is just the not-yet-updated version, then `restore --force` to take the
vault's. Never reach for `backup --force` to clear it: that pushes the stale
local copy up over the change being propagated, which is the exact shape of
the June-2025 lexicon incident above.

## Clients

The Neovim client and the tmux reporter both live in
[rime-copilot-clients](https://github.com/lidongpeng36/rime-copilot-clients)
now, each installable by its own ecosystem's plugin manager (lazy.nvim, TPM) —
neither was, sitting four directories inside this C++ repo. This repo keeps
only the end of the wire they talk to: `ImeBridgeServer`
(`src/ime_bridge.{h,cc}`) and the protocol it speaks — the greeting, the
identity message, `ascii_mode` set/restore. The two sides were never testable
together, because the handler lives behind a real Rime engine the other repo
has no way to stand up, so the contract is kept in step by two goldens that
name each other, the same arrangement `MakeKey` and `MakeClientKey` already
use (`src/context_memory.h:45-59`): the literal wire-format tests at the
bottom of `test/ime_bridge_state_test.cc` name that repo's `scripts/report.sh`
and `test/report_test.py`, and that repo's golden names this file back.
Bumping `kProtocolVersion` (`src/ime_bridge.cc`) is therefore a coordinated
change across both repos; a version mismatch is inert, never wrong —
`ProcessMessage` logs a warning and ignores the message.

## Deployment / config
Users add `copilot` to `engine/processors` **before `ascii_composer`** — not
merely before `key_binder`. `AsciiComposer` returns `kRejected` for letter
keys while `ascii_mode` is true (librime `ascii_composer.cc:129-140`) and the
engine breaks the processor loop on `kRejected` (`engine.cc:104-105`), so a
`copilot` ordered after it never runs in English mode at all. Context memory
is then dead in the English-to-Chinese direction only, which reads as
flakiness rather than as misconfiguration; the constructor logs a
`LOG(WARNING)` when it detects this. Also add `copilot_translator`
to `engine/translators`, plus a `copilot` switch. Full schema-config reference (db path,
`max_candidates`, `max_iterations`, LLM `model`/`n_predict`, `ime_bridge`, `auto_spacer`)
lives in `README.md`.

### One reader per config key

`ReadCopilotSharedConfig` / `ReadTelemetryOptions` (`src/copilot_config.{h,cc}`)
are the only readers of every `copilot/*` key that more than one component
needs. Before 2026-09-06 eight groups of keys had two or three independent
readers, each with its own spelling of the path, its own default, and — for
`rerank/max_context_chars` and `rerank/llm/context_chars` — its own clamp:

| key | readers it used to have |
| --- | --- |
| `copilot/db` | filter + engine component |
| `copilot/rerank/enable` | processor + filter + engine component |
| `copilot/rerank/max_context_chars` | processor + filter (both clamped) |
| `copilot/rerank/llm/enable` | processor + filter + engine component |
| `copilot/rerank/llm/model` | filter + engine component |
| `copilot/rerank/llm/battery_active` | processor + filter |
| `copilot/rerank/llm/context_chars` | processor + filter (both clamped) |
| `copilot/telemetry/*` | processor ctor + `CopilotComponent::Create`, byte-identical |

A key with a single reader — `tmux_source/*`, `context_memory/*`,
`disabled_plugins`, `surrounding_context_chars`, `llm/*`,
`rerank/{window,max_rank,same_span_only}`, `rerank/llm/{top_n,margin,…}` — is
deliberately still read where it is used. It cannot disagree with itself, and
moving it would only put distance between the read and the use. The check is
one line:

```sh
grep -rn 'config->Get[A-Za-z]*("copilot' src/   | sed -E 's/.*"(copilot[^"]*)".*/\1/' | sort | uniq -c | awk '$1>1'
```

Empty output is the invariant. A key that appears there has grown a second
reader and belongs in `copilot_config.h` instead.

**It is deliberately NOT cached**, and that is the part to not "fix". Caching
by `schema_id` is the obvious shape — `CopilotEngineComponent` already keys two
maps that way — and it is wrong here: a redeploy changes the config while the
`schema_id` stays put, so the cache would serve the pre-deploy value forever.
`copilot_engine_by_schema_id` escapes that only because it holds a `weak<>`
that dies with the instance, which a plain value cache has no analogue for. So
each component calls the reader at its own construction time and the keys are
read two or three times — a YAML map lookup, at construction, off the critical
path. **The goal was never to read once; it was to make the readers unable to
disagree**, and one function is what does that.

`test/copilot_config_test.cc` is the only test in the tree that drives real
config, and it does so without breaking the "no Rime engine in tests" rule:
`rime::Config` has a public default constructor and an exported
`LoadFromStream`, so it takes YAML strings directly. It pins every default,
every key, both clamps at both ends, and the FLOW MAP form Rime's deployer
writes into `build/*.schema.yaml` (`llm: {battery_active: true, …}`) — which is
what the plugin actually reads at runtime, and a shape `_config_leaves` had to
be taught separately on the Python side.

### Flat vs nested keys in a `.custom.yaml` patch

A patch key whose value is a map REPLACES that node wholesale
(`config_compiler.cc` `EditNode`: without a `/+` suffix `merging` is false, so
`*target = value`). So `"copilot/llm/enable": false` and a nested `copilot:` →
`llm:` → `enable:` are both correct for `copilot` — the patch already owns
that whole node — but writing `grammar:` or `translator:` as a map would drop
every other key the schema defines under them, silently. Those stay flat.

Patch entries are a `map<string, ...>` (`config_types.h:89`), so they apply in
**lexicographic key order, not file order**. `copilot` sorts before
`copilot/llm/enable`, which is the only reason a file mixing both forms worked
at all. One nested block has no ordering to get wrong.

Anything reading these files by scanning lines must handle three shapes: the
flat key, the nested block, and the FLOW map Rime's deployer writes into
`build/*.schema.yaml` (`llm: {..., model: "...", ...}`). `_config_leaves`
(`tools/rime_copilot/cli.py`) is the one that does; `status` reported a
working model as `unconfigured` for want of it.
