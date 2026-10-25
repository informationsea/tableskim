"""Drives the TUI (tableskim -t) in a pseudo-terminal for end-to-end tests.

Linux only: the program runs under pty with a pyte screen reading its
output, so that a test sends keys and waits for text on the screen.
"""

import fcntl
import os
import pty
import select
import shutil
import signal
import struct
import tempfile
import termios
import time

import pyte

COLUMNS = 120
LINES = 40
TIMEOUT = 15.0


class TuiTimeout(AssertionError):
    pass


class Tui:
    """One run of the TUI on a file. Use as a context manager."""

    KEYS = {
        "enter": "\r",
        "esc": "\x1b",
        "tab": "\t",
        "backspace": "\x7f",
        "up": "\x1b[A",
        "down": "\x1b[B",
        "right": "\x1b[C",
        "left": "\x1b[D",
        "ctrl+s": "\x13",
        "ctrl+c": "\x03",
        "ctrl+n": "\x0e",
        "ctrl+u": "\x15",
        "delete": "\x1b[3~",
    }

    def __init__(self, executable, path, args=(), home=None, columns=COLUMNS, lines=LINES):
        self.executable = executable
        self.path = str(path)
        self.args = list(args)
        self.columns = columns
        self.lines = lines
        self._own_home = home is None
        self.home = home or tempfile.mkdtemp(prefix="tableskim-tui-home-")
        self.screen = pyte.Screen(columns, lines)
        self.stream = pyte.ByteStream(self.screen)
        self.pid = None
        self.fd = None
        self.exit_status = None

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.close()
        return False

    def start(self):
        env = dict(os.environ)
        # Keeps the SQL history and recent files out of the user's own.
        env["HOME"] = self.home
        env["XDG_STATE_HOME"] = os.path.join(self.home, ".local", "state")
        env["XDG_DATA_HOME"] = os.path.join(self.home, ".local", "share")
        env["XDG_CONFIG_HOME"] = os.path.join(self.home, ".config")
        env["TERM"] = "xterm-256color"
        env.pop("DISPLAY", None)
        env.pop("WAYLAND_DISPLAY", None)
        pid, fd = pty.fork()
        if pid == 0:
            try:
                os.execve(self.executable, [self.executable, "-t", *self.args, self.path], env)
            finally:
                os._exit(127)
        self.pid = pid
        self.fd = fd
        fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", self.lines, self.columns, 0, 0))
        os.kill(pid, signal.SIGWINCH)

    def close(self):
        if self.pid is not None and self.exit_status is None:
            try:
                os.kill(self.pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            self._reap(block=True)
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None
        if self._own_home:
            shutil.rmtree(self.home, ignore_errors=True)

    def _reap(self, block=False):
        if self.exit_status is not None:
            return True
        pid, status = os.waitpid(self.pid, 0 if block else os.WNOHANG)
        if pid == 0:
            return False
        self.exit_status = status
        return True

    def pump(self, seconds=0.05):
        """Reads what the program wrote, for up to `seconds`."""
        end = time.monotonic() + seconds
        while True:
            remaining = end - time.monotonic()
            if remaining <= 0 or self.fd is None:
                return
            ready, _, _ = select.select([self.fd], [], [], remaining)
            if not ready:
                return
            try:
                data = os.read(self.fd, 65536)
            except OSError:  # the program exited
                return
            if not data:
                return
            self.stream.feed(data)
            # FTXUI asks where the cursor is; answer so that it goes on.
            if b"\x1b[6n" in data:
                y, x = self.screen.cursor.y + 1, self.screen.cursor.x + 1
                os.write(self.fd, f"\x1b[{y};{x}R".encode())

    def text(self):
        return "\n".join(line.rstrip() for line in self.screen.display)

    def line(self, index):
        return self.screen.display[index].rstrip()

    def send(self, *keys):
        """Sends keys: names in KEYS, or text sent as typed."""
        for key in keys:
            data = self.KEYS.get(key, key)
            os.write(self.fd, data.encode())
            self.pump(0.05)

    def type(self, text):
        os.write(self.fd, text.encode())
        self.pump(0.05)

    def wait_for(self, predicate, timeout=TIMEOUT, what="the condition"):
        """Waits until predicate(text) holds; returns the screen text."""
        end = time.monotonic() + timeout
        while True:
            self.pump(0.05)
            text = self.text()
            if predicate(text):
                return text
            if self._reap():
                raise TuiTimeout(f"The program exited ({self.exit_status}) before {what}:\n{text}")
            if time.monotonic() > end:
                raise TuiTimeout(f"Timed out waiting for {what}:\n{text}")

    def wait_text(self, needle, timeout=TIMEOUT):
        return self.wait_for(lambda text: needle in text, timeout, repr(needle))

    def wait_gone(self, needle, timeout=TIMEOUT):
        return self.wait_for(lambda text: needle not in text, timeout, f"{needle!r} to go")

    def settle(self, quiet=0.3, timeout=TIMEOUT):
        """Waits until the screen stops changing for `quiet` seconds."""
        end = time.monotonic() + timeout
        last = self.text()
        stable_since = time.monotonic()
        while time.monotonic() < end:
            self.pump(0.05)
            text = self.text()
            if text != last:
                last = text
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= quiet:
                return text
        return last

    def wait_exit(self, timeout=TIMEOUT):
        end = time.monotonic() + timeout
        while time.monotonic() < end:
            self.pump(0.05)
            if self._reap():
                return self.exit_status
        raise TuiTimeout("The program did not exit:\n" + self.text())
