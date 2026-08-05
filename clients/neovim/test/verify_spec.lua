-- End-to-end tests for greeting verification: the real connection path, real
-- libuv sockets and timers, against fake bridges that greet as one machine or
-- another. endpoint_spec covers the pure helpers; this covers the part where
-- handles, timers and cleanup can actually go wrong.
--
--   nvim -l clients/neovim/test/verify_spec.lua
--
-- Each scenario runs in its own nvim: reloading the module leaves the previous
-- instance's sockets and timers alive in the event loop, and they go on talking,
-- which silently invalidates whatever the next scenario asserts.

local here = debug.getinfo(1, "S").source:sub(2):match("(.*/)") or "./"
local luadir = here .. "../lua"
local scenario = arg and arg[1] or nil

local SCENARIOS = { "picks-matching", "refuses-mismatch", "silent-peer", "unverified-unchanged" }

-- Driver: no scenario given, so run each one in a child process.
if not scenario then
  local failed = {}
  for _, s in ipairs(SCENARIOS) do
    local cmd = string.format("%q -l %q %s 2>&1", vim.v.progpath, here .. "verify_spec.lua", s)
    local pipe = assert(io.popen(cmd, "r"))
    local out = pipe:read("*a")
    local ok = pipe:close()
    if not ok then
      failed[#failed + 1] = s
      io.stderr:write(out)
    end
  end
  if #failed > 0 then
    io.stderr:write("verify_spec: failed scenarios: " .. table.concat(failed, ", ") .. "\n")
    os.exit(1)
  end
  print("verify_spec: all scenarios passed")
  return
end

-- package.preload, not package.path: nvim's runtimepath loader runs before the
-- path searcher, so on a machine with this plugin installed under
-- ~/.config/nvim the spec would silently exercise the *installed* copy. That is
-- invisible while the two agree and actively misleading the moment they differ.
package.preload["rime_ime.endpoint"] = function()
  return assert(loadfile(luadir .. "/rime_ime/endpoint.lua"))()
end
package.preload["rime_ime"] = function()
  return assert(loadfile(luadir .. "/rime_ime/init.lua"))()
end

local uv = vim.uv or vim.loop
local failures = 0
local function check(ok, msg)
  if not ok then
    failures = failures + 1
    io.stderr:write("FAIL[" .. scenario .. "]: " .. msg .. "\n")
  end
end

--- A bridge that greets as `host` (or never greets, when host is nil) and
--- records everything a client sends it.
local function bridge(path, host)
  os.remove(path)
  local server = uv.new_pipe(false)
  local received = {}
  assert(server:bind(path))
  assert(server:listen(16, function()
    local c = uv.new_pipe(false)
    server:accept(c)
    if host then
      c:write('{"v":1,"ns":"rime.ime","type":"hello","data":{"host":"' .. host .. '"}}\n')
    end
    c:read_start(function(err, data)
      if err or not data then
        pcall(function() c:close() end)
        return
      end
      received[#received + 1] = data
    end)
  end))
  return {
    received = received,
    close = function()
      pcall(function() server:close() end)
      os.remove(path)
    end,
  }
end

local function settle(ms)
  vim.wait(ms, function() return false end)
end

local MINE = "/tmp/rime-verify-spec-mine.sock"
local THEIRS = "/tmp/rime-verify-spec-theirs.sock"

if scenario == "picks-matching" then
  -- Two laptops, one remote account: both tunnels are reachable and both look
  -- equally valid. Only the greeting tells them apart.
  local mine, theirs = bridge(MINE, "my-laptop"), bridge(THEIRS, "other-laptop")
  vim.env.LC_RIME_IME_HOST = "my-laptop"
  vim.env.RIME_IME_SOCKET = THEIRS  -- dialled first, and must still lose
  require("rime_ime").setup({ socket_path = MINE })
  settle(1500)
  require("rime_ime").set(true)
  settle(600)
  check(#mine.received > 0, "the matching bridge receives our messages")
  check(#theirs.received == 0, "the other laptop's bridge receives nothing")
  mine.close()
  theirs.close()

elseif scenario == "refuses-mismatch" then
  -- The only tunnel present leads somewhere else. Refusing is the whole point:
  -- no input method beats driving someone else's.
  local theirs = bridge(THEIRS, "other-laptop")
  vim.env.LC_RIME_IME_HOST = "my-laptop"
  vim.env.RIME_IME_SOCKET = THEIRS
  require("rime_ime").setup({ socket_path = THEIRS })
  settle(1500)
  require("rime_ime").set(true)
  settle(600)
  check(#theirs.received == 0, "a mismatched tunnel is never written to")
  theirs.close()

elseif scenario == "silent-peer" then
  -- Something that accepts and then says nothing -- an unrelated service that
  -- happened to be listening. It must time out, and must not leave `connecting`
  -- set, or every later attempt would no-op for the life of the process.
  local mute = bridge(THEIRS, nil)
  vim.env.LC_RIME_IME_HOST = "my-laptop"
  vim.env.RIME_IME_SOCKET = THEIRS
  require("rime_ime").setup({ socket_path = THEIRS, probe_timeout = 300 })
  settle(1200)
  check(#mute.received == 0, "a silent peer is never written to")
  mute.close()

  local mine = bridge(MINE, "my-laptop")
  vim.env.RIME_IME_SOCKET = MINE
  settle(4000)  -- let the reconnect backoff come around
  require("rime_ime").set(true)
  settle(800)
  check(#mine.received > 0, "recovers once a matching bridge appears")
  mine.close()

elseif scenario == "unverified-unchanged" then
  -- No LC_RIME_IME_HOST: running where Rime itself runs, or an ssh setup that
  -- predates greetings. Nothing may change there, including working against a
  -- bridge too old to greet at all.
  vim.env.LC_RIME_IME_HOST = nil
  local old = bridge(MINE, nil)
  vim.env.RIME_IME_SOCKET = MINE
  require("rime_ime").setup({ socket_path = MINE })
  settle(1200)
  require("rime_ime").set(true)
  settle(600)
  check(#old.received > 0, "unverified path still talks to a greeting-less bridge")
  old.close()

else
  io.stderr:write("unknown scenario: " .. tostring(scenario) .. "\n")
  os.exit(1)
end

if failures > 0 then
  os.exit(1)
end
