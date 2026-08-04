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

The client is one self-contained module directory:

```
lua/rime_ime/
├── init.lua       -- connection lifecycle, autocmds, protocol
└── endpoint.lua   -- pure logic: endpoint parsing, discovery, backoff, identity
```

Nothing else is needed at runtime — `test/` is not required.

### With a plugin manager

```lua
-- lazy.nvim
{
  dir = "path/to/librime-copilot/clients/neovim",
  config = function()
    require("rime_ime").setup()
  end,
}
```

### Without one, including on a remote host

Drop the directory into any `lua/` on your `runtimepath` and call `setup()`.
Because it is one directory, copying it is a single command — which is how you
keep a remote host byte-identical to your laptop:

```sh
scp -r ~/.config/nvim/lua/rime_ime dev:~/.config/nvim/lua/
```

Then in the remote `init.lua`:

```lua
require("rime_ime").setup()
```

`~/.config/nvim/lua` is already on the runtimepath, so no plugin manager is
involved. See "Remote Neovim over ssh" below for the tunnel that lets it reach
your laptop's IME.

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
4. `/tmp/rime-ime-*.sock` — sockets forwarded here by ssh, newest first
5. `127.0.0.1:19527` — the forwarded loopback port (the remote case)

The list is the same on every machine, which is the point: on the machine Rime
runs on, 3 connects and the rest is never reached; on a remote host, 3 and 4
fail and 5 is the tunnel. Nothing to set per host.

**Every candidate is checked before it is dialled.** A unix path that exists
must be a socket (by `lstat`, so a symlink is refused) and must be owned by you;
one that does not exist yet is kept and simply fails to connect, because the list
is rebuilt on every attempt. A loopback port is checked the same way, through
`/proc/net/tcp`: if someone else holds the listener, the candidate is dropped.
This matters on a shared host — `/tmp` is world-writable and a port is owned by
whoever binds it first, so without these checks any other user could squat the
well-known endpoint and receive what you type. Set `debug = true` to see
rejections.

> The port check needs `/proc`, so it does not run on macOS. A forwarded port on
> a **macOS remote** is trusted. On a single-user Mac that is fine; on a shared
> one, pin `$RIME_IME_SOCKET` to a socket path instead.

Both `/path/to.sock` and `host:port` are accepted wherever an endpoint is given.
A port on another host is never checked against the local table, so an explicitly
configured `1.2.3.4:9527` is passed through untouched.

## Remote Neovim over ssh

Run Neovim on a remote host and have it drive the input method on your laptop.
Nothing is needed on the remote side beyond the plugin — no sshd change, no
`AcceptEnv`, no environment variable.

This works because **every tunnel terminates at the same bridge socket on your
laptop**, so the remote plugin does not have to know which tunnel is "its own":
any live one is correct.

### Setup

All of it lives in `~/.ssh/config` on your laptop. You keep typing `ssh dev`.

```
Host dev
  HostName ...
  ControlMaster auto
  ControlPath ~/.ssh/cm-%u-%r-%h-%p
  ControlPersist 10m
  RemoteForward 127.0.0.1:19527 /tmp/rime_copilot_ime.sock
  ServerAliveInterval 15
  ServerAliveCountMax 3
```

**Forward a port, not a socket file.** This is the one decision that matters, and
it is not a style preference — see [below](#the-socket-file-outlives-the-forward)
for what the socket-file form does after its first session. A port leaves no
residue: when the listener goes, the port is free, and the next `ssh` binds it
again. Nothing to configure on the remote, and nothing to re-apply when you
reinstall a machine or add a host.

What a port gives up is ownership — a socket file carries a uid, a port belongs
to whoever binds it — and the plugin buys that back by refusing a listener that
is not yours (see [Endpoint discovery](#endpoint-discovery), including the macOS
gap).

**`ControlMaster` is load-bearing, not a nicety.** ssh_config offers no token
that varies per session — `%C`, `%l`, `%n`, `%p`, `%r`, `%u` are all functions of
the host tuple — so every session to a host wants to bind the *same* path.
Multiplexing removes the contention instead of working around it: all your
`ssh dev` invocations share one transport, the forward belongs to the master, and
`ControlPersist` keeps it alive past the last session's exit.

Without it, things still work but degrade: the second concurrent session fails to
bind, ssh warns, and its Neovim borrows the first session's socket — correct,
since all tunnels lead to the same place. The catch is that closing the *first*
session takes the socket with it and the later ones go dormant until any new
`ssh dev` recreates it (they then recover on the next `InsertEnter`).

`ServerAlive*` is not about this plugin, but multiplexing makes it matter more.
When a master's TCP dies silently — network change, VPN drop, laptop resume —
every new `ssh dev` blocks on the mux handshake with **no timeout of its own**;
`ConnectTimeout` does not apply to it. The only thing that ends the wait is the
master giving up and exiting, so the keepalive interval *is* the hang duration:
measured at 14s with `5 x 2`, and the stock `60 x 3` would be about three
minutes. Multiplexing concentrates this — one wedged master hangs every session
at once, which is why it looks worse when several are open.

> **Do not use `${VAR}` in the path.** The manual says an undefined variable
> makes ssh ignore the keyword; in practice it is fatal:
> ```
> config line 3: Bad forwarding specification.
> config: terminating, 1 bad configuration options
> ```
> `~/.ssh/config` applies to `scp`, `git` and `rsync` too, so that form takes
> them all down with it. Gating it behind `Match exec` does **not** help — the
> parse still fails. (`ssh -G` will not show you this: it does not evaluate
> `Match exec` at all.) Use a token that is always defined.

### The socket file outlives the forward — read this one

`Warning: remote port forwarding failed for listen path ...` is not an edge case
you might hit. It is the **default second outcome on every host**, and it is the
single most likely reason a remote setup that worked once never works again.

sshd does not remove the forwarded socket file when the forward goes away. Not on
a clean `ssh -O exit`, not on `SIGKILL` — both were measured leaving the file
behind. And sshd refuses to bind over an existing file. So the first session to
exit poisons the name for every session after it: the tunnel works exactly once
per host, and from then on every `ssh` prints that warning and the remote Neovim
finds a socket file that refuses connection.

**This is why the setup above forwards a port.** A port has no such residue, so
the failure cannot occur; measured across repeated `SIGKILL`s of the master, every
reconnect bound cleanly.

If you want the socket-file form anyway — the one case where it is genuinely
better is a **shared macOS remote**, where the port cannot be ownership-checked —
then set this on that host and do not forget it after a reinstall:

```
# remote /etc/ssh/sshd_config (or a drop-in under sshd_config.d/)
StreamLocalBindUnlink yes
```

sshd then unlinks before binding, so the name is always live. Requires root and a
`systemctl reload ssh`.

Without it the plugin still recovers, but only partly: when a candidate under
`/tmp/rime-ime-*.sock` refuses connection it unlinks the file, so the *next* ssh
session binds cleanly. `ECONNREFUSED` is what proves no one is listening — a live
forward accepts immediately — and only sockets matching the tunnel pattern and
owned by you are ever touched, so this cannot reach Rime's own socket or another
user's. The catch is that a multiplexed session will not re-establish a forward
its master failed to bind, so **the session you are in stays without an IME** and
recovery lands on the next master. In practice that means the first ssh after
each idle gap is dead. That is the cost the port form avoids.

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
4. **A brief permission window**, if you forward a socket file rather than the
   port. Between sshd creating it and the plugin `chmod`-ing it to 0600, it
   carries sshd's login umask (often 0755). On a shared host, pre-create a 0700
   directory in `~/.profile` and forward into that instead.
5. **Two laptops, one remote account.** "Any live tunnel is correct" holds
   because every tunnel ends at the same bridge — which stops being true if you
   ssh to the same remote host, as the same user, from two different machines.
   Both are then yours by uid and both look valid, but they lead to different
   IMEs. With the forwarded port they contend for one number, so the first
   laptop to bind keeps it and the second gets `Warning: remote port forwarding
   failed` and no tunnel — annoying, but at least never *silently* driving the
   wrong Mac. Give each machine its own port if you work that way.

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
   - push `context` on every `CursorMovedI` and `TextChangedI`, without
     deduplication. Each push also re-claims ownership of the surrounding
     context on the server, and that claim is the only thing that reliably
     follows the keyboard when the terminal does not report focus events
     (common under tmux and ssh) — suppressing an "unchanged" payload
     suppressed the re-claim with it, letting a second Neovim keep owning the
     context while you typed into the first.
4. **InsertLeave/CmdlineEnter/FocusLost**:
   - set English mode as needed,
   - `deactivate`,
   - `clear_context`.
5. **FocusGained**:
   - if back in insert mode: `activate` + `context`,
   - otherwise force English mode (`stack=false`) and clear context ownership.
6. **Exit**: `reset(true)`, `deactivate`, `clear_context`, `unregister`.
