-- Pure-Lua tests for rime_ime.endpoint. No test framework on purpose: the
-- module has no vim.* dependency, so this runs anywhere.
--
--   nvim -l clients/neovim/test/endpoint_spec.lua

local here = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"

-- loadfile, not require: nvim resolves require() through its own runtimepath
-- loader first, so on a machine that has this plugin installed under
-- ~/.config/nvim/lua the spec would silently test the *installed* copy instead
-- of the one in this checkout. That is invisible while the two agree and
-- actively misleading the moment they do not.
local endpoint = assert(loadfile(here .. "../lua/rime_ime/endpoint.lua"))()

local failures = 0
local function check(ok, msg)
  if not ok then
    failures = failures + 1
    io.stderr:write("FAIL: " .. msg .. "\n")
  end
end
local function eq(got, want, msg)
  check(got == want, string.format("%s (got %s, want %s)", msg, tostring(got), tostring(want)))
end

-- Every candidate list now ends with the forwarded loopback port. The cases
-- below are about which *paths* survive filtering, so they look at those alone;
-- the port has its own section.
local function unix_only(list)
  local out = {}
  for _, ep in ipairs(list) do
    if ep.kind == "unix" then
      out[#out + 1] = ep
    end
  end
  return out
end

-- parse -----------------------------------------------------------------

do
  local ep = endpoint.parse("/tmp/rime_copilot_ime.sock")
  eq(ep and ep.kind, "unix", "absolute path is a unix endpoint")
  eq(ep and ep.path, "/tmp/rime_copilot_ime.sock", "unix path is kept verbatim")

  ep = endpoint.parse("127.0.0.1:9527")
  eq(ep and ep.kind, "tcp", "host:port is a tcp endpoint")
  eq(ep and ep.host, "127.0.0.1", "tcp host")
  eq(ep and ep.port, 9527, "tcp port is a number")

  ep = endpoint.parse("localhost:1")
  eq(ep and ep.port, 1, "port 1 is valid")

  -- A unix path may legitimately contain a colon; the slash disambiguates.
  ep = endpoint.parse("/tmp/weird:1.sock")
  eq(ep and ep.kind, "unix", "colon inside an absolute path stays unix")

  eq(endpoint.parse(""), nil, "empty string is not an endpoint")
  eq(endpoint.parse(nil), nil, "nil is not an endpoint")
  eq(endpoint.parse("relative/path.sock"), nil, "relative path is rejected")
  eq(endpoint.parse("host:0"), nil, "port 0 is rejected")
  eq(endpoint.parse("host:65536"), nil, "port 65536 is rejected")
  eq(endpoint.parse("host:notaport"), nil, "non-numeric port is rejected")
end

-- candidates ------------------------------------------------------------

do
  local list = unix_only(endpoint.candidates({ glob = function() return {} end }))
  eq(#list, 1, "with nothing configured, only the default remains")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "default socket is the fallback")

  list = endpoint.candidates({
    env = "127.0.0.1:9527",
    configured = "/tmp/explicit.sock",
    glob = function() return {} end,
  })
  eq(#list, 4, "env + configured + default + the forwarded port")
  eq(list[1].kind, "tcp", "env wins")
  eq(list[2].path, "/tmp/explicit.sock", "configured comes second")
  eq(list[3].path, endpoint.DEFAULT_SOCKET, "default comes last of the three")

  -- Duplicates collapse: configuring the default explicitly must not make us
  -- dial the same socket twice.
  list = unix_only(endpoint.candidates({
    configured = endpoint.DEFAULT_SOCKET,
    glob = function() return {} end,
  }))
  eq(#list, 1, "duplicate endpoints are collapsed")
end

do
  -- Tunnels are discovered by glob, newest first, and only ours are used: a
  -- socket owned by another user is somebody else's tunnel to somebody else's
  -- IME, and dialling it would drive their input method.
  local stats = {
    ["/tmp/rime-ime-aaa.sock"] = { type = "socket", uid = 501, mtime = 100 },
    ["/tmp/rime-ime-bbb.sock"] = { type = "socket", uid = 501, mtime = 300 },
    ["/tmp/rime-ime-ccc.sock"] = { type = "socket", uid = 999, mtime = 400 },
    ["/tmp/rime-ime-ddd.sock"] = { type = "file", uid = 501, mtime = 500 },
  }
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function()
      return {
        "/tmp/rime-ime-aaa.sock",
        "/tmp/rime-ime-bbb.sock",
        "/tmp/rime-ime-ccc.sock",
        "/tmp/rime-ime-ddd.sock",
      }
    end,
    stat = function(p) return stats[p] end,
  }))
  eq(#list, 3, "default + two usable tunnels")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "local default is still tried first")
  eq(list[2].path, "/tmp/rime-ime-bbb.sock", "newest usable tunnel comes first")
  eq(list[3].path, "/tmp/rime-ime-aaa.sock", "older tunnel comes next")
end

do
  -- A stale socket left by a crashed session is kept in the list; connecting to
  -- it fails immediately and we fall through, so no cleanup logic is needed.
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function() return { "/tmp/rime-ime-stale.sock" } end,
    stat = function() return { type = "socket", uid = 501, mtime = 1 } end,
  }))
  eq(#list, 2, "stale-looking sockets are still offered")
end

-- candidates: the type+ownership filter applies to EVERY unix candidate ------
-- Not just globbed ones. /tmp is world-writable, and on a remote host where no
-- Rime runs the hard-coded default is an unclaimed name any other user can bind
-- first -- and would then receive a copy of every keystroke.

do
  -- Outcome 1 of 3 for a non-glob candidate: stat says nil (path absent).
  -- Keep it. candidates() is recomputed on every dial, so pruning here would
  -- make "Squirrel has not started yet" permanent for the life of the process.
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function() return {} end,
    stat = function() return nil end,
  }))
  eq(#list, 1, "an absent default is still offered")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "...and it is the default")
end

do
  -- Outcome 2 of 3: it exists but is not a socket. Drop it.
  local logged = {}
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function() return {} end,
    stat = function() return { type = "file", uid = 501, mtime = 1 } end,
    log = function(m) logged[#logged + 1] = m end,
  }))
  eq(#list, 0, "a non-socket at the default path is dropped")
  eq(#logged, 1, "and the rejection is logged")
  check(logged[1] and logged[1]:find("not a socket", 1, true) ~= nil,
        "the log says why: " .. tostring(logged[1]))
end

do
  -- Outcome 3 of 3: a socket, but someone else's. Drop it. This is the actual
  -- keystroke-leak case on a shared host.
  local logged = {}
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function() return {} end,
    stat = function() return { type = "socket", uid = 999, mtime = 1 } end,
    log = function(m) logged[#logged + 1] = m end,
  }))
  eq(#list, 0, "a socket owned by another uid is dropped")
  eq(#logged, 1, "and the rejection is logged")
  check(logged[1] and logged[1]:find("uid 999", 1, true) ~= nil,
        "the log names the owner: " .. tostring(logged[1]))
end

do
  -- A symlink is what init.lua's lstat reports for a planted link to a
  -- socket we do own; it must not survive the type check.
  local list = unix_only(endpoint.candidates({
    uid = 501,
    glob = function() return {} end,
    stat = function() return { type = "link", uid = 501, mtime = 1 } end,
  }))
  eq(#list, 0, "a symlink is not a socket and is dropped")
end

do
  -- $RIME_IME_SOCKET and socket_path get no exemption. Only the good one and
  -- the (absent, hence kept) default survive.
  local stats = {
    ["/tmp/env-hijacked.sock"] = { type = "socket", uid = 999, mtime = 9 },
    ["/tmp/configured-regular-file"] = { type = "file", uid = 501, mtime = 9 },
    ["/tmp/rime-ime-good.sock"] = { type = "socket", uid = 501, mtime = 9 },
  }
  local list = unix_only(endpoint.candidates({
    uid = 501,
    env = "/tmp/env-hijacked.sock",
    configured = "/tmp/configured-regular-file",
    glob = function() return { "/tmp/rime-ime-good.sock" } end,
    stat = function(p) return stats[p] end,
  }))
  eq(#list, 2, "hijacked env and bogus configured path are both dropped")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "the absent default is still tried")
  eq(list[2].path, "/tmp/rime-ime-good.sock", "our own tunnel survives")
end

do
  -- A tcp endpoint cannot be stat'ed, so the filter must leave it alone even
  -- when stat() would answer for every path handed to it.
  local list = endpoint.candidates({
    uid = 501,
    env = "127.0.0.1:9527",
    glob = function() return {} end,
    stat = function() return { type = "file", uid = 999, mtime = 1 } end,
  })
  eq(#list, 2, "the tcp endpoints remain: the configured one and the default port")
  eq(list[1].kind, "tcp", "tcp endpoints are not filtered by stat")
  eq(list[1].port, 9527, "and the configured one still comes first")
end

do
  -- With no stat injected nothing can be checked, so nothing is dropped: the
  -- module stays usable (and testable) without a filesystem.
  local list = unix_only(endpoint.candidates({ uid = 501, glob = function() return {} end }))
  eq(#list, 1, "without stat, candidates are passed through")
end

-- listener_uids -----------------------------------------------------------
-- Real /proc/net/tcp rows. Port 19527 is 0x4C47; state 0A is TCP_LISTEN.

local PROC_TCP = [[
  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode
   0: 0100007F:0C08 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1001        0 111 1 0 100 0 0 10 0
   3: 0100007F:4C47 00000000:0000 0A 00000000:00000000 00:00000000 00000000  1000        0 222 1 0 100 0 0 10 0
   4: 0100007F:4C47 0100007F:9CA1 01 00000000:00000000 00:00000000 00000000  1010        0 333 1 0 100 0 0 10 0
]]

do
  local uids = endpoint.listener_uids(PROC_TCP, 19527)
  eq(#uids, 1, "one LISTEN socket on 19527")
  eq(uids[1], 1000, "and it is owned by uid 1000")

  -- Row 4 is an ESTABLISHED connection *to* that port under another uid. Only
  -- the listener decides who receives what we send; counting it would refuse a
  -- port that is perfectly ours.
  eq(#endpoint.listener_uids(PROC_TCP, 3080), 1, "port 0x0C08 has its own listener")
  eq(endpoint.listener_uids(PROC_TCP, 3080)[1], 1001, "owned by uid 1001")

  eq(#endpoint.listener_uids(PROC_TCP, 9999), 0, "no listener on an unused port")
  eq(#endpoint.listener_uids("", 19527), 0, "empty content yields nothing")
  eq(#endpoint.listener_uids(nil, 19527), 0, "nil content yields nothing")
end

-- candidates: the loopback tunnel port ------------------------------------

do
  -- Present by default and tried last, so a live local Rime always wins.
  local list = endpoint.candidates({ glob = function() return {} end })
  eq(#list, 2, "default socket plus the forwarded port")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "the unix socket is tried first")
  eq(list[2].kind, "tcp", "the port is the fallback")
  eq(list[2].port, endpoint.DEFAULT_TCP_PORT, "on the agreed port")
end

do
  -- The whole point of the uid check: another user holding the port must not
  -- receive our context pushes.
  local logged = {}
  local list = endpoint.candidates({
    uid = 1000,
    glob = function() return {} end,
    tcp_owner = function() return { 999 } end,
    log = function(m) logged[#logged + 1] = m end,
  })
  eq(#list, 1, "a port owned by another user is dropped")
  eq(list[1].kind, "unix", "only the unix default is left")
  check(logged[1] and logged[1]:find("uid 999", 1, true), "the log names the owner")
end

do
  local list = endpoint.candidates({
    uid = 1000,
    glob = function() return {} end,
    tcp_owner = function() return { 1000 } end,
  })
  eq(#list, 2, "our own listener is kept")
end

do
  -- Nothing listening yet is not a reason to drop it: ssh may connect later.
  local list = endpoint.candidates({
    uid = 1000,
    glob = function() return {} end,
    tcp_owner = function() return {} end,
  })
  eq(#list, 2, "an unbound port is still offered")

  -- nil means "cannot tell" (no /proc). Keep it, as on macOS.
  list = endpoint.candidates({
    uid = 1000,
    glob = function() return {} end,
    tcp_owner = function() return nil end,
  })
  eq(#list, 2, "an uncheckable port is still offered")
end

do
  -- A port on another machine cannot be checked against this machine's table,
  -- so tcp_owner must not be consulted for it.
  local asked = {}
  local list = endpoint.candidates({
    uid = 1000,
    env = "10.0.0.5:9527",
    glob = function() return {} end,
    tcp_owner = function(port) asked[port] = true; return { 999 } end,
  })
  check(not asked[9527], "tcp_owner is not consulted for a non-loopback host")
  check(asked[endpoint.DEFAULT_TCP_PORT], "it is consulted for the loopback default")
  eq(list[1].host, "10.0.0.5", "the remote endpoint survives unchecked")
  local loopback = false
  for _, ep in ipairs(list) do
    if ep.kind == "tcp" and ep.host == "127.0.0.1" then loopback = true end
  end
  check(not loopback, "while the loopback default, owned by uid 999, is dropped")
end

-- is_tunnel ---------------------------------------------------------------
-- Gates what init.lua may unlink, so over-matching here means deleting a socket
-- that is not ours to delete.

do
  check(endpoint.is_tunnel("/tmp/rime-ime-lidongpeng-udeer.sock"),
        "a forwarded tunnel socket is recognised")
  check(endpoint.is_tunnel("/tmp/rime-ime-a.sock"), "a one-character id counts")

  check(not endpoint.is_tunnel(endpoint.DEFAULT_SOCKET),
        "Rime's own socket is NOT a tunnel and must never be unlinked")
  check(not endpoint.is_tunnel("/tmp/rime-ime-.sock"),
        "the wildcard must match at least one character")
  check(not endpoint.is_tunnel("/tmp/rime-ime-x.sock/../../etc/passwd"),
        "the wildcard must not span a directory separator")
  check(not endpoint.is_tunnel("/tmp/sub/rime-ime-x.sock"),
        "a tunnel name in another directory does not count")
  check(not endpoint.is_tunnel("/tmp/rime-ime-x.socket"), "the suffix must match exactly")
  check(not endpoint.is_tunnel("/var/tmp/rime-ime-x.sock"), "the prefix must match exactly")
  check(not endpoint.is_tunnel("/tmp/other.sock"), "an unrelated socket does not count")
  check(not endpoint.is_tunnel(nil), "nil is not a tunnel")
  check(not endpoint.is_tunnel(42), "a non-string is not a tunnel")
end

-- backoff ---------------------------------------------------------------

do
  eq(endpoint.backoff(1, 1000, 30000), 1000, "first retry uses the base delay")
  eq(endpoint.backoff(2, 1000, 30000), 2000, "second retry doubles")
  eq(endpoint.backoff(3, 1000, 30000), 4000, "third retry doubles again")
  eq(endpoint.backoff(6, 1000, 30000), 30000, "capped at max")
  eq(endpoint.backoff(1000, 1000, 30000), 30000, "a huge attempt count still caps")
  eq(endpoint.backoff(0, 1000, 30000), 1000, "attempt 0 is treated as 1")
end

-- instance_id -----------------------------------------------------------

do
  eq(endpoint.instance_id({ hostname = "mac", pid = 123 }), "mac-123", "hostname and pid")
  eq(endpoint.instance_id({ hostname = "devbox", pid = 123 }), "devbox-123",
     "same pid on another host is a different instance")
  eq(endpoint.instance_id({ hostname = "mac", pid = 1, term_session_id = "ABCDEFGHIJK" }),
     "mac-1-ABCDEFGH", "term session id is truncated to 8 chars")
  eq(endpoint.instance_id({ hostname = "mac", pid = 1, windowid = "77" }), "mac-1-77",
     "windowid is used when there is no term session id")
  eq(endpoint.instance_id({ hostname = "mac", pid = 1, term_session_id = "" }), "mac-1",
     "an empty term session id is ignored")
end

-- ------------------------------------------------------------------------

if failures > 0 then
  io.stderr:write(failures .. " failure(s)\n")
  os.exit(1)
end
print("endpoint_spec: all checks passed")
