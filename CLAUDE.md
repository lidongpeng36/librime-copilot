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

## Build & lint

Mirror CI (`.github/workflows/ci.yml`) — check out librime and nest this repo under `plugins/copilot`:

```sh
# from a fresh librime checkout, with this repo at plugins/copilot
./action-install-linux.sh                                    # deps (Linux)
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DENABLE_ASAN=ON
cmake --build build
```

- **llama.cpp** (tag `b10456`) and **nlohmann_json** are pulled via CMake `FetchContent` — the first configure downloads and builds llama.cpp, which is slow.
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
- **Lua tests** run under bare `nvim -l`, no framework:
  ```sh
  nvim -l clients/neovim/test/endpoint_spec.lua   # pure helpers
  nvim -l clients/neovim/test/verify_spec.lua     # real sockets, timers, handle cleanup
  ```
  `verify_spec` drives the actual connection path against fake bridges. It runs each scenario
  in a **child nvim**: reloading the module leaves the previous instance's sockets and timers
  alive in the event loop, and they go on talking, silently invalidating the next scenario.
  Both specs load the module under test via explicit path (`loadfile` / `package.preload`),
  never `require` — nvim's runtimepath loader resolves `rime_ime` to the copy installed under
  `~/.config/nvim` first, so on a machine that has the plugin installed a plain `require`
  tests the *installed* code. That has produced falsely-green runs twice; do not undo it.
  CI runs lint + both Lua specs + build-with-ASAN + the GTest suite.

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

## Tools (`tools/`)

C++ tools, built when `BUILD_TOOLS=ON` (default):
- `build_copilot` — builds the prediction DB. Reads stdin lines of
  `key text weight`, writes the DB file (arg 1, default `copilot.db`).
  Lands at `<librime>/build/plugins/copilot/bin/build_copilot`.
- `dump_copilot` — read-only probe: prints a key's continuations with weights,
  ranks and duplicate counts. First stop when candidates come out in a
  surprising order (`dump_copilot <db> --find 议 -- 建`).
- `ax_poc.mm` — macOS Accessibility API proof-of-concept (Apple-only).

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
| `cli.py` | orchestration |

Subcommands: `status`, `restore`, `backup`, `fetch`, `build`, `deploy`,
`update`, `install`, `clean`. A new machine builds `build_copilot` from a
librime checkout, runs `install` once, then `restore` and `update` from the
installed `private/bin/rime-copilot` — no checkout needed after that.

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

**What travels where.** The lexicon, its pristine `.raw`, the clean stamp and
`dict.json` are vaulted (`vault.py:VAULTED_FILES`) and move by iCloud;
`private.predict.db` and `sogou.dict.yaml` are derived and rebuilt locally;
the CLI moves by git plus `install`; `jieba` and `pypinyin` move by neither —
`status` names any that are missing, what they break, and the `pip` command.
`.copilot_clean_stamp.json` is vaulted while `.copilot_build_stamp.json` is
not, and the asymmetry is deliberate: the build stamp describes a locally
built database, the clean stamp describes a shared file. Without the clean
stamp in the vault a second machine reports `lexicon: not cleaned`, and
acting on that report replaces the pristine export with an already-cleaned
copy and then pushes it over the genuine one.

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
(`tools/rime_train/`, see `docs/superpowers/specs/2026-08-19-corpus-pipeline-design.md`);
42MB at Q8_0, p50 4.7ms / p99 11.0ms per scoring.

Three artifacts have to be present:

| artifact | how it travels | check |
| --- | --- | --- |
| `private/rime40m-q8.gguf` | **vaulted** — `rime-copilot restore` brings it down; `install-model --from PATH` is the manual route | `rime-copilot status` → `model:` |
| `private/zh-hans-t-essay-bgw.gram` | `rime-copilot fetch-grammar` (public download) | `rime-copilot status` → `grammar:` |
| `librime-copilot.dylib` | built from a librime checkout, copied into `Squirrel.app` by hand | — |

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
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build build
# 2. installation.yaml needs `sync_dir` by hand -- Squirrel never writes it
plugins/copilot/tools/rime-copilot install
# 3. only now
~/Library/Rime/private/bin/rime-copilot restore   # config, lexicon, model (42MB)
rime-copilot fetch-grammar                        # 41MB, public download
rime-copilot update                               # sogou lexicon + prediction db
# 4. the plugin itself
sudo cp build/lib/rime-plugins/librime-copilot.dylib \
  "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/"
rime-copilot status                               # the check, not a formality
```

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

### The two switches, and why they are switches

Both answer questions no corpus can settle, so both default to today's
behaviour and are left for real use to decide:

- **`copilot/rerank/llm/margin`** (default 2.0) — how much better a candidate
  must score before it is promoted. Swept on this model: harmful promotion
  0.8% at margin 2 against 2.8% at margin 1, for 13.3% net against 14.0%.
- **`copilot/rerank/same_span_only`** (default true) — whether a promotion may
  take a candidate covering a *different amount of input* than the one it
  displaces. False is worth +4.5 points of segments and costs p99 11.0ms →
  15.1ms, and changes how much input Space commits.

### What is NOT built

Whole-sentence decoding (Path A of
`docs/superpowers/specs/2026-08-20-neural-integration-design.md`) is designed
and deliberately not implemented: the user's priority is candidate ordering
given existing context, not long-sentence correctness.

The model is consulted on 8.8% of the replay corpus's segments, 68.2% skipped
as `llm_skip=noctx`. That is largely a corpus artifact — the harness splits
requests at maximal Han runs, so the character before every run is by
construction not Han, while real typing continues after committed Chinese.
Lifting that gate was tried and reverted: it reaches exactly the segments
where `RawInputFilter` had put the raw keystrokes first, and promoting there
displaces that head (bucket B+D fell from 43.8% to 13.4%). `Decide` enforces
"never touch bucket B" when choosing *which* candidate wins but nothing
enforces it on where the winner is *inserted*. See the comment guarding the
early return in `rerank_filter.cc`.

**That measurement's premise has since changed and the gate has not been
re-measured.** `RawInputFilter` no longer puts the raw keystrokes first in any
segment (`b64ef1c` — they go to the last slot of the page, or nowhere), so the
head those promotions displaced is not there any more. Whether the gate is
still earning its 68.2% is now an open question rather than a settled one; it
was left in place deliberately, because re-deciding it needs its own numbers,
not an inference from this paragraph.

## Propagating a change to a machine that already has this

A new machine follows the bootstrap above. An existing one is a different
problem, because a change lands on **three channels that do not talk to each
other**, and most changes touch more than one:

| what changed | how it travels |
| --- | --- |
| `tools/` (the CLI) | git, then `install` |
| `src/` (the plugin) | git, then a **rebuild and a hand-copied dylib** |
| `*.custom.yaml`, the lexicon, `dict.json`, the model | the vault, via `backup` / `restore` |

The dylib is the channel with no automation, and it is the one carrying every
C++ change. `git pull` and `restore` both succeeding says nothing about
whether the plugin's behaviour changed on that machine.

```sh
cd <librime>/plugins/copilot && git pull
cd <librime> && cmake --build build          # C++ changes live here and nowhere else
plugins/copilot/tools/rime-copilot install   # BEFORE restore -- see below
~/Library/Rime/private/bin/rime-copilot restore
sudo cp build/lib/rime-plugins/librime-copilot.dylib \
  "/Library/Input Methods/Squirrel.app/Contents/Frameworks/rime-plugins/"
rime-copilot deploy
rime-copilot status                          # read every line
```

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
- `clients/neovim/lua/rime_ime/` — Neovim client for the IME Bridge, one
  self-contained module directory so it can be synced to a remote host as a
  unit. `init.lua` is the stateful part (connection lifecycle, autocmds,
  protocol); `endpoint.lua` is pure logic (endpoint parsing, discovery order,
  tunnel-port enumeration, greeting parsing, backoff, instance identity) with no
  `vim.*` reference, which is what lets `endpoint_spec.lua` run under bare
  `nvim -l`. Keep it that way — anything stateful belongs in `init.lua`.

### Remote Neovim, and why the client verifies who answers
The client can run on a machine that is not the one Rime is on, reached through
`RemoteForward` in the user's `~/.ssh/config`. Two facts drive the design, both
measured (see `docs/superpowers/specs/2026-08-05-nvim-remote-identity-design.md`
for the full record and the alternatives that were eliminated):

- **sshd never unlinks a forwarded socket file** — not on `ssh -O exit`, not on
  SIGKILL — and refuses to bind over one, so the socket-file form of the tunnel
  works once per host and is dead after. Hence a forwarded **port**.
- **Two laptops on one remote account** each get a tunnel to a *different* IME,
  indistinguishable from the far end. Hence `RemoteForward 127.0.0.1:0` (a
  private port per laptop, no coordination) plus a greeting: `ImeBridgeServer`
  writes one `type:"hello"` line naming its host the instant it accepts, and the
  client keeps only the tunnel whose host matches `$LC_RIME_IME_HOST` (delivered
  by `SetEnv LC_RIME_IME_HOST=%L`).

Invariants to preserve when touching this:
- The greeting must be **unprompted** — a client must never have to send a
  keystroke to discover it reached the wrong machine.
- A connection that sends no action must register **no** client state. Discovery
  probes other laptops' tunnels; if a silent probe registered a client, its
  disconnect would synthesize a reset and flip that machine's `ascii_mode`.
  Guaranteed by `HandleConnection` filling `keys` only from `ProcessMessage`.
- With `$LC_RIME_IME_HOST` unset the client must behave exactly as before,
  greeting or not: that is the local case and every pre-existing setup.
- No match means **no** input method. Refusing is the point; guessing is what
  drove the wrong laptop.

## Deployment / config
Users add `copilot` to `engine/processors` (before `key_binder`) and `copilot_translator`
to `engine/translators`, plus a `copilot` switch. Full schema-config reference (db path,
`max_candidates`, `max_iterations`, LLM `model`/`n_predict`, `ime_bridge`, `auto_spacer`)
lives in `README.md`.

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
