-- rime-ime.nvim
-- Neovim plugin for Rime IME Bridge
-- Control Rime's ascii_mode based on Vim mode

local M = {}

local uv = vim.uv or vim.loop
local socket = nil
local connected = false
local connecting = false
local pending_messages = {}
local enabled = false
local pending_insert_leave_timer = nil

local config = {
  socket_path = "/tmp/rime_copilot_ime.sock",
  app_name = nil,         -- auto-detect if nil
  instance = nil,         -- auto-generate if nil
  debug = false,
  reconnect_delay = 1000, -- ms
  max_pending = 10,       -- max queued messages
  rime_user_dir = nil,    -- auto-detect if nil
}

-- Detect platform and return rime user directory
local function detect_rime_user_dir()
  if vim.fn.has("mac") == 1 or vim.fn.has("macunix") == 1 then
    return vim.fn.expand("~/Library/Rime")
  elseif vim.fn.has("win32") == 1 or vim.fn.has("win64") == 1 then
    -- Windows: %APPDATA%\Rime
    local appdata = vim.env.APPDATA or ""
    return appdata .. "\\Rime"
  else
    -- Linux/BSD: ~/.config/ibus/rime or ~/.local/share/fcitx5/rime
    local xdg_config = vim.env.XDG_CONFIG_HOME or vim.fn.expand("~/.config")
    local ibus_path = xdg_config .. "/ibus/rime"
    local fcitx5_path = vim.fn.expand("~/.local/share/fcitx5/rime")

    -- Check ibus first, then fcitx5
    local stat = uv.fs_stat(ibus_path)
    if stat and stat.type == "directory" then
      return ibus_path
    end
    stat = uv.fs_stat(fcitx5_path)
    if stat and stat.type == "directory" then
      return fcitx5_path
    end
    -- Default to ibus path for check
    return ibus_path
  end
end

-- Check if Rime directory exists
local function rime_exists()
  local dir = config.rime_user_dir
  if not dir then return false end
  local stat = uv.fs_stat(dir)
  return stat ~= nil and stat.type == "directory"
end

-- Auto-detect app name
local function detect_app_name()
  if vim.g.vscode then
    return "vscode-neovim"
  elseif vim.env.NVIM_APPNAME then
    return vim.env.NVIM_APPNAME
  else
    return "nvim"
  end
end

-- Generate unique instance ID
local function generate_instance_id()
  local parts = {}

  -- PID is always unique per process
  table.insert(parts, tostring(vim.fn.getpid()))

  -- Add terminal/GUI info if available
  if vim.env.TERM_SESSION_ID then
    table.insert(parts, vim.env.TERM_SESSION_ID:sub(1, 8))
  elseif vim.env.WINDOWID then
    table.insert(parts, vim.env.WINDOWID)
  end

  return table.concat(parts, "-")
end

-- Log helper
local function log(msg)
  if config.debug then
    vim.notify("[rime-ime] " .. msg, vim.log.levels.DEBUG)
  end
end

-- Get surrounding text (up to 2 chars before and 1 char after cursor)
local function get_surrounding()
  local ok, before, after = pcall(function()
    local _, col = unpack(vim.api.nvim_win_get_cursor(0))
    local line = vim.api.nvim_get_current_line()

    if line == "" then
      return "", ""
    end

    -- Convert cursor byte offset to UTF-8 character index, then slice by characters.
    -- This is robust for CJK content and avoids byte-boundary drift.
    local char_index = vim.str_utfindex(line, col)
    local char_count = vim.fn.strchars(line)

    local before_len = math.min(2, math.max(0, char_index))
    local before_start = math.max(0, char_index - before_len)
    local b = ""
    if before_len > 0 then
      b = vim.fn.strcharpart(line, before_start, before_len)
    end

    local a = ""
    if char_index < char_count then
      a = vim.fn.strcharpart(line, char_index, 1)
    end

    return b, a
  end)

  if ok then
    return before or "", after or ""
  else
    return "", ""
  end
end

-- Build JSON message
local function build_message(action, data)
  local msg = {
    v = 1,
    ns = "rime.ime",
    type = "ascii",
    src = {
      app = config.app_name,
      instance = config.instance,
    },
    data = vim.tbl_extend("force", { action = action }, data or {}),
  }
  return vim.json.encode(msg) .. "\n"
end

-- Flush pending messages
local function flush_pending()
  if not connected or #pending_messages == 0 then
    return
  end

  local messages = pending_messages
  pending_messages = {}

  for _, msg in ipairs(messages) do
    local ok = pcall(function()
      socket:write(msg)
    end)
    if not ok then
      log("Flush failed, message dropped")
    end
  end
end

-- Handle connection close
local function on_close()
  connected = false
  connecting = false
  if socket then
    pcall(function() socket:close() end)
    socket = nil
  end
  log("Connection closed")
end

-- Connect to socket (async)
local function connect()
  if connected or connecting then
    return
  end

  connecting = true
  socket = uv.new_pipe(false)

  socket:connect(config.socket_path, function(err)
    connecting = false

    if err then
      log("Connect failed: " .. tostring(err))
      pcall(function() socket:close() end)
      socket = nil

      -- Schedule reconnect
      vim.defer_fn(connect, config.reconnect_delay)
      return
    end

    connected = true
    log("Connected to " .. config.socket_path)

    -- Flush pending messages
    vim.schedule(flush_pending)
  end)
end

-- Send message (non-blocking)
local function send(action, data)
  local msg = build_message(action, data)
  log("Queueing: " .. action)

  if connected and socket then
    local ok = pcall(function()
      socket:write(msg)
    end)
    if not ok then
      on_close()
      connect()
    end
  else
    -- Queue message
    if #pending_messages < config.max_pending then
      table.insert(pending_messages, msg)
    end
    connect()
  end
end

-- Disconnect
local function disconnect()
  on_close()
  pending_messages = {}
  log("Disconnected")
end

-- Public API

--- Check if plugin is enabled
function M.is_enabled()
  return enabled
end

--- Set ascii_mode
---@param ascii boolean
---@param opts table|nil
function M.set(ascii, opts)
  if not enabled then return end

  local data = { ascii = ascii }
  if opts and opts.stack ~= nil then
    data.stack = opts.stack
  end

  send("set", data)
end

--- Restore previous ascii_mode
function M.restore()
  if not enabled then return end
  send("restore")
end

--- Reset state
---@param restore_mode boolean|nil
function M.reset(restore_mode)
  if not enabled then return end
  send("reset", { restore = restore_mode ~= false })
end

--- Unregister client (clean exit without restoring)
function M.unregister()
  if not enabled then return end
  send("unregister")
end

--- Ping server
function M.ping()
  if not enabled then return end
  send("ping")
end

--- Mark current client as active context owner
function M.activate()
  if not enabled then return end
  send("activate")
end

--- Mark current client as inactive context owner
function M.deactivate()
  if not enabled then return end
  send("deactivate")
end

--- Push surrounding text context
function M.context()
  if not enabled then return end

  -- Only push context in insert/replace mode
  local mode = vim.fn.mode()
  if mode ~= 'i' and mode ~= 'R' then
    return
  end

  local before, after = get_surrounding()
  send("context", { before = before, after = after })
end

--- Clear surrounding text context
function M.clear_context()
  if not enabled then return end
  send("clear_context")
end

--- Get current config
function M.get_config()
  return vim.deepcopy(config)
end

--- Setup plugin with options
---@param opts table|nil
function M.setup(opts)
  config = vim.tbl_deep_extend("force", config, opts or {})

  -- Auto-detect rime_user_dir if not provided
  config.rime_user_dir = config.rime_user_dir or detect_rime_user_dir()

  -- Check if Rime directory exists
  if not rime_exists() then
    if config.debug then
      vim.notify("[rime-ime] Disabled: Rime directory not found at " .. (config.rime_user_dir or "nil"),
        vim.log.levels.INFO)
    end
    return
  end

  enabled = true

  -- Auto-detect app_name and instance if not provided
  config.app_name = config.app_name or detect_app_name()
  config.instance = config.instance or generate_instance_id()

  log("app_name=" .. config.app_name .. ", instance=" .. config.instance)

  local group = vim.api.nvim_create_augroup("RimeIme", { clear = true })

  -- Insert mode: restore previous mode and push context
  vim.api.nvim_create_autocmd("InsertEnter", {
    group = group,
    callback = function()
      if pending_insert_leave_timer then
        pcall(function() pending_insert_leave_timer:stop() end)
        pcall(function() pending_insert_leave_timer:close() end)
        pending_insert_leave_timer = nil
      end
      M.activate()
      M.restore()
      M.context()
      vim.defer_fn(function()
        M.context()
      end, 10)
    end,
  })

  -- Push context on cursor movement in insert mode
  vim.api.nvim_create_autocmd("CursorMovedI", {
    group = group,
    callback = function()
      M.context()
    end,
  })

  -- Push context on text change in insert mode
  vim.api.nvim_create_autocmd("TextChangedI", {
    group = group,
    callback = function()
      M.context()
    end,
  })

  -- Push context before each inserted character.
  -- This improves first-key boundary accuracy for auto spacing.
  vim.api.nvim_create_autocmd("InsertCharPre", {
    group = group,
    callback = function()
      M.context()
    end,
  })

  -- Leave Insert mode: set ascii and clear context
  vim.api.nvim_create_autocmd("InsertLeave", {
    group = group,
    callback = function()
      if pending_insert_leave_timer then
        pcall(function() pending_insert_leave_timer:stop() end)
        pcall(function() pending_insert_leave_timer:close() end)
      end
      pending_insert_leave_timer = uv.new_timer()
      pending_insert_leave_timer:start(60, 0, vim.schedule_wrap(function()
        if not enabled then
          return
        end
        local mode = vim.fn.mode()
        if mode ~= "i" and mode ~= "R" then
          M.set(true)
          M.deactivate()
          M.clear_context()
        end
        if pending_insert_leave_timer then
          pcall(function() pending_insert_leave_timer:close() end)
          pending_insert_leave_timer = nil
        end
      end))
    end,
  })

  -- Command mode: set ascii and clear context
  vim.api.nvim_create_autocmd("CmdlineEnter", {
    group = group,
    callback = function()
      M.set(true)
      M.deactivate()
      M.clear_context()
    end,
  })

  vim.api.nvim_create_autocmd("CmdlineLeave", {
    group = group,
    callback = function()
      -- 总是调用 restore 来平衡 CmdlineEnter 的 set
      M.restore()
    end,
  })

  -- FocusGained: ensure ascii if in normal mode (without stacking)
  vim.api.nvim_create_autocmd({ "FocusGained" }, {
    group = group,
    callback = function()
      -- 延迟执行以确保在 OS/IDE 焦点切换和状态恢复完成后强制覆盖
      local mode = vim.fn.mode()
      if mode == "i" or mode == "R" then
        M.activate()
        M.context()
      elseif mode ~= "c" then
        -- 使用 stack=false 避免影响 restore 栈
        M.set(true, { stack = false })
        M.deactivate()
        M.clear_context()
      end
    end,
  })

  vim.api.nvim_create_autocmd({ "FocusLost" }, {
    group = group,
    callback = function()
      M.deactivate()
      M.clear_context()
    end,
  })

  -- Visual mode: set ascii
  vim.api.nvim_create_autocmd("ModeChanged", {
    group = group,
    pattern = "*:[vV\x16]*",
    callback = function()
      M.set(true)
    end,
  })

  -- Leave Visual mode: restore (to balance the set)
  vim.api.nvim_create_autocmd("ModeChanged", {
    group = group,
    pattern = "[vV\x16]*:*",
    callback = function()
      M.restore()
    end,
  })

  -- Leave Vim: unregister client
  vim.api.nvim_create_autocmd("VimLeavePre", {
    group = group,
    callback = function()
      M.reset(true)  -- 恢复到 Neovim 启动时的状态
      M.deactivate()
      M.clear_context()
      M.unregister() -- 清除注册
      disconnect()
    end,
  })

  -- Initial connect (async)
  connect()

  -- 在连接建立后立即保存初始状态并设置为英文
  -- 这样 reset(true) 可以恢复到 Neovim 启动时的状态
  vim.defer_fn(function()
    M.set(true)
    log("Initial state saved, ascii_mode set to true")
  end, 200)

  log("Plugin initialized")
end

return M
