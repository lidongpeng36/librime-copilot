-- Pure helpers for the rime-ime client: endpoint parsing, discovery order,
-- reconnect backoff, instance identity.
--
-- Deliberately free of any vim.* API and of any side effect, so
-- clients/neovim/test/endpoint_spec.lua can drive it with `nvim -l` and no test
-- framework. Keep it that way: everything stateful lives in rime_ime.lua.

local M = {}

-- Where the IME Bridge listens when Rime and Neovim are on the same machine.
M.DEFAULT_SOCKET = "/tmp/rime_copilot_ime.sock"

-- Sockets that `ssh -R` dropped here on our behalf. Every tunnel terminates at
-- the *same* bridge on the local machine, so any live one is equally correct --
-- which is why discovery can be this simple.
M.TUNNEL_GLOB = "/tmp/rime-ime-*.sock"

--- Parse an endpoint string.
--- "/tmp/x.sock"    -> { kind = "unix", path = "/tmp/x.sock" }
--- "127.0.0.1:9527" -> { kind = "tcp", host = "127.0.0.1", port = 9527 }
--- Anything else    -> nil
function M.parse(s)
  if type(s) ~= "string" or s == "" then
    return nil
  end
  -- A host cannot contain a slash, which is what tells "1.2.3.4:80" apart from
  -- a unix path that happens to contain a colon.
  local host, port = s:match("^([^/]-):(%d+)$")
  if host and host ~= "" then
    local p = tonumber(port)
    if p and p >= 1 and p <= 65535 then
      return { kind = "tcp", host = host, port = p }
    end
    return nil
  end
  if s:sub(1, 1) == "/" then
    return { kind = "unix", path = s }
  end
  return nil
end

local function endpoint_key(ep)
  if ep.kind == "unix" then
    return "unix:" .. ep.path
  end
  return "tcp:" .. ep.host .. ":" .. ep.port
end

--- Endpoints to try, in order; the first one that connects wins.
---
--- opts.env        $RIME_IME_SOCKET, or nil
--- opts.configured explicit socket_path from setup(), or nil
--- opts.glob       function(pattern) -> list of paths
--- opts.stat       function(path) -> { type = , uid = , mtime = } or nil
---                 must NOT follow symlinks (lstat), or the ownership check
---                 below can be defeated by a symlink to a socket we do own
--- opts.uid        our uid; endpoints owned by anyone else are skipped
--- opts.log        function(msg), optional; called with the reason a candidate
---                 was dropped
function M.candidates(opts)
  opts = opts or {}
  local out, seen = {}, {}

  -- One stat per path per call: the tunnel branch needs the mtime to order
  -- candidates and usable_unix needs the type/uid, and re-stat'ing would also
  -- open a window where the two disagree.
  local stat_cache, stat_done = {}, {}
  local function stat(path)
    if not opts.stat then
      return nil
    end
    if not stat_done[path] then
      stat_done[path] = true
      stat_cache[path] = opts.stat(path)
    end
    return stat_cache[path]
  end

  -- Every unix candidate goes through this, not just the globbed ones. The
  -- hard-coded default lives in a world-writable /tmp, and on a host where no
  -- Rime runs (the usual remote case) that name is unclaimed: any other user
  -- can bind it first and then receive every context push -- i.e. what you are
  -- typing, per keystroke. $RIME_IME_SOCKET and socket_path get the same
  -- treatment on purpose; a bridge socket owned by another uid is exotic
  -- enough that uniformity is worth more than the flexibility.
  local function usable_unix(path)
    if not opts.stat then
      return true  -- nothing injected to check with
    end
    local st = stat(path)
    if not st then
      -- Absent, not hostile. candidates() is recomputed on every dial, so the
      -- local default must survive "Squirrel has not started yet": dialling it
      -- simply fails and we fall through to the next candidate.
      return true
    end
    if st.type ~= "socket" then
      if opts.log then
        opts.log("skipping " .. path .. ": not a socket (" .. tostring(st.type) .. ")")
      end
      return false
    end
    if opts.uid ~= nil and st.uid ~= opts.uid then
      if opts.log then
        opts.log("skipping " .. path .. ": owned by uid " .. tostring(st.uid)
                 .. ", not " .. tostring(opts.uid))
      end
      return false
    end
    return true
  end

  local function add(s)
    local ep = M.parse(s)
    if not ep then
      return
    end
    -- A tcp endpoint cannot be stat'ed, so it is passed through untouched.
    if ep.kind == "unix" and not usable_unix(ep.path) then
      return
    end
    local key = endpoint_key(ep)
    if seen[key] then
      return
    end
    seen[key] = true
    out[#out + 1] = ep
  end

  add(opts.env)
  add(opts.configured)
  add(M.DEFAULT_SOCKET)

  -- Tunnels are ordered newest-first; add() does the type/ownership filtering.
  local paths = opts.glob and opts.glob(M.TUNNEL_GLOB) or {}
  local usable = {}
  for _, p in ipairs(paths) do
    local st = stat(p)
    usable[#usable + 1] = { path = p, mtime = (st and st.mtime) or 0 }
  end
  table.sort(usable, function(a, b)
    if a.mtime == b.mtime then
      return a.path < b.path
    end
    return a.mtime > b.mtime
  end)
  for _, u in ipairs(usable) do
    add(u.path)
  end

  return out
end

--- Exponential backoff with a ceiling. `attempt` is 1-based; returns ms.
function M.backoff(attempt, base, max)
  base = base or 1000
  max = max or 30000
  if not attempt or attempt < 1 then
    attempt = 1
  end
  local delay = base * (2 ^ (attempt - 1))
  if not (delay < max) then  -- also catches inf from a huge attempt count
    delay = max
  end
  return math.floor(delay)
end

--- Instance identity. Must be unique across machines: a remote nvim and a local
--- one easily share a pid, and the bridge keys per-client ascii_mode state
--- (depth/base) on this string, so a collision corrupts both.
function M.instance_id(o)
  o = o or {}
  local parts = { o.hostname or "unknown", tostring(o.pid or 0) }
  if o.term_session_id and o.term_session_id ~= "" then
    parts[#parts + 1] = o.term_session_id:sub(1, 8)
  elseif o.windowid and o.windowid ~= "" then
    parts[#parts + 1] = o.windowid
  end
  return table.concat(parts, "-")
end

return M
