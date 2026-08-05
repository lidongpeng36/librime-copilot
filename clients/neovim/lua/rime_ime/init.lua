-- rime-ime.nvim
-- Neovim plugin for Rime IME Bridge
-- Control Rime's ascii_mode based on Vim mode

local M = {}

local endpoint = require("rime_ime.endpoint")

local uv = vim.uv or vim.loop
local socket = nil
local connected = false
local connecting = false
local pending_messages = {}
local enabled = false
local shutting_down = false
local reconnect_timer = nil
local attempt = 0
local active_endpoint = nil
local pending_insert_leave_timer = nil
-- Connections dialled but not yet adopted or rejected. Tracked so disconnect()
-- can close them: a probe still awaiting a greeting when VimLeavePre runs would
-- otherwise outlive the client it belongs to.
local probe_handles = {}

local config = {
  -- Explicit endpoint. nil means "discover it": $RIME_IME_SOCKET, then this
  -- field, then the local default, then any ssh-forwarded tunnel socket.
  -- Accepts "/path/to.sock" and "host:port".
  socket_path = nil,
  app_name = nil,             -- auto-detect if nil
  instance = nil,             -- auto-generate if nil
  debug = false,
  reconnect_delay = 1000,     -- ms, first retry
  reconnect_max_delay = 30000, -- ms, ceiling
  max_pending = 10,           -- max queued messages
  -- How long to wait for the bridge's greeting when verifying which machine a
  -- tunnel reaches. Only reached by a candidate that accepts and then stays
  -- silent; a real greeting crosses the ssh link in 5-30ms on real hosts.
  probe_timeout = 500,
  -- How many characters before the cursor to send. The plugin needs only the
  -- boundary character for auto-spacing, but uses the rest as the n-gram
  -- prediction context (copilot/surrounding_context_chars, default 8).
  context_chars = 8,
}

-- Forward declarations: these call each other.
local connect, schedule_reconnect, on_close, resync, flush_pending, queue

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
  -- The hostname matters: a remote nvim and a local one easily share a pid, and
  -- the bridge keys per-client ascii_mode state (depth/base) on this string, so
  -- a collision would have the two corrupting each other's restore stack.
  return endpoint.instance_id({
    hostname = vim.fn.hostname(),
    pid = vim.fn.getpid(),
    term_session_id = vim.env.TERM_SESSION_ID,
    windowid = vim.env.WINDOWID,
  })
end

-- Log helper. Several callers are libuv callbacks, and nvim_echo (under
-- vim.notify) raises E5560 in a fast event context, so defer when we are in one.
local function log(msg)
  if not config.debug then
    return
  end
  if vim.in_fast_event() then
    vim.schedule(function()
      vim.notify("[rime-ime] " .. msg, vim.log.levels.DEBUG)
    end)
    return
  end
  vim.notify("[rime-ime] " .. msg, vim.log.levels.DEBUG)
end

-- Get surrounding text (up to config.context_chars before, 1 char after)
local function get_surrounding()
  local ok, before, after = pcall(function()
    local _, col = unpack(vim.api.nvim_win_get_cursor(0))
    local line = vim.api.nvim_get_current_line()

    if line == "" then
      return "", ""
    end

    -- Convert cursor byte offset to UTF-8 character index, then slice by characters.
    -- This is robust for CJK content and avoids byte-boundary drift.
    -- vim.str_utfindex(s, index) is the deprecated 2-arg form (nvim 0.11).
    -- Counting characters in the prefix is stable across versions and degrades
    -- gracefully if col ever lands mid-character, instead of throwing into the
    -- pcall and silently yielding an empty context.
    local char_index = vim.fn.strchars(line:sub(1, col))
    local char_count = vim.fn.strchars(line)

    local want = math.max(1, config.context_chars or 8)
    local before_len = math.min(want, math.max(0, char_index))
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

-- Endpoint discovery ------------------------------------------------------

-- lstat, deliberately: fs_stat follows symlinks, so an attacker who plants
-- /tmp/rime-ime-x.sock as a link to a socket *we* own (tmux server, ssh agent,
-- a language server) would pass both the type and the ownership check. We would
-- then write JSON Lines into that daemon, consider ourselves connected -- the
-- protocol is one-way, so nothing contradicts it -- and never reach the real
-- tunnel; the fs_chmod below would follow the link too and change that daemon's
-- socket mode. With lstat a symlink reports type == "link" and is rejected.
local function stat_endpoint(path)
  local st = uv.fs_lstat(path)
  if not st then
    return nil
  end
  return {
    type = st.type,
    uid = st.uid,
    mtime = (st.mtime and st.mtime.sec) or 0,
  }
end

-- Who owns the LISTEN socket on a loopback port. Returns nil where that cannot
-- be answered -- macOS has no /proc -- in which case the port is not filtered
-- on ownership. When we are verifying greetings (the remote case) that gap is
-- closed anyway: a tunnel someone else opened answers with someone else's
-- hostname and is refused.
local function tcp_owner(port)
  local uids = nil
  for _, path in ipairs({ "/proc/net/tcp", "/proc/net/tcp6" }) do
    local fh = io.open(path, "r")
    if fh then
      local content = fh:read("*a")
      fh:close()
      uids = uids or {}
      for _, u in ipairs(endpoint.listener_uids(content, port)) do
        uids[#uids + 1] = u
      end
    end
  end
  return uids
end

local function read_file(path)
  local fh = io.open(path, "r")
  if not fh then
    return nil
  end
  local content = fh:read("*a")
  fh:close()
  return content
end

-- The machine this ssh session came from, announced by `SetEnv LC_RIME_IME_HOST
-- =%L`. Its presence is what puts us in "remote, must verify" mode; without it
-- (running on the machine Rime itself is on, or an ssh setup that predates
-- this) nothing below changes behaviour at all.
local function expected_host()
  local h = vim.env.LC_RIME_IME_HOST
  if type(h) == "string" and h ~= "" then
    return h
  end
  return nil
end

-- Loopback ports that could be an ssh reverse tunnel. Deliberately narrow: we
-- open a connection to each one to read its greeting, and a port that cannot be
-- a tunnel should never be touched.
local function tunnel_ports()
  local proc = read_file("/proc/net/tcp")
  if proc then
    local lo, hi = endpoint.parse_port_range(read_file("/proc/sys/net/ipv4/ip_local_port_range"))
    local uid = uv.getuid and uv.getuid() or nil
    local ports = endpoint.listen_ports(proc, uid, lo, hi)
    local six = read_file("/proc/net/tcp6")
    if six then
      for _, p in ipairs(endpoint.listen_ports(six, uid, lo, hi)) do
        ports[#ports + 1] = p
      end
    end
    return ports
  end

  -- macOS: no /proc, but lsof names the owning process, so the candidate set is
  -- exact -- ollama, language servers and the like are never dialled.
  local ok, out = pcall(vim.fn.system, {
    "lsof", "-nP", "-iTCP", "-sTCP:LISTEN", "-a", "-u" .. tostring(uv.getuid and uv.getuid() or 0),
  })
  if not ok or vim.v.shell_error ~= 0 then
    return {}
  end
  return endpoint.lsof_ssh_ports(out)
end

local function candidates()
  local list = endpoint.candidates({
    env = vim.env.RIME_IME_SOCKET,
    configured = config.socket_path,
    glob = function(pattern) return vim.fn.glob(pattern, true, true) end,
    stat = stat_endpoint,
    tcp_owner = tcp_owner,
    uid = uv.getuid and uv.getuid() or nil,
    log = log,
  })

  -- Only worth enumerating when there is something to check the answer
  -- against; otherwise we would be dialling ports with no way to tell whose
  -- they are, which is exactly what we are trying to avoid.
  if expected_host() then
    local seen = {}
    for _, ep in ipairs(list) do
      if ep.kind == "tcp" then
        seen[ep.port] = true
      end
    end
    for _, port in ipairs(tunnel_ports()) do
      if not seen[port] then
        seen[port] = true
        list[#list + 1] = { kind = "tcp", host = "127.0.0.1", port = port }
      end
    end
  end

  return list
end

-- True when there is something worth dialling right now. A path that does not
-- exist is nothing to poll for, so we go dormant instead of burning a timer
-- forever; InsertEnter and FocusGained retry lazily.
--
-- A loopback port leaves no trace on disk, so it can never be ruled out this way
-- and we keep retrying at the backoff ceiling while one is a candidate. That is
-- one refused connect every 30s, and it buys picking up a tunnel the moment ssh
-- reconnects rather than at the next InsertEnter. When another user owns the
-- port the candidate is dropped upstream, and dormancy comes back.
local function any_endpoint_present()
  for _, ep in ipairs(candidates()) do
    if ep.kind == "tcp" then
      return true  -- a port cannot be stat'ed; keep trying
    end
    if uv.fs_lstat(ep.path) then
      return true
    end
  end
  return false
end

-- Message plumbing --------------------------------------------------------

queue = function(msg)
  if #pending_messages >= config.max_pending then
    -- Drop the oldest: these are state messages, and the newest is the one that
    -- describes reality.
    table.remove(pending_messages, 1)
  end
  pending_messages[#pending_messages + 1] = msg
end

flush_pending = function()
  if not connected or not socket then
    return
  end
  local messages = pending_messages
  pending_messages = {}
  for _, msg in ipairs(messages) do
    local ok, err = socket:write(msg)
    if not ok then
      log("Flush failed: " .. tostring(err))
      on_close()
      return
    end
  end
end

-- Connection lifecycle ----------------------------------------------------

on_close = function()
  local was_connected = connected
  connected = false
  connecting = false
  if socket then
    pcall(function() socket:read_stop() end)
    pcall(function() socket:close() end)
    socket = nil
  end
  active_endpoint = nil
  if was_connected then
    log("Connection lost")
    attempt = 0  -- a working endpoint just died; retry promptly
  end
  schedule_reconnect()
end

schedule_reconnect = function()
  if shutting_down or reconnect_timer or connected or connecting then
    return
  end
  if not any_endpoint_present() then
    log("No endpoint present; dormant until the next InsertEnter/FocusGained")
    return
  end
  attempt = attempt + 1
  local delay = endpoint.backoff(attempt, config.reconnect_delay, config.reconnect_max_delay)
  log("Reconnecting in " .. delay .. "ms (attempt " .. attempt .. ")")
  reconnect_timer = vim.defer_fn(function()
    reconnect_timer = nil
    connect()
  end, delay)
end

-- After a reconnect the server knows nothing about us, so re-assert whatever
-- the current mode implies. Note we do NOT send restore here: the restore stack
-- died with the old connection and there is nothing meaningful to pop.
resync = function()
  local mode = vim.fn.mode()
  if mode == "i" or mode == "R" then
    M.activate()
    M.context()
  else
    M.set(true, { stack = false })
    M.deactivate()
  end
end

-- sshd does not remove a forwarded socket file when the forward goes away --
-- not on a clean `ssh -O exit`, not on SIGKILL -- and it refuses to bind over an
-- existing file. So the first session to exit poisons the name for every session
-- after it: the tunnel works exactly once per host and is then dead for good,
-- with "remote port forwarding failed for listen path ..." on every later ssh.
--
-- The remote-side cure is `StreamLocalBindUnlink yes` in sshd_config; unlinking
-- here is what recovers hosts whose sshd we cannot change. It is also the only
-- thing that reaches sockets left behind under a name no longer in use (the
-- ssh_config token changed, an alias was renamed) -- nothing else ever will.
--
-- ECONNREFUSED is the proof that no one is listening: a live forward accepts
-- immediately, and a broken Mac-side bridge only shows up after accept. We
-- narrow it further to sockets that ssh itself minted (is_tunnel) and that we
-- own, so this can never touch Rime's own socket or another user's.
--
-- A new master could bind between the refused connect and the unlink, in which
-- case we drop a live socket; the window is sub-millisecond and the next ssh
-- session rebinds, whereas leaving the file costs the tunnel permanently.
local function reap_stale_tunnel(ep, err)
  if ep.kind ~= "unix" or not endpoint.is_tunnel(ep.path) then
    return
  end
  if not (err and tostring(err):find("ECONNREFUSED", 1, true)) then
    return
  end
  local st = uv.fs_lstat(ep.path)
  if not st or st.type ~= "socket" then
    return
  end
  local uid = uv.getuid and uv.getuid() or nil
  if uid and st.uid ~= uid then
    return
  end
  local ok, unlink_err = uv.fs_unlink(ep.path)
  if ok then
    log("Removed dead tunnel socket " .. ep.path .. "; the next ssh session can bind it")
  else
    log("Could not remove dead tunnel socket " .. ep.path .. ": " .. tostring(unlink_err))
  end
end

-- Take `handle` as the live connection. Shared by both dial paths, so the
-- socket ends up in exactly the same state whether or not a greeting was
-- verified first.
local function adopt(handle, ep, already_reading)
  socket = handle
  connected = true
  connecting = false
  attempt = 0
  active_endpoint = ep

  if ep.kind == "unix" then
    -- sshd creates a forwarded socket under the login umask (often 022 ->
    -- 0755), which would let any other user on that host drive this IME. We
    -- own the file, so tighten it to 0600.
    pcall(function() uv.fs_chmod(ep.path, 384) end)
  end

  -- Without read_start libuv never surfaces the peer's EOF, and writes go on
  -- "succeeding" into a dead socket forever -- which is why a Squirrel restart
  -- used to kill the bridge until nvim itself was restarted. Past the greeting
  -- the protocol is one-way, so any payload is ignored; we are here for the
  -- close notification alone.
  if already_reading then
    pcall(function() handle:read_stop() end)
  end
  handle:read_start(function(read_err, data)
    if read_err or not data then
      vim.schedule(on_close)
    end
  end)

  log("Connected to " .. (ep.kind == "tcp" and (ep.host .. ":" .. ep.port) or ep.path))
  vim.schedule(function()
    flush_pending()
    resync()
  end)
end

local function ep_name(ep)
  return ep.kind == "tcp" and (ep.host .. ":" .. ep.port) or ep.path
end

-- Remote path: dial every candidate at once and keep the first whose greeting
-- names the machine this ssh session came from.
--
-- Concurrent rather than serial because a candidate that is not a bridge at all
-- never answers, and waiting out its timeout before trying the next one would
-- serialise the very case this exists to make fast. In flight they cost one
-- round trip in total instead of one each.
--
-- Refusing is the safe outcome, not a bug: no tunnel means no input method,
-- where guessing means typing into the other laptop's IME.
local function verified_connect(list, expected)
  local done = false
  local live = {}       -- handle -> true, connected and awaiting a greeting
  local remaining = #list
  local timer = nil

  local function release(handle)
    live[handle] = nil
    probe_handles[handle] = nil
    pcall(function() handle:read_stop() end)
    pcall(function() handle:close() end)
  end

  local function finish(winner, ep)
    if done then
      return
    end
    done = true
    if timer then
      pcall(function() timer:stop() end)
      pcall(function() timer:close() end)
      timer = nil
    end
    for handle in pairs(live) do
      if handle ~= winner then
        release(handle)
      end
    end
    if winner then
      live[winner] = nil
      probe_handles[winner] = nil
      adopt(winner, ep, true)
    else
      connecting = false
      schedule_reconnect()
    end
  end

  -- One candidate is out. When the last one goes we have nothing to connect to.
  local function reject(handle)
    if done then
      return
    end
    if handle then
      release(handle)
    end
    remaining = remaining - 1
    if remaining <= 0 then
      log("No tunnel reaches " .. expected .. "; not guessing")
      finish(nil)
    end
  end

  for _, ep in ipairs(list) do
    if done or shutting_down then
      break
    end
    local handle = (ep.kind == "tcp") and uv.new_tcp() or uv.new_pipe(false)
    local buf = ""

    local function on_conn(err)
      if done or shutting_down then
        pcall(function() handle:close() end)
        probe_handles[handle] = nil
        return
      end
      if err then
        reap_stale_tunnel(ep, err)
        vim.schedule(function() reject(handle) end)
        return
      end
      live[handle] = true

      handle:read_start(function(read_err, data)
        if done then
          return
        end
        if read_err or not data then
          vim.schedule(function() reject(handle) end)
          return
        end
        buf = buf .. data
        local line = buf:match("^([^\n]*)\n")
        if not line then
          -- A peer that talks but never sends a newline is not our bridge.
          if #buf > 8192 then
            vim.schedule(function() reject(handle) end)
          end
          return
        end
        -- vim.json.decode is a plain C call, safe in this fast event context;
        -- everything that touches editor state is deferred below.
        local host = endpoint.parse_hello(line, vim.json.decode)
        if host and endpoint.host_matches(host, expected) then
          vim.schedule(function() finish(handle, ep) end)
        else
          vim.schedule(function()
            if host then
              log("Skipping " .. ep_name(ep) .. ": tunnel leads to " .. host
                  .. ", not " .. expected)
            end
            reject(handle)
          end)
        end
      end)
    end

    probe_handles[handle] = true
    local ok, ret, ret_err
    if ep.kind == "tcp" then
      ok, ret, ret_err = pcall(handle.connect, handle, ep.host, ep.port, on_conn)
    else
      ok, ret, ret_err = pcall(handle.connect, handle, ep.path, on_conn)
    end
    if not ok or not ret then
      local dial_err = ok and ret_err or ret
      reap_stale_tunnel(ep, dial_err)
      vim.schedule(function() reject(handle) end)
    end
  end

  if remaining <= 0 then
    finish(nil)
    return
  end

  -- Backstop for a candidate that accepts and then says nothing. Generous: the
  -- greeting crosses the ssh link, measured at 5-30ms on real hosts.
  timer = uv.new_timer()
  timer:start(config.probe_timeout, 0, vim.schedule_wrap(function()
    if not done then
      log("No greeting within " .. config.probe_timeout .. "ms; not guessing")
      finish(nil)
    end
  end))
end

local function try_connect(list, idx)
  -- A candidate walk in flight when VimLeavePre runs would otherwise keep
  -- dialling after disconnect(), and a late success would set socket/connected
  -- back up and have resync() send set/activate/context *after* the
  -- reset(true)+unregister the exit path just sent -- re-registering the very
  -- client we were trying to erase.
  if shutting_down then
    connecting = false
    return
  end

  if idx > #list then
    connecting = false
    schedule_reconnect()
    return
  end

  local ep = list[idx]
  local handle = (ep.kind == "tcp") and uv.new_tcp() or uv.new_pipe(false)

  local function on_result(err)
    if shutting_down then
      -- Close it here: nothing downstream will, and disconnect() has already
      -- forgotten about this handle.
      pcall(function() handle:close() end)
      connecting = false
      return
    end

    if err then
      pcall(function() handle:close() end)
      reap_stale_tunnel(ep, err)
      -- This callback is a fast event context, where vim.fn/vim.env are
      -- forbidden -- and the next step needs both, via candidates() in
      -- schedule_reconnect. Raising here would abort before any retry is
      -- scheduled, i.e. go dormant forever. Hop onto the main loop first.
      vim.schedule(function() try_connect(list, idx + 1) end)
      return
    end

    adopt(handle, ep, false)
  end

  -- The dial can fail without ever invoking on_result: uv_tcp_connect raises
  -- for a host that is not an IP literal ("localhost:9527", which parse()
  -- accepts), and returns nil, err inline for e.g. an unusable address family.
  -- Either way the callback never runs, so `connecting` would stay true for the
  -- life of the process and every later connect()/schedule_reconnect() would
  -- no-op -- a permanently dead client that never even tries the next
  -- candidate. Treat both as this candidate failing.
  local ok, ret, ret_err
  if ep.kind == "tcp" then
    ok, ret, ret_err = pcall(handle.connect, handle, ep.host, ep.port, on_result)
  else
    ok, ret, ret_err = pcall(handle.connect, handle, ep.path, on_result)
  end
  if not ok or not ret then
    local dial_err = ok and ret_err or ret
    log("Dial failed: " .. tostring(dial_err))
    pcall(function() handle:close() end)
    reap_stale_tunnel(ep, dial_err)
    vim.schedule(function() try_connect(list, idx + 1) end)
  end
end

connect = function()
  if shutting_down or connected or connecting or reconnect_timer then
    return
  end
  connecting = true
  local expected = expected_host()
  if expected then
    verified_connect(candidates(), expected)
  else
    -- No ssh session told us which machine to expect, so there is nothing to
    -- verify against and nothing to gain by waiting for a greeting: the local
    -- case, and any setup predating LC_RIME_IME_HOST, behaves exactly as before.
    try_connect(candidates(), 1)
  end
end

local function disconnect()
  shutting_down = true
  if reconnect_timer then
    pcall(function() reconnect_timer:stop() end)
    pcall(function() reconnect_timer:close() end)
    reconnect_timer = nil
  end
  connected = false
  connecting = false
  if socket then
    pcall(function() socket:read_stop() end)
    pcall(function() socket:close() end)
    socket = nil
  end
  -- A probe that has connected but not yet been judged holds an open socket to
  -- some bridge. Left alone it would sit there past VimLeavePre, and a greeting
  -- arriving after the exit path already sent reset+unregister would adopt it
  -- and re-register the very client we just erased.
  for handle in pairs(probe_handles) do
    pcall(function() handle:read_stop() end)
    pcall(function() handle:close() end)
  end
  probe_handles = {}
  active_endpoint = nil
  pending_messages = {}
  log("Disconnected")
end

-- Send message (non-blocking)
local function send(action, data)
  local msg = build_message(action, data)
  log("Queueing: " .. action)

  if connected and socket then
    -- luv returns nil, err on failure; it does NOT raise. The old pcall-only
    -- check could therefore never see a write error.
    local ok, err = socket:write(msg)
    if not ok then
      log("Write failed: " .. tostring(err))
      on_close()
      queue(msg)
    end
    return
  end

  queue(msg)
  connect()
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
  -- Deliberately not deduplicated. Every push also re-claims ownership of the
  -- surrounding context on the server, and that claim is the only thing that
  -- reliably follows the keyboard when the terminal does not report focus
  -- events (common under tmux and ssh). Suppressing an "unchanged" payload
  -- suppressed the re-claim with it, so a second nvim could keep owning the
  -- context while you typed into the first. ~100 bytes per keystroke is
  -- nothing next to the terminal's own traffic.
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

  -- No "is Rime installed here?" gate any more: with ssh forwarding the IME can
  -- live on a completely different machine. Whether an endpoint answers is the
  -- only signal that means anything.
  enabled = true
  shutting_down = false

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
      connect()
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

  -- Leave Insert mode: set ascii and clear context
  vim.api.nvim_create_autocmd("InsertLeave", {
    group = group,
    callback = function()
      if pending_insert_leave_timer then
        pcall(function() pending_insert_leave_timer:stop() end)
        pcall(function() pending_insert_leave_timer:close() end)
        pending_insert_leave_timer = nil
      end
      -- Capture the handle locally: by the time the callback runs, another
      -- InsertLeave may already have replaced the module-level variable, and
      -- closing that one would kill a timer that is still needed.
      local timer = uv.new_timer()
      pending_insert_leave_timer = timer
      timer:start(60, 0, vim.schedule_wrap(function()
        if enabled then
          local mode = vim.fn.mode()
          if mode ~= "i" and mode ~= "R" then
            M.set(true)
            M.deactivate()
            M.clear_context()
          end
        end
        pcall(function() timer:close() end)
        if pending_insert_leave_timer == timer then
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
      connect()
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
