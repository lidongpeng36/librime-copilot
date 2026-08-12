# librime-copilot

> from [librime-predict](https://github.com/rime/librime-predict.git)

librime plugin. Copilot next word prediction with LLM support.

## Features

- **Next Word Prediction** - DB-based n-gram prediction and LLM-based prediction
- **Auto Spacer** - Automatically add spaces between Chinese and English/numbers
- **IME Bridge** - Control `ascii_mode` via IPC from external editors (Neovim, Obsidian, VS Code)

## Building the db

`tools/make_copilot_db.py` turns Rime dictionaries into `copilot.db`. Point it at
a JSON config listing the dictionaries to merge (start from
`tools/dict.example.json`):

```sh
tools/make_copilot_db.py -c dict.json -o copilot.db
```

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

## Usage

* Put the db file (by default `copilot.db`) in rime user directory.
* In `*.schema.yaml`, add `copilot` to the list of `engine/processors` before `key_binder`,
add `copilot_translator` to the list of `engine/translators`;
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

  # Contextual candidate re-ranking, as our own filter. Also add
  # `copilot_rerank_filter` to engine/filters. See "Contextual Re-ranking".
  rerank:
    enable: true
    max_context_chars: 8
    window: 32
    max_rank: 50

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

  # tmux pane scrape (for terminal emulators IMK can never answer for; see
  # "tmux Source" below). default: disabled
  tmux_source:
    enabled: true                    # default false; opt-in
    binary: /opt/homebrew/bin/tmux   # optional; probed if empty
    socket: ""                       # optional; tmux default socket if empty
    app_bundle_ids: []               # optional; empty = the built-in terminal list
    timeout_ms: 50
```

### Prediction Context

The db n-gram lookup keys are the last few characters before the caret:

- **Surrounding path** (`use_surrounding_context: true`, default): the text is
  read from the frontend — IMK on macOS, otherwise an IME Bridge client — and
  the word just committed is appended to it (the snapshot is taken before the
  key is handled). Because it follows the caret, moving to another paragraph or
  app immediately predicts from *there*, instead of from whatever was typed
  earlier in the session.
- **History fallback**: when the frontend cannot answer (Chrome/Electron and
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
| `max_rank` | `50` | a continuation ranked below this among its key's continuations never promotes |

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

It is opt-in (`enabled: false` by default) because it reaches into another
process and scrapes a screen. `app_bundle_ids` gates which frontmost
applications may trigger the scrape at all; the built-in list already covers
ten terminals — Alacritty, kitty, WezTerm, Apple Terminal, iTerm2, Ghostty,
Hyper, Warp, Rio, Tabby. Setting `app_bundle_ids` in config **narrows** that
gate, it never adds to it — you cannot widen coverage past the built-in list
from config, only restrict it to a subset.

This source ranks below ImeBridge in the surrounding-text chain (see
`surrounding_source.h`), so the Neovim client keeps winning while it is in
insert mode, with no nvim-side configuration change needed. The source
refuses to answer — falling back to `commit_history` — whenever the answer
would be a guess: two attached tmux clients tied on activity, the frontmost
app is not one of the known terminals, or tmux itself cannot be reached.

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

### Multi-Client Behavior

- IME Bridge handles multiple clients concurrently.
- Surrounding context is resolved from the explicitly active client (`activate/deactivate`), not by timeout heuristics.

Neovim on a remote host can drive the local IME over an ssh reverse tunnel —
see [clients/neovim/README.md](clients/neovim/README.md#remote-neovim-over-ssh).
The wire protocol is unchanged; the client just dials a forwarded socket.

* Deploy and enjoy.
