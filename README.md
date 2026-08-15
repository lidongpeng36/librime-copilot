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

  # tmux pane scrape (macOS only, for terminal emulators IMK can never answer
  # for; see "tmux Source" below). default: disabled
  tmux_source:
    enabled: true                    # default false; opt-in
    binary: /opt/homebrew/bin/tmux   # optional; probed if empty
    socket: ""                       # optional; a full socket *path* for
                                     # `tmux -S`, not a `-L` name. Empty uses
                                     # tmux's own default socket.
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
```

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

**Privacy.** The `ctx` field is Han characters only, by construction:
`TrailingCjkRun` stops at the first non-Han character, so an ASCII secret on
screen yields an empty context and re-ranking does not even run. But the
selected and candidate texts are the user's own Chinese input, which makes this
file a chronological transcript of it — unlike Rime's `*.userdb`, which is a
frequency table with no timeline. The plugin makes no network connections;
copying the file anywhere is an explicit, separate step
(`tools/sync_telemetry.sh`). Be clear about what that step does, though: it puts
the transcript into the iCloud sync directory, from where it is uploaded and
lands on every machine signed into the account — so run it only when you accept
that, and only for as long as you are analysing. Set `enable: false` to stop
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

The report is organised by the claim each section tests, from the design spec.
A rejection rate that is flat across a section's buckets refutes that section's
claim — which is the point. Python 3 standard library only; no dependencies.

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

`update` needs `pypinyin` if any configured dictionary has no pinyin column
(`tencent.dict.yaml` is the common case — it's `word⇥weight` only, so every
entry needs a generated pinyin). Third-party imports are lazy, so a stock
interpreter is fine until that point. `install` pins the interpreter for
this: it rewrites `private/bin/rime-copilot`'s shebang to the absolute path
of the interpreter that ran `install` (`sys.executable`), instead of copying
the checkout's `#!/usr/bin/env python3` through verbatim. That rewrite is
what makes the installed copy work standalone — a bare `#!/usr/bin/env
python3` would *not* have been enough even with `private/.python-version`
pinning a pyenv environment that has `pypinyin`: pyenv resolves
`.python-version` from the *caller's* current working directory, not the
script's location, so `rime-copilot` run from anywhere other than
`private/` would silently fall back to the global interpreter and lose
`pypinyin`. `install` also checks, at install time, whether the interpreter
it is pinning can import `pypinyin`, and warns (without refusing to install)
if it cannot — `status`, `restore`, and `backup` do not need it; only
`build` and `update` do, and only for dictionaries without a pinyin column.
