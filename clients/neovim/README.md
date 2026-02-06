# rime-ime.nvim

Neovim plugin for Rime IME Bridge. Automatically switches Rime's `ascii_mode` based on Vim mode.

## Features

- **Normal/Visual/Command mode** → English mode (`ascii_mode = true`)
- **Insert mode** → Restore previous mode
- **Auto instance ID** → Unique per Neovim process and Buffer, supports multiple windows/splits seamlessly
- **VS Code detection** → Automatically uses `vscode-neovim` as app name
- **Multi-platform** → Supports macOS, Linux, and Windows

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
```

## Requirements

- Neovim 0.7+
- Rime installed (auto-detected based on platform)
- Rime with copilot plugin (IME Bridge enabled)

## Functionality Details

This plugin manages Rime's `ascii_mode` intelligently to provide a seamless Vim experience:

1.  **Startup**: Saves the initial IME state (e.g., English) immediately upon connection.
2.  **Insert Mode**: Restores the previous IME state (e.g., switches back to Chinese if you were typing Chinese).
3.  **Normal/Visual/Command Mode**: Switches to English mode (`ascii_mode = true`) for command input.
4.  **Focus Gained / Window Switch**: When switching back to Neovim window or between panels (e.g., in VS Code) in Normal mode, it ensures English mode is active. It includes a small delay (50ms) to override any OS/IDE default state restoration.
5.  **Exit**: Restores the IME state to what it was when Neovim started.

