# rime-ime.nvim

Neovim plugin for Rime IME Bridge. Automatically switches Rime's `ascii_mode` based on Vim mode.

## Features

- **Normal/Visual/Command mode** → English mode (`ascii_mode = true`)
- **Insert mode** → Restore previous mode
- **Auto instance ID** → Unique per host and process, so local and remote instances never collide
- **VS Code detection** → Automatically uses `vscode-neovim` as app name
- **Multi-platform** → Supports macOS, Linux, and Windows
- **Surrounding context push** → Sends `before/after` around cursor for auto spacing
- **Active client routing** → Uses `activate/deactivate` so multiple Neovim instances do not mix context
- **Remote over ssh** → Drive the local IME from Neovim on another machine

## Installation

### lazy.nvim

```lua
{
  dir = "path/to/librime-copilot/clients/neovim",
  config = function()
    require("rime_ime").setup()
  end,
}
```

## Configuration

```lua
require("rime_ime").setup({
  socket_path = nil,          -- explicit endpoint; nil means discover it
  app_name = nil,             -- auto-detect: nvim / vscode-neovim
  instance = nil,             -- auto-generate: hostname-pid
  debug = false,
  reconnect_delay = 1000,     -- ms, first retry
  reconnect_max_delay = 30000, -- ms, ceiling
  max_pending = 10,
  context_chars = 8,          -- characters before the cursor sent as context
})
```

`context_chars` feeds two things on the Rime side: the last character is the
boundary the auto-spacer needs, and the whole run is the n-gram prediction
context (keep it in step with `copilot/surrounding_context_chars`, default 8).

### Endpoint discovery

The plugin tries these in order and uses the first that connects:

1. `$RIME_IME_SOCKET`
2. `socket_path` from `setup()`
3. `/tmp/rime_copilot_ime.sock` (the local default)
4. `/tmp/rime-ime-*.sock` — sockets forwarded here by ssh, newest first,
   restricted to ones owned by you

Both `/path/to.sock` and `host:port` are accepted wherever an endpoint is given.
A socket left behind by a crashed session refuses the connection immediately and
we fall through to the next one, so no cleanup is needed.

## Remote Neovim over ssh

Run Neovim on a remote host and have it drive the input method on your laptop.
Nothing is needed on the remote side beyond the plugin — no sshd change, no
`AcceptEnv`, no environment variable.

This works because **every tunnel terminates at the same bridge socket on your
laptop**, so the remote plugin does not have to know which tunnel is "its own":
any live one is correct.

### Recommended: the `rime-ssh` wrapper

```sh
ln -s "$PWD/clients/neovim/rime-ssh" ~/bin/rime-ssh
rime-ssh dev            # instead of: ssh dev
```

Each invocation forwards a freshly named socket, so several sessions to the same
host each own theirs and logging out of one cannot strand the others. It does
not touch `~/.ssh/config`, so `scp`, `git` and `rsync` are unaffected. With Rime
not running it execs plain `ssh`.

### Alternative: plain `~/.ssh/config`

```
Host dev
  RemoteForward /tmp/rime-ime-%C.sock /tmp/rime_copilot_ime.sock
```

`%C` hashes (local host, remote host, port, remote user). A second concurrent
session fails to bind that path — ssh only warns — and simply reuses the first
session's tunnel, which is harmless. The catch is that closing the *first*
session takes the socket with it and the later ones go dormant.

> **Do not use `${VAR}` in the path.** The manual says an undefined variable
> makes ssh ignore the keyword; in practice it is fatal:
> ```
> config line 3: Bad forwarding specification.
> config: terminating, 1 bad configuration options
> ```
> `~/.ssh/config` applies to `scp`, `git` and `rsync` too, so that form takes
> them all down with it. Gating it behind `Match exec` does not help. Use a
> token that is always defined, or the wrapper.

### Hosts where you cannot change sshd

Forward a loopback port instead:

```
Host locked
  RemoteForward 127.0.0.1:9527 /tmp/rime_copilot_ime.sock
```

and on the remote host `export RIME_IME_SOCKET=127.0.0.1:9527`.

Note that any user on that host can reach a loopback port. The protocol is
one-way — the server never replies, so nothing can be read out of it — but they
could flip your IME to English. Prefer the unix-socket forms; the plugin
`chmod`s a forwarded socket to 0600 as soon as it picks one.

### Focus events

Ownership of the surrounding context follows whoever last pushed it. For the
*first* keystroke after switching windows to be right as well, the terminal has
to report focus. Under tmux:

```
set -g focus-events on
```

iTerm2, Kitty, WezTerm and Ghostty all support focus reporting, and it works
over ssh (it is just an escape sequence).

## Known limitations

1. **The first character after switching windows.** With two Neovim instances
   both sitting in insert mode and a terminal that does not report focus, the
   first character you type after switching can use the previous window's
   context. Keystrokes reach Rime and are composed there *before* anything
   arrives in Neovim, so no Neovim event can fire early enough to prevent it.
   `copilot/ime_bridge/context_ttl_seconds` (default 60) bounds the damage: past
   that the context is treated as absent rather than wrong.
2. **`ascii_mode` is a single global.** Each instance keeps its own set/restore
   stack, but the base each stack records is "whatever the global was at the
   time", possibly just set by another instance. So instance A can record
   `base=false`, B then record `base=true`, and B's restore switch you to
   Chinese while A is still in normal mode. This is inherent to per-client
   stacks over one shared value. In practice the window you touched last is
   always right, because every `InsertEnter` restores, every `InsertLeave` sets
   English, and `FocusGained` outside insert mode forces English without
   touching the stack.
3. **Actions land on the next keypress.** The bridge applies queued actions from
   Rime's key handler, so an `ascii_mode` change pushed by Neovim takes effect
   when you next press a key in Rime.
4. **A brief permission window.** Between sshd creating a forwarded socket and
   the plugin `chmod`-ing it to 0600, it carries sshd's login umask (often
   0755). On a shared host, pre-create a 0700 directory in `~/.profile` and
   forward into that instead.

## API

```lua
local ime = require("rime_ime")

ime.is_enabled() -- Check if plugin is active
ime.set(true)    -- Set ascii_mode
ime.restore()    -- Restore previous mode
ime.reset(true)  -- Reset state
ime.unregister() -- Unregister client
ime.ping()       -- Health check
ime.get_config() -- Get current config
ime.activate()   -- Mark this nvim as active context owner
ime.deactivate() -- Clear active ownership
ime.context()    -- Push surrounding text (insert mode)
ime.clear_context() -- Clear surrounding context
```

## Requirements

- Neovim 0.7+
- A reachable Rime IME Bridge endpoint (local socket, or forwarded over ssh)
- Rime with copilot plugin (IME Bridge enabled)

## Functionality Details

This plugin manages Rime's `ascii_mode` intelligently to provide a seamless Vim experience:

1. **Startup**: Connects socket and records initial IME state through `set(true)` / restore stack semantics.
2. **InsertEnter**:
   - `activate` this client,
   - `restore` previous mode,
   - push `context` immediately and once more after a short delay.
3. **Insert mode typing**:
   - push `context` on `CursorMovedI` and `TextChangedI` (deduped: only when the
     surrounding text actually changed).
4. **InsertLeave/CmdlineEnter/FocusLost**:
   - set English mode as needed,
   - `deactivate`,
   - `clear_context`.
5. **FocusGained**:
   - if back in insert mode: `activate` + `context`,
   - otherwise force English mode (`stack=false`) and clear context ownership.
6. **Exit**: `reset(true)`, `deactivate`, `clear_context`, `unregister`.
