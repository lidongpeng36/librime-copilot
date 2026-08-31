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

import os
import socket
import stat
import subprocess
import sys
import tempfile
import time
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

SCRIPT = Path(__file__).resolve().parents[1] / "rime_ctx_report.sh"

FAKE_TMUX = """#!/bin/sh
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
        """
        body = SCRIPT.read_text()
        nc_lines = [ln for ln in body.splitlines()
                    if "nc " in ln and not ln.strip().startswith("#")]
        self.assertEqual(len(nc_lines), 1, f"expected one nc invocation, found {nc_lines}")
        invocation = nc_lines[0]
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


if __name__ == "__main__":
    unittest.main()
