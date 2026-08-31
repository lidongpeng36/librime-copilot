"""The tmux identity reporter's wire format, pinned byte for byte.

rime_ctx_report.sh is invoked from a tmux hook on every pane switch and talks
to the running plugin over a Unix socket the plugin's C++ handler parses. The
two sides cannot be exercised together by an automated test -- the handler
lives behind a real Rime engine -- so this test pins the one thing that can
silently drift between them: the exact JSON Lines message this script emits.
A mismatch here fails silently in production, since the plugin just ignores
an unrecognized message and the feature quietly falls back to polling.

Fakes stand in for both `tmux` (so the pane/command are known) and `nc` (so
the message can be captured without a real listener) -- `nc -U` exists in BSD
netcat but not GNU netcat, so a test that needed a live Unix-socket listener
would be flaky on Linux CI. Only `[ -S "$SOCK" ]` needs a real socket special
file, which Python's own `socket.AF_UNIX` bind() can create without anything
listening on it.
"""
from __future__ import annotations

import json
import os
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

SCRIPT = Path(__file__).resolve().parents[1] / "rime_ctx_report.sh"

FAKE_TMUX = """#!/bin/sh
printf '%s\\n' '%7|claude'
"""

# A tmux fake that leaves evidence of having been invoked at all -- used to
# pin the early-exit restored in test_bare_invocation_with_no_socket_never_
# spawns_tmux: the assertion there is that `tmux display-message` is never
# run, and a fake that only controls its OWN output can't show that; a marker
# file it touches on every invocation can.
FAKE_TMUX_WITH_MARKER = """#!/bin/sh
: > "$TMUX_INVOKED_MARKER"
printf '%s\\n' '%7|claude'
"""

# NOTE the lifecycle mismatch, and do not "fix" it by making this fake sleep:
# `cat` exits at stdin EOF, which is the OPPOSITE of what macOS's nc does.
# The real nc waits for the REMOTE to close, and ImeBridgeServer never closes
# -- so the fake is faithful about the bytes and structurally incapable of
# reproducing the leak the bytes travel through. That is what
# `test_nc_is_invoked_with_a_connection_timeout` covers instead; see the
# comment there before deleting it as redundant.
FAKE_NC = """#!/bin/sh
cat > "$NC_CAPTURE_FILE"
"""

FAKE_NC_TCP = """#!/bin/sh
printf '%s\\n' "$*" >> "$NC_ARGS_FILE"
cat >> "$NC_CAPTURE_FILE"
[ -z "${NC_FAIL:-}" ] || exit 1
exit 0
"""


def _write_fake(path: Path, content: str) -> None:
    path.write_text(content)
    path.chmod(0o755)


class RimeCtxReportTest(unittest.TestCase):
    def setUp(self):
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.fakebin = Path(self.tmp.name) / "fakebin"
        self.fakebin.mkdir()
        _write_fake(self.fakebin / "tmux", FAKE_TMUX)
        _write_fake(self.fakebin / "nc", FAKE_NC)
        self.capture = Path(self.tmp.name) / "captured.jsonl"

        # AF_UNIX paths are capped at ~104-108 bytes (sun_path); a path under
        # tempfile's default dir (long on macOS) can exceed that, so the
        # socket itself lives directly under /tmp rather than self.tmp.
        self.sockdir = tempfile.mkdtemp(dir="/tmp")
        self.sock_path = os.path.join(self.sockdir, "s.sock")
        self.addCleanup(self._cleanup_sockdir)

    def _cleanup_sockdir(self):
        if os.path.exists(self.sock_path):
            os.unlink(self.sock_path)
        if os.path.isdir(self.sockdir):
            os.rmdir(self.sockdir)

    def _make_socket_file(self) -> socket.socket:
        """A bound-but-not-listening AF_UNIX socket: enough for `[ -S ]`."""
        sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        sock.bind(self.sock_path)
        self.assertTrue(stat.S_ISSOCK(os.stat(self.sock_path).st_mode))
        return sock

    def _run(self, extra_env=None, args=None):
        env = dict(os.environ)
        env["PATH"] = f"{self.fakebin}{os.pathsep}{env.get('PATH', '')}"
        env["RIME_COPILOT_IME_SOCK"] = self.sock_path
        env["NC_CAPTURE_FILE"] = str(self.capture)
        # A real tmux session's env var: the part before the first comma is
        # the server's socket path, which is what data.socket carries --
        # whole, not its basename. See
        # test_socket_field_is_the_full_path_the_plugin_keys_on.
        env["TMUX"] = "/tmp/tmux-501/default,12345,0"
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            ["/bin/sh", str(SCRIPT), *(args or ())],
            env=env,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            timeout=5,
        )

    def _wait_for_capture(self, timeout=2.0) -> str:
        """The message is written by a backgrounded `nc`, not the script
        itself, so give it a moment to land rather than racing it."""
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.capture.exists() and self.capture.stat().st_size > 0:
                return self.capture.read_text()
            time.sleep(0.02)
        return self.capture.read_text() if self.capture.exists() else ""

    def test_wire_format_matches_the_plugin_handler_byte_for_byte(self):
        sock = self._make_socket_file()
        try:
            result = self._run()
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, b"")

        captured = self._wait_for_capture()
        expected = (
            '{"v":1,"ns":"rime.ime","type":"identity",'
            '"data":{"socket":"/tmp/tmux-501/default","pane":"%7","command":"claude"}}\n'
        )
        self.assertEqual(captured, expected)

    def test_socket_field_is_the_full_path_the_plugin_keys_on(self):
        """The pushed rung's `socket` must be what the polled rung derives.

        The polled rung keys on tmux's `#{socket_path}` (Snapshot::socket_path,
        src/tmux_source_util.h), which is an absolute path. `basename` here
        used to make the two rungs build different keys for the same pane on
        any non-default socket -- and the default socket hid it, because
        MakeKey renders an empty socket as "default", which is also that
        socket's basename.

        The other half of this pin is
        test/context_identity_test.cc's PolledKeyForANonDefaultSocketIsThe-
        ReportersString: it asserts this exact string keys to
        `tmux:/tmp/tmux-501/work:%7|claude`. Change one, change both.
        """
        sock = self._make_socket_file()
        try:
            result = self._run({"TMUX": "/tmp/tmux-501/work,12345,0"})
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        captured = self._wait_for_capture()
        self.assertIn('"socket":"/tmp/tmux-501/work"', captured)

    def test_arguments_win_over_asking_tmux(self):
        """Told beats asked, and the fake tmux here disagrees on purpose.

        `display-message -p` with no `-t` resolves against the INVOKING
        client, not the hook's target. Measured on tmux 3.7c, 2026-08-31:
        `tmux select-window -t copilot:3` run from a pane in another session
        fired the hook, and the hook's own `display-message -p` reported that
        other session's pane (`librime:1 %3 claude`) rather than `%5`. Both
        `run-shell` and `run-shell -b` did it, so it is not an async race.

        tmux expands `#{pane_id}` in the hook command against the hook's own
        target, correctly -- verified in the same session. So the hook passes
        the identity in and the script stops asking.

        The fake tmux reports `%7|claude`; this test passes `%3 zsh`. A script
        that falls back to asking emits `%7` and fails here.
        """
        sock = self._make_socket_file()
        try:
            result = self._run(args=["%3", "zsh"])
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        captured = self._wait_for_capture()
        self.assertIn('"pane":"%3"', captured)
        self.assertIn('"command":"zsh"', captured)
        self.assertNotIn("%7", captured)
        self.assertNotIn("claude", captured)

    def test_socket_argument_wins_over_the_TMUX_variable(self):
        """`#{socket_path}` is what the POLLED rung reads, so prefer it.

        `${TMUX%%,*}` is correct too -- measured, a hook's environment carries
        the hook target's own `$TMUX` even while `display-message` is looking
        at another client -- and it stays as the fallback for a tmux too old
        to report `#{socket_path}`. Taking the argument when it is there makes
        both rungs read literally the same tmux format, which is the property
        `MakeKey`'s note in src/context_memory.h demands.
        """
        sock = self._make_socket_file()
        try:
            result = self._run(
                {"TMUX": "/tmp/tmux-501/wrong,12345,0"},
                args=["%3", "zsh", "/tmp/tmux-501/right"],
            )
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        captured = self._wait_for_capture()
        self.assertIn('"socket":"/tmp/tmux-501/right"', captured)
        self.assertNotIn("wrong", captured)

    def test_still_asks_tmux_when_invoked_with_no_arguments(self):
        """The fallback is not vestigial: it is what a hand-run invocation
        and every .tmux.conf written before 2026-08-31 use. Dropping it would
        turn an out-of-date hook from `slightly wrong under scripted
        switches` into `silently does nothing`."""
        sock = self._make_socket_file()
        try:
            result = self._run()
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        captured = self._wait_for_capture()
        self.assertIn('"pane":"%7"', captured)
        self.assertIn('"command":"claude"', captured)

    def test_bare_invocation_with_no_socket_never_spawns_tmux(self):
        """Regression: the `[ -S "$SOCK" ]` guard used to run before ANY call
        into tmux, so a machine with no bridge running paid nothing for a
        bare hook invocation. Once identity became hook arguments, the guard
        moved after the `tmux display-message` fallback -- so a `.tmux.conf`
        written before 2026-08-31 (which invokes this script bare on every
        pane switch) now pays a posix_spawn on every switch on a machine
        where the bridge is simply not running. No socket is created here at
        all -- the fixed script must fail closed before ever touching tmux.
        """
        _write_fake(self.fakebin / "tmux", FAKE_TMUX_WITH_MARKER)
        marker = Path(self.tmp.name) / "tmux_invoked"
        self.assertFalse(marker.exists())

        result = self._run(extra_env={"TMUX_INVOKED_MARKER": str(marker)})

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse(self.capture.exists())
        self.assertFalse(marker.exists(), "tmux was spawned despite no bridge socket")

    def test_bare_invocation_with_a_socket_present_still_asks_tmux(self):
        """The other half of the guard above: when the bridge IS running,
        the bare-invocation fallback must still fire -- the early exit must
        not become an early exit ALWAYS."""
        _write_fake(self.fakebin / "tmux", FAKE_TMUX_WITH_MARKER)
        marker = Path(self.tmp.name) / "tmux_invoked"
        sock = self._make_socket_file()
        try:
            result = self._run(extra_env={"TMUX_INVOKED_MARKER": str(marker)})
        finally:
            sock.close()
            os.unlink(self.sock_path)

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertTrue(marker.exists(), "tmux was never spawned")
        captured = self._wait_for_capture()
        self.assertIn('"pane":"%7"', captured)

    def test_nc_is_invoked_with_a_connection_timeout(self):
        """The committed script must pass a connection timeout to `nc`.

        This asserts on the SOURCE rather than on behaviour, and that is not
        laziness -- it is the only place the thing can be checked. The fake
        `nc` above is `cat`, which exits at stdin EOF; macOS's real nc does
        not, it waits for the remote to close, and
        `ImeBridgeServer::HandleConnection` never closes. So every pane switch
        without `-w` leaks an nc, a detached server thread parked in read(),
        and an fd inside Squirrel until it is restarted. No fake can model
        that: a fake faithful enough to hang would just hang this test.

        Measured on macOS 2026-08-29: `nc -U <sock>` against a
        non-closing server was still alive at 3s and 6s; `nc -w 1 -U <sock>`
        delivered the message, saw EOF and exited immediately.

        `-N` is NOT a substitute: macOS's -N takes an argument and swallows
        -U (`nc: invalid tcp adaptive write timeout value`, exit 1), which
        stops the reporter working altogether -- worse than the leak.

        Deliberately no assertion on HOW MANY `nc` invocations there are: an
        exact count here has been bumped by every task that added one, on
        zero bugs caught (1 -> 2 -> 3 across two tasks), while the thing that
        actually caught a real bug is `-N` being absent. So every invocation
        the script has, whatever the count, gets checked -- and
        `assertTrue(nc_lines)` guards against the count silently going to
        zero, which an unconditional loop over an empty list would pass.
        """
        body = SCRIPT.read_text()
        nc_lines = [ln for ln in body.splitlines()
                    if "nc " in ln and not ln.strip().startswith("#")]
        self.assertTrue(nc_lines, "expected at least one nc invocation")
        for invocation in nc_lines:
            self.assertRegex(invocation, r"\bnc\s+-w\s*\d")
            self.assertNotRegex(invocation, r"\bnc\s+(-\w+\s+)*-N\b")

    def test_inert_when_the_socket_path_is_not_a_socket(self):
        # No AF_UNIX file created at self.sock_path at all: `[ -S "$SOCK" ]`
        # must fail closed, silently, and the script must not hang.
        result = self._run()

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(result.stdout, b"")
        self.assertEqual(result.stderr, b"")
        self.assertFalse(self.capture.exists())

    def test_inert_when_a_plain_file_sits_at_the_socket_path(self):
        # A regular file (e.g. a stale path) must not pass `[ -S ]` either.
        Path(self.sock_path).write_text("not a socket")
        result = self._run()

        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse(self.capture.exists())

    def test_an_empty_fourth_argument_stays_in_local_mode(self):
        """同一份 tmux.conf 同步到所有机器，本机拿到的第 4 参数是空串。
        空串必须走 unix socket 并且不发 host/expect —— 否则本机的每次
        pane 切换都会去拨回环端口。"""
        sock = self._make_socket_file()
        try:
            result = self._run(args=["%3", "zsh", "/tmp/tmux-501/default", ""])
        finally:
            sock.close()
            os.unlink(self.sock_path)
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        captured = self._wait_for_capture()
        self.assertIn('"pane":"%3"', captured)
        self.assertNotIn("host", captured)
        self.assertNotIn("expect", captured)


class RemoteModeTest(unittest.TestCase):
    """第 4 个参数非空 = 远端模式：转发端口、带 host 与 expect。

    和本机模式共用一个脚本，因为用户把同一份 tmux.conf 同步到所有机器 ——
    钩子行必须在两边都成立，脚本必须跟着配置一起走。
    """

    def setUp(self):
        self.assertTrue(SCRIPT.exists(), f"missing {SCRIPT}")
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.fakebin = Path(self.tmp.name) / "fakebin"
        self.fakebin.mkdir()
        _write_fake(self.fakebin / "tmux", FAKE_TMUX)
        _write_fake(self.fakebin / "nc", FAKE_NC_TCP)
        self.capture = Path(self.tmp.name) / "captured.jsonl"
        self.ncargs = Path(self.tmp.name) / "ncargs.txt"
        self.runtime = Path(self.tmp.name) / "run"
        self.runtime.mkdir()

    def _cache_path(self) -> Path:
        return self.runtime / f"rime_ctx_endpoint.{os.getuid()}"

    def _run(self, args, extra_env=None):
        env = dict(os.environ)
        env["PATH"] = f"{self.fakebin}{os.pathsep}{env.get('PATH', '')}"
        env["NC_CAPTURE_FILE"] = str(self.capture)
        env["NC_ARGS_FILE"] = str(self.ncargs)
        env["XDG_RUNTIME_DIR"] = str(self.runtime)
        env["TMUX"] = "/tmp/tmux-1000/default,999,0"
        env["RIME_CTX_HOSTNAME"] = "devbox"
        env.pop("RIME_IME_ENDPOINT", None)
        if extra_env:
            env.update(extra_env)
        return subprocess.run(
            ["/bin/sh", str(SCRIPT), *args],
            env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=5)

    def _wait_for_capture(self, timeout=2.0) -> str:
        deadline = time.time() + timeout
        while time.time() < deadline:
            if self.capture.exists() and self.capture.stat().st_size > 0:
                return self.capture.read_text()
            time.sleep(0.02)
        return self.capture.read_text() if self.capture.exists() else ""

    def test_wire_format_carries_this_hosts_name_and_the_expected_recipient(self):
        """host 是**远端自己**的名字，expect 是**本机**的。

        把两者搞反是这个设计最容易犯的错：$LC_RIME_IME_HOST 对每台远端
        都相同，拿它做 host 会把所有远端塌成一个命名空间 —— 也就是这个
        功能要修的 bug 本身，只是升了一层。这里两者故意不同。
        """
        result = self._run(
            ["%2", "zsh", "/tmp/tmux-1000/default", "Mac-Mini"],
            extra_env={"RIME_IME_ENDPOINT": "127.0.0.1:19527"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        expected = (
            '{"v":1,"ns":"rime.ime","type":"identity","data":'
            '{"expect":"Mac-Mini","host":"devbox","socket":"/tmp/tmux-1000/default",'
            '"pane":"%2","command":"zsh"}}\n')
        self.assertEqual(self._wait_for_capture(), expected)

    def test_remote_mode_dials_a_port_not_a_unix_socket(self):
        self._run(["%2", "zsh", "", "Mac-Mini"],
                  extra_env={"RIME_IME_ENDPOINT": "127.0.0.1:19527"})
        self._wait_for_capture()
        args = self.ncargs.read_text()
        self.assertIn("127.0.0.1", args)
        self.assertIn("19527", args)
        self.assertNotIn("-U", args)
        self.assertRegex(args, r"-w\s*\d")

    def test_a_successful_send_writes_the_cache(self):
        self._run(["%2", "zsh", "", "Mac-Mini"],
                  extra_env={"RIME_IME_ENDPOINT": "127.0.0.1:19527"})
        self._wait_for_capture()
        self.assertEqual(self._cache_path().read_text().strip(), "127.0.0.1:19527")

    def test_a_cached_endpoint_is_used_without_an_override(self):
        self._cache_path().write_text("127.0.0.1:12345\n")
        self._run(["%2", "zsh", "", "Mac-Mini"])
        self._wait_for_capture()
        self.assertIn("12345", self.ncargs.read_text())

    def test_a_world_writable_cached_endpoint_is_ignored(self):
        """Without XDG_RUNTIME_DIR the cache lives directly under /tmp, which
        is world-writable, at a path (this uid) any local user can predict.
        Another user pre-creating it there, world-writable, pointing at
        their own listener would receive this host's hostname, tmux socket
        path, pane id and current command with no further prompt -- unless a
        permissive cache is refused outright rather than dialled. The
        malicious port (12345) must never be dialled at all; only the
        legitimate discovery candidate (50001) may be."""
        cache = self._cache_path()
        cache.write_text("127.0.0.1:12345\n")
        os.chmod(cache, 0o666)
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PORTS": "50001"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        args = self.ncargs.read_text() if self.ncargs.exists() else ""
        self.assertNotIn("12345", args)

    def test_a_foreign_owned_cached_endpoint_is_ignored(self):
        """Same hazard as above, the other half of it: a cache file this
        user does not own, whatever its permission bits say. `ls` is faked
        here -- and ONLY here, `_cache_is_safe` is the sole caller of `ls` in
        this script -- to report an owner uid this test process is not, so
        the rejection is exercised without needing an actual second uid or
        root to chown a fixture with."""
        fake_ls = self.fakebin / "ls"
        _write_fake(fake_ls, "#!/bin/sh\nprintf -- '-rw------- 1 1 1 5 Jan 1 00:00 x\\n'\n")
        cache = self._cache_path()
        cache.write_text("127.0.0.1:12345\n")
        os.chmod(cache, 0o600)  # otherwise-pristine permissions -- only the owner is foreign
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PORTS": "50001"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        args = self.ncargs.read_text() if self.ncargs.exists() else ""
        self.assertNotIn("12345", args)

    def test_a_freshly_written_cache_endpoint_file_has_no_group_or_other_bits(self):
        """The other half of the fix: a cache written by THIS script must not
        itself start out group/other writable (or readable) -- refusing to
        READ an unsafe file is not enough if every write leaves a wide-open
        file sitting at a predictable path for the next pane switch to trust
        again."""
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_IME_ENDPOINT": "127.0.0.1:19527"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self._wait_for_capture()
        mode = stat.S_IMODE(self._cache_path().stat().st_mode)
        self.assertEqual(mode & 0o077, 0, oct(mode))

    def test_a_failed_send_leaves_the_cached_endpoint_alone(self):
        """本机 bridge 在重部署与下一次按键之间没有 socket。

        那是个正常窗口，不是 endpoint 失效 —— 隧道好好的，监听者只是
        暂时不在。清掉缓存会让每次重部署之后的第一次 pane 切换白做一遍
        端口发现，而发现本身在那个窗口里也注定失败。
        """
        self._cache_path().write_text("127.0.0.1:12345\n")
        result = self._run(["%2", "zsh", "", "Mac-Mini"], extra_env={"NC_FAIL": "1"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self._cache_path().read_text().strip(), "127.0.0.1:12345")

    def test_pinned_endpoint_is_authoritative_even_when_the_send_fails(self):
        """RIME_IME_ENDPOINT names where this goes ON PURPOSE -- the escape
        hatch for a shared macOS remote where candidate_ports()'s uid check
        has no /proc to run against. A failed send there must not fall
        through to discovery: that would let the pin undo itself on exactly
        the transient failure it exists to survive -- the window between a
        redeploy and the next keystroke where the laptop's bridge briefly
        has no socket -- and start spraying port probes from a host that was
        told precisely where to send. Unlike the cached-endpoint case (see
        test_a_failed_send_leaves_the_cached_endpoint_alone just above,
        which keeps falling through on purpose -- that is inference, not
        instruction), a pin exits either way.
        """
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_IME_ENDPOINT": "127.0.0.1:19527",
                       "NC_FAIL": "1",
                       "RIME_CTX_PORTS": "40000 40001"})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        args = self.ncargs.read_text()
        self.assertIn("19527", args)
        # Discovery's candidates must never have been dialled.
        self.assertNotIn("40000", args)
        self.assertNotIn("40001", args)
        # Nothing worth remembering from a failed pinned send.
        self.assertFalse(self._cache_path().exists())

    def _serve(self, host_in_greeting: str) -> int:
        srv = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        srv.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        srv.bind(("127.0.0.1", 0))
        srv.listen(4)
        port = srv.getsockname()[1]

        def loop():
            while True:
                try:
                    conn, _ = srv.accept()
                except OSError:
                    return
                greeting = json.dumps(
                    {"data": {"host": host_in_greeting}, "ns": "rime.ime",
                     "type": "hello", "v": 1},
                    separators=(",", ":"), sort_keys=True)
                try:
                    conn.sendall((greeting + "\n").encode())
                    conn.settimeout(1.0)
                    try:
                        conn.recv(4096)
                    except OSError:
                        pass
                finally:
                    conn.close()

        threading.Thread(target=loop, daemon=True).start()
        self.addCleanup(srv.close)
        return port

    def test_discovery_keeps_the_tunnel_whose_greeting_matches(self):
        """两个真的回环监听者：一个报对主机名，一个报错的。

        fake 在这里不够用 —— 要验的就是「读一行问候、比对 host」这个动作，
        而 clients/neovim 的设计记录里量过：两台笔记本连同一个远端账号，
        各自的隧道从远端看是无法区分的，只有问候能分辨。所以这两个用例
        用真 nc 而不是 setUp 里那个 fake。
        """
        if shutil.which("nc") is None:
            self.skipTest("no nc on PATH; discovery dials for real")
        wrong = self._serve("SomeOtherLaptop")
        right = self._serve("Mac-Mini")
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PORTS": f"{wrong} {right}",
                       "PATH": os.environ.get("PATH", "")})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertEqual(self._cache_path().read_text().strip(), f"127.0.0.1:{right}")

    def test_discovery_writes_no_cache_when_nothing_matches(self):
        if shutil.which("nc") is None:
            self.skipTest("no nc on PATH; discovery dials for real")
        wrong = self._serve("SomeOtherLaptop")
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PORTS": str(wrong),
                       "PATH": os.environ.get("PATH", "")})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertFalse(self._cache_path().exists())

    # ---- candidate_ports(): the /proc/net/tcp + ip_local_port_range logic ----
    #
    # test_discovery_* above both pass RIME_CTX_PORTS, which bypasses
    # candidate_ports() entirely -- neither the uid filter nor the range
    # filter is exercised by anything above this line. These use
    # RIME_CTX_PROC_TCP / RIME_CTX_PORT_RANGE_FILE, the seams that let a
    # fixture drive that logic for real, with the (still fake, arg-logging)
    # nc from setUp: nothing in these fixtures is a real listener, so every
    # candidate greeting_matches() tries fails to match and the script falls
    # through having *tried* every one of them -- which is exactly what lets
    # a test read $NC_ARGS_FILE afterwards and see precisely which ports
    # candidate_ports() handed it, in order.

    def _fixture_proc_tcp(self, entries) -> Path:
        """A /proc/net/tcp fixture: `entries` is a list of (uid, port) pairs,
        each rendered as a LISTEN row on 127.0.0.1. Shape and field order
        (uid at whitespace-split field 8) match a real captured line from a
        live Linux host:

          1: 0100007F:6E17 00000000:0000 0A 00000000:00000000 00:00000000
             00000000  1000        0 12345 1 0000000000000000 100 0 0 10 0
        """
        lines = [
            "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when "
            "retrnsmt   uid  timeout inode"
        ]
        for uid, port in entries:
            addr = "0100007F:%04X" % port
            lines.append(
                "   1: %s 00000000:0000 0A 00000000:00000000 00:00000000 "
                "00000000 %d 0 12345 1 0000000000000000 100 0 0 10 0" % (addr, uid)
            )
        path = Path(self.tmp.name) / "proc_net_tcp"
        path.write_text("\n".join(lines) + "\n")
        return path

    def _fixture_range(self, content: str) -> Path:
        path = Path(self.tmp.name) / "ip_local_port_range"
        path.write_text(content)
        return path

    def test_candidate_ports_only_probes_this_uids_listeners(self):
        """The uid check is the security property, not tidiness: on a shared
        host a port belongs to whoever binds it first, so without it another
        user's listener on 127.0.0.1 could be probed (and, if it answered a
        forged greeting, could receive what this user types)."""
        my_uid = os.getuid()
        other_uid = my_uid + 1
        proc_tcp = self._fixture_proc_tcp([(my_uid, 53211), (other_uid, 53212)])
        range_file = self._fixture_range("1024 65535\n")
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PROC_TCP": str(proc_tcp),
                       "RIME_CTX_PORT_RANGE_FILE": str(range_file)})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        args = self.ncargs.read_text()
        self.assertIn("53211", args)
        self.assertNotIn("53212", args)

    def test_candidate_ports_keeps_in_range_and_drops_out_of_range(self):
        my_uid = os.getuid()
        proc_tcp = self._fixture_proc_tcp([(my_uid, 40000), (my_uid, 5000)])
        range_file = self._fixture_range("30000 50000\n")
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PROC_TCP": str(proc_tcp),
                       "RIME_CTX_PORT_RANGE_FILE": str(range_file)})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        args = self.ncargs.read_text()
        self.assertIn("40000", args)
        self.assertNotIn("5000", args)

    def test_candidate_ports_ignores_an_unparseable_range_file_instead_of_dropping_everything(self):
        """Regression for a bug shipped in the Task 6 brief itself, reproduced
        live under `dash`: the range file being READABLE but not a clean pair
        of integers (missing second field, non-numeric field, ...) used to
        make `[ "$_p" -le "$_hi" ]` error ("Illegal number") for EVERY
        candidate, which reads as false and filters all of them -- the exact
        "drop everything" failure the surrounding comment warns against by
        name. Unreadable already degraded correctly (no filter); this is the
        readable-but-garbage case, which did not.

        The fixture below has only one field ("10240", no second word) --
        `read -r _lo _hi` leaves `_hi` empty, which is the shape the reviewer
        called out specifically ("the file's second field is non-numeric or
        missing"). Must still probe the candidate, unfiltered.
        """
        my_uid = os.getuid()
        proc_tcp = self._fixture_proc_tcp([(my_uid, 45000)])
        range_file = self._fixture_range("10240\n")
        result = self._run(
            ["%2", "zsh", "", "Mac-Mini"],
            extra_env={"RIME_CTX_PROC_TCP": str(proc_tcp),
                       "RIME_CTX_PORT_RANGE_FILE": str(range_file)})
        self.assertEqual(result.returncode, 0, result.stderr.decode())
        self.assertIn("45000", self.ncargs.read_text())


if __name__ == "__main__":
    unittest.main()
