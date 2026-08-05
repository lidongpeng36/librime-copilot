-- Pure helpers for the rime-ime client: endpoint parsing, discovery order,
-- reconnect backoff, instance identity.
--
-- Deliberately free of any vim.* API and of any side effect, so
-- clients/neovim/test/endpoint_spec.lua can drive it with `nvim -l` and no test
-- framework. Keep it that way: everything stateful lives in init.lua.

local M = {}

-- Where the IME Bridge listens when Rime and Neovim are on the same machine.
M.DEFAULT_SOCKET = "/tmp/rime_copilot_ime.sock"

-- Sockets that `ssh -R` dropped here on our behalf. Every tunnel terminates at
-- the *same* bridge on the local machine, so any live one is equally correct --
-- which is why discovery can be this simple.
M.TUNNEL_GLOB = "/tmp/rime-ime-*.sock"

-- The loopback port form of the same tunnel, and the preferred one. A forwarded
-- *socket file* outlives its listener: sshd never unlinks it -- not on a clean
-- `ssh -O exit`, not on SIGKILL -- and refuses to bind over what is left, so the
-- tunnel works exactly once per host unless the remote sshd sets
-- StreamLocalBindUnlink. A port has no such residue: when the listener goes, the
-- port is free, and the next ssh binds it again. Nothing to configure remotely,
-- nothing to remember after reinstalling a machine.
--
-- The cost is that a port carries no ownership, where a socket file has a uid.
-- listener_uids() below buys that back on Linux.
M.DEFAULT_TCP_PORT = 19527
M.DEFAULT_TCP = "127.0.0.1:" .. M.DEFAULT_TCP_PORT

local LOOPBACK = { ["127.0.0.1"] = true, ["localhost"] = true, ["::1"] = true }

--- uids owning LISTEN sockets on `port`, parsed from /proc/net/tcp{,6} content.
--- Returns {} when nothing is listening, and a list (usually of one) otherwise.
---
--- This is the port-world equivalent of the uid check on a socket file. Without
--- it, any other user on the host can bind the forwarded port while your ssh is
--- away and then receive every context push -- i.e. what you are typing.
function M.listener_uids(content, port)
  local out = {}
  if type(content) ~= "string" or type(port) ~= "number" then
    return out
  end
  local want = string.format("%04X", port)
  for line in content:gmatch("[^\n]+") do
    -- sl: local_address rem_address st tx:rx tr:when retrnsmt uid
    local local_addr, _, state, _, _, _, uid =
      line:match("^%s*%d+:%s+(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s+(%S+)%s+(%d+)")
    if local_addr and state == "0A" then  -- 0A = TCP_LISTEN
      local hex = local_addr:match(":(%x+)$")
      if hex and hex:upper() == want then
        out[#out + 1] = tonumber(uid)
      end
    end
  end
  return out
end

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

  -- A loopback port is checkable the way a socket file is: whoever holds the
  -- LISTEN socket owns it. A port on another host is not -- opts.tcp_owner reads
  -- this machine's table -- so those are passed through.
  local function usable_tcp(ep)
    if not opts.tcp_owner or opts.uid == nil or not LOOPBACK[ep.host] then
      return true
    end
    local uids = opts.tcp_owner(ep.port)
    if not uids then
      return true  -- no way to tell here (no /proc, e.g. macOS)
    end
    for _, u in ipairs(uids) do
      if u ~= opts.uid then
        if opts.log then
          opts.log("skipping 127.0.0.1:" .. ep.port .. ": listener owned by uid "
                   .. tostring(u) .. ", not " .. tostring(opts.uid))
        end
        return false
      end
    end
    return true
  end

  local function add(s)
    local ep = M.parse(s)
    if not ep then
      return
    end
    if ep.kind == "unix" and not usable_unix(ep.path) then
      return
    end
    if ep.kind == "tcp" and not usable_tcp(ep) then
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

  -- Last: the forwarded loopback port. On the machine Rime runs on, the unix
  -- socket above has already won, so this only ever carries the remote case.
  add(M.DEFAULT_TCP)

  return out
end

--- True when `path` is one of the ssh-forwarded tunnel sockets -- a name our own
--- ssh_config minted, as opposed to Rime's socket or anything a user configured
--- by hand. Derived from TUNNEL_GLOB so the two cannot drift apart.
---
--- This gates what init.lua is allowed to unlink when a candidate turns out to
--- be dead, so it has to be exact: the `*` must match at least one character and
--- may not span a directory separator, or "/tmp/rime-ime-" and
--- "/tmp/rime-ime-x.sock/../../etc/passwd" would both qualify.
function M.is_tunnel(path)
  if type(path) ~= "string" then
    return false
  end
  local prefix, suffix = M.TUNNEL_GLOB:match("^(.-)%*(.*)$")
  if not prefix then
    return false
  end
  if #path <= #prefix + #suffix then
    return false
  end
  if path:sub(1, #prefix) ~= prefix then
    return false
  end
  if suffix ~= "" and path:sub(-#suffix) ~= suffix then
    return false
  end
  local middle = path:sub(#prefix + 1, #path - #suffix)
  return middle ~= "" and not middle:find("/", 1, true)
end

-- Tunnel discovery for the remote case -------------------------------------
--
-- `RemoteForward 127.0.0.1:0` lets sshd pick a free port, so two laptops
-- sharing one remote account each get their own tunnel instead of racing for
-- one number -- and a port, unlike a socket file, leaves nothing behind when
-- the forward dies. The price is that nobody knows the number in advance, so
-- the client has to find it and then prove it leads to the right machine.

--- Ports listening on IPv4 loopback, owned by `uid`, parsed from the content of
--- /proc/net/tcp (or tcp6, same columns).
---
--- `lo`/`hi`, when given, keep only ports inside the ephemeral range. That is
--- not a heuristic: a bind to port 0 is served from exactly that range, so a
--- long-lived service on a well-known port (8080, 11434, ...) cannot be one of
--- our tunnels and does not deserve a probe.
function M.listen_ports(content, uid, lo, hi)
  local out = {}
  if type(content) ~= "string" then
    return out
  end
  for line in content:gmatch("[^\n]+") do
    local addr, state, owner =
      line:match("^%s*%d+:%s+(%S+)%s+%S+%s+(%S+)%s+%S+%s+%S+%s+%S+%s+(%d+)")
    if addr and state == "0A" then  -- 0A = TCP_LISTEN
      local ip, hex = addr:match("^(%x+):(%x+)$")
      -- 0100007F is 127.0.0.1 in the little-endian hex /proc uses.
      if ip == "0100007F" and (uid == nil or tonumber(owner) == uid) then
        local port = tonumber(hex, 16)
        if port and (not lo or (port >= lo and port <= hi)) then
          out[#out + 1] = port
        end
      end
    end
  end
  table.sort(out)
  return out
end

--- "10240\t65535" -> 10240, 65535. nil when unparseable, which means "do not
--- filter by range" rather than "drop everything".
function M.parse_port_range(content)
  if type(content) ~= "string" then
    return nil
  end
  local lo, hi = content:match("(%d+)%s+(%d+)")
  if not lo then
    return nil
  end
  return tonumber(lo), tonumber(hi)
end

--- Loopback ports held by sshd, from `lsof -nP -iTCP -sTCP:LISTEN -a -u<uid>`.
--- macOS has no /proc, but it does let us see the owning process, so there the
--- candidate set is exact and no unrelated service is ever probed.
function M.lsof_ssh_ports(output)
  local out, seen = {}, {}
  if type(output) ~= "string" then
    return out
  end
  for line in output:gmatch("[^\n]+") do
    local cmd = line:match("^(%S+)")
    if cmd and cmd:lower():find("sshd", 1, true) then
      local port = line:match("127%.0%.0%.1:(%d+)%s+%(LISTEN%)")
      if port and not seen[port] then
        seen[port] = true
        out[#out + 1] = tonumber(port)
      end
    end
  end
  table.sort(out)
  return out
end

--- The host named in a `hello` greeting, or nil if this is not one.
function M.parse_hello(line, decode)
  if type(line) ~= "string" or not decode then
    return nil
  end
  local ok, msg = pcall(decode, line)
  if not ok or type(msg) ~= "table" then
    return nil
  end
  if msg.ns ~= "rime.ime" or msg.type ~= "hello" then
    return nil
  end
  local host = type(msg.data) == "table" and msg.data.host or nil
  if type(host) ~= "string" or host == "" then
    return nil
  end
  return host
end

--- Does a greeting from `host` mean we reached the machine we came from?
--- Hostnames are case-insensitive, and ssh's %L is truncated at the first dot,
--- so compare the short forms.
function M.host_matches(host, expected)
  if type(host) ~= "string" or type(expected) ~= "string" then
    return false
  end
  local function short(s)
    return (s:gsub("%..*$", "")):lower()
  end
  return short(host) ~= "" and short(host) == short(expected)
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
