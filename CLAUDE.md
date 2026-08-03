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

- **llama.cpp** (tag `b7820`) and **nlohmann_json** are pulled via CMake `FetchContent` — the first configure downloads and builds llama.cpp, which is slow.
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
  CI runs lint + build-with-ASAN + this suite.

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

## Tools (`tools/`, built when `BUILD_TOOLS=ON`, default)
- `build_copilot` — builds the `copilot.db` prediction DB. Reads stdin lines of
  `key text weight`, writes the DB file (arg 1, default `copilot.db`).
- `dump_copilot` — read-only probe: prints a key's continuations with weights,
  ranks and duplicate counts. First stop when candidates come out in a
  surprising order (`dump_copilot <db> --find 议 -- 建`).
- `make_copilot_db.py` — the pipeline that feeds `build_copilot`: reads Rime
  `*.dict.yaml` files listed in a JSON config (see `dict.example.json`), merges
  and weights them, splits every word into `prefix → suffix` pairs.
  **The weight convention lives here and in `src/db_provider.h` / `src/rerank.h`
  and must match: larger = more likely.** Writing a rank instead of a real
  weight silently inverts every ordering in the plugin.
- `ax_poc.mm` — macOS Accessibility API proof-of-concept (Apple-only).

## Clients
- `clients/neovim/lua/rime_ime/` — Neovim client for the IME Bridge, one
  self-contained module directory so it can be synced to a remote host as a
  unit. `init.lua` is the stateful part (connection lifecycle, autocmds,
  protocol); `endpoint.lua` is pure logic (endpoint parsing, discovery order,
  backoff, instance identity) with no `vim.*` reference, which is what lets
  `clients/neovim/test/endpoint_spec.lua` run under bare `nvim -l`.

## Deployment / config
Users add `copilot` to `engine/processors` (before `key_binder`) and `copilot_translator`
to `engine/translators`, plus a `copilot` switch. Full schema-config reference (db path,
`max_candidates`, `max_iterations`, LLM `model`/`n_predict`, `ime_bridge`, `auto_spacer`)
lives in `README.md`.
