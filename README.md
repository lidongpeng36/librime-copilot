# librime-copilot

> from [librime-predict](https://github.com/rime/librime-predict.git)

librime plugin. Copilot next word prediction with LLM support.

## Features

- **Next Word Prediction** - DB-based n-gram prediction and LLM-based prediction
- **Auto Spacer** - Automatically add spaces between Chinese and English/numbers
- **IME Bridge** - Control `ascii_mode` via IPC from external editors (Neovim, Obsidian, VS Code)

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
    debug: false

  # Auto Spacer configuration
  auto_spacer:
    # Whether to add right-side space when committing text in the middle,
    # e.g. "测|试" + "test" -> "测 test 试".
    # default: true
    enable_right_space: true
```

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

### Actions

| Action | Description |
|--------|-------------|
| `set` | Set `ascii_mode`. Params: `ascii` (bool), `stack` (bool, default true). `stack=false` sets mode without affecting restore stack. |
| `restore` | Restore to previous state (supports nested calls) |
| `reset` | Clear state and optionally restore original mode |
| `unregister` | Remove client registration (on exit) |
| `activate` | Mark this client as active context owner |
| `deactivate` | Clear active ownership for this client |
| `context` | Push surrounding text (`before`, `after`) |
| `clear_context` | Clear stored surrounding text for this client |
| `ping` | Health check |

### Multi-Client Behavior

- IME Bridge handles multiple clients concurrently.
- Surrounding context is resolved from the explicitly active client (`activate/deactivate`), not by timeout heuristics.

* Deploy and enjoy.
