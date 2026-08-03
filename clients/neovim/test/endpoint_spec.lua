-- Pure-Lua tests for rime_ime.endpoint. No test framework on purpose: the
-- module has no vim.* dependency, so this runs anywhere.
--
--   nvim -l clients/neovim/test/endpoint_spec.lua

local here = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"
package.path = here .. "../lua/?.lua;" .. here .. "../lua/?/init.lua;" .. package.path

local endpoint = require("rime_ime.endpoint")

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
  local list = endpoint.candidates({ glob = function() return {} end })
  eq(#list, 1, "with nothing configured, only the default remains")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "default socket is the fallback")

  list = endpoint.candidates({
    env = "127.0.0.1:9527",
    configured = "/tmp/explicit.sock",
    glob = function() return {} end,
  })
  eq(#list, 3, "env + configured + default")
  eq(list[1].kind, "tcp", "env wins")
  eq(list[2].path, "/tmp/explicit.sock", "configured comes second")
  eq(list[3].path, endpoint.DEFAULT_SOCKET, "default comes last of the three")

  -- Duplicates collapse: configuring the default explicitly must not make us
  -- dial the same socket twice.
  list = endpoint.candidates({
    configured = endpoint.DEFAULT_SOCKET,
    glob = function() return {} end,
  })
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
  local list = endpoint.candidates({
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
  })
  eq(#list, 3, "default + two usable tunnels")
  eq(list[1].path, endpoint.DEFAULT_SOCKET, "local default is still tried first")
  eq(list[2].path, "/tmp/rime-ime-bbb.sock", "newest usable tunnel comes first")
  eq(list[3].path, "/tmp/rime-ime-aaa.sock", "older tunnel comes next")
end

do
  -- A stale socket left by a crashed session is kept in the list; connecting to
  -- it fails immediately and we fall through, so no cleanup logic is needed.
  local list = endpoint.candidates({
    uid = 501,
    glob = function() return { "/tmp/rime-ime-stale.sock" } end,
    stat = function() return { type = "socket", uid = 501, mtime = 1 } end,
  })
  eq(#list, 2, "stale-looking sockets are still offered")
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
