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
```

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
```

### Actions

| Action | Description |
|--------|-------------|
| `set` | Set `ascii_mode` to specified value, saves previous state |
| `restore` | Restore to previous state (supports nested calls) |
| `reset` | Clear state and optionally restore original mode |
| `unregister` | Remove client registration (on exit) |
| `ping` | Health check |

* Deploy and enjoy.


