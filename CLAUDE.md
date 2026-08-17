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
| `cli.py` | orchestration |

Subcommands: `status`, `restore`, `backup`, `fetch`, `build`, `deploy`,
`update`, `install`. A new machine builds `build_copilot` from a librime
checkout, runs `install` once, then `restore` and `update` from the installed
`private/bin/rime-copilot` — no checkout needed after that.

**Drift-detection contract:** `install` records a manifest
(`private/bin/.installed.json`: source commit, source path, content hash per
installed file) so `status` can report, every run, whether the installed copy
has been edited in place, is missing a file, or has fallen behind the repo it
came from — without that, an installed copy is just a second, unversioned
`private/bin/` waiting to rot the same way the original one did.

**The weight convention lives in `dictdb.py` and in `src/db_provider.h` /
`src/rerank.h`, and must match: larger = more likely.** Writing a rank instead
of a real weight silently inverts every ordering in the plugin. `Entry.weight`
(`dictfile.py`) is a `float`, not an `int` — it becomes fractional once a
`scale` is applied.

Third-party imports (`pypinyin`, `requests`, `bs4`) are lazy, at the
point of use, so every module imports on a stock interpreter. `pypinyin` is
required at runtime for any dictionary with no pinyin column (e.g.
`tencent.dict.yaml`, which is `word⇥weight` only); `requests`/`bs4` for
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
command above needs no activation. On a new machine:

```sh
pyenv virtualenv system rime-copilot
~/.pyenv/versions/rime-copilot/bin/pip install pypinyin requests beautifulsoup4
```

`RECIPE_VERSION` in `freshness.py` must be bumped by hand when a change to the
build algorithm alters the output for unchanged inputs. No input hash catches
that, and a stale database will otherwise be reported as fresh.

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
