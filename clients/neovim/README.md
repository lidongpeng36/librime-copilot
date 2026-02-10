# rime-ime.nvim

Neovim plugin for Rime IME Bridge. Automatically switches Rime's `ascii_mode` based on Vim mode.

## Features

- **Normal/Visual/Command mode** → English mode (`ascii_mode = true`)
- **Insert mode** → Restore previous mode
- **Auto instance ID** → Unique per Neovim process (PID-based), supports multi-instance routing
- **VS Code detection** → Automatically uses `vscode-neovim` as app name
- **Multi-platform** → Supports macOS, Linux, and Windows
- **Surrounding context push** → Sends `before/after` around cursor for auto spacing
- **Active client routing** → Uses `activate/deactivate` so multiple Neovim instances do not mix context

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
  socket_path = "/tmp/rime_copilot_ime.sock",
  app_name = nil,       -- auto-detect: nvim / vscode-neovim
  instance = nil,       -- auto-generate: PID-based
  debug = false,
  reconnect_delay = 1000,
  max_pending = 10,
  rime_user_dir = nil,  -- auto-detect based on platform
})
```

**Auto-detected Rime directories:**
- **macOS**: `~/Library/Rime`
- **Linux**: `~/.config/ibus/rime` or `~/.local/share/fcitx5/rime`
- **Windows**: `%APPDATA%\Rime`

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
- Rime installed (auto-detected based on platform)
- Rime with copilot plugin (IME Bridge enabled)

## Functionality Details

This plugin manages Rime's `ascii_mode` intelligently to provide a seamless Vim experience:

1. **Startup**: Connects socket and records initial IME state through `set(true)` / restore stack semantics.
2. **InsertEnter**:
   - `activate` this client,
   - `restore` previous mode,
   - push `context` immediately and once more after a short delay.
3. **Insert mode typing**:
   - push `context` on `CursorMovedI`, `TextChangedI`, and `InsertCharPre`.
4. **InsertLeave/CmdlineEnter/FocusLost**:
   - set English mode as needed,
   - `deactivate`,
   - `clear_context`.
5. **FocusGained**:
   - if back in insert mode: `activate` + `context`,
   - otherwise force English mode (`stack=false`) and clear context ownership.
6. **Exit**: `reset(true)`, `deactivate`, `clear_context`, `unregister`.
