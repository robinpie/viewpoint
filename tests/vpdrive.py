# SPDX-License-Identifier: GPL-3.0-only
#
# Copyright (C) 2026  robinpie <robin@dreamstation.systems>
#
# This program is free software: you can redistribute it and/or modify
# it under the terms of the GNU General Public License as published by
# the Free Software Foundation, version 3.
#
# This program is distributed in the hope that it will be useful,
# but WITHOUT ANY WARRANTY; without even the implied warranty of
# MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
# GNU General Public License for more details.
#
# You should have received a copy of the GNU General Public License
# along with this program.  If not, see <https://www.gnu.org/licenses/>.

"""Drive a real viewpoint on a real pty and read back what it drew.

This is the black-box half of the test helpers: it runs the built binary as a
subprocess and never links project code, so what it reports is what a user
would actually see.

Two pieces of terminal reality make that harder than piping bytes in, and this
module exists to handle both:

  * notcurses interrogates the terminal at startup (cursor position, device
    attributes, palette entries) and blocks until it gets answers. A pipe never
    answers, so the app hangs before drawing a frame; a pty alone doesn't help,
    because nothing is on the other end to reply. Session.respond() plays the
    terminal's part.

  * viewpoint keeps a detached session daemon holding live windows, keyed on
    $XDG_RUNTIME_DIR. Sharing the developer's runtime dir means inheriting
    whatever windows a previous run left behind, so every Session gets a
    private one - and, because the daemon setsid()s out of our process group,
    stop() hunts it down by that same private path.

Typical use:

    with Session() as vp:
        vp.click(2, 5)                       # the Settings desktop icon
        scr = vp.wait_for(lambda s: "Keybindings" in s.text())
        print(scr)

Waiting is by predicate rather than by sleep: the app redraws when it has
something to say, and a fixed delay is either slower than it needs to be or
flaky on a loaded machine.
"""

import codecs
import fcntl
import os
import pty
import re
import select
import signal
import shutil
import struct
import sys
import tempfile
import termios
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BINARY = os.path.join(ROOT, "viewpoint")

DEFAULT_ROWS = 30
DEFAULT_COLS = 100

# ---------------------------------------------------------------------------
# the screen model
# ---------------------------------------------------------------------------

CSI = re.compile(r"\x1b\[([0-9;?<>=!$\"' ]*)([@-~])")
OSC = re.compile(r"\x1b\][^\x07\x1b]*(?:\x07|\x1b\\)", re.S)
DCS = re.compile(r"\x1b[PX^_].*?\x1b\\", re.S)
ESC2 = re.compile(r"\x1b[ #%()*+].")
ESC1 = re.compile(r"\x1b[a-zA-Z0-9=><]")

DEFAULT_FG = None  # "the terminal's default", i.e. no explicit color set


class Screen:
    """A cell grid fed terminal output incrementally.

    Only what notcurses actually emits is interpreted - absolute positioning,
    erases, and truecolor SGR. Colors are kept per cell because they carry
    real information here: the size indicator's fade is a color ramp, so a
    test that could only read text would be blind to it.
    """

    def __init__(self, rows=DEFAULT_ROWS, cols=DEFAULT_COLS):
        self.rows = rows
        self.cols = cols
        self.reset()

    def reset(self):
        self.ch = [[" "] * self.cols for _ in range(self.rows)]
        self.fg = [[DEFAULT_FG] * self.cols for _ in range(self.rows)]
        self.bg = [[DEFAULT_FG] * self.cols for _ in range(self.rows)]
        self.y = 0
        self.x = 0
        self.cur_fg = DEFAULT_FG
        self.cur_bg = DEFAULT_FG
        self._pending = ""
        # A multi-byte character can straddle two reads, so decoding is
        # incremental: feeding each chunk independently would turn the split
        # ones into replacement characters.
        self._decoder = codecs.getincrementaldecoder("utf-8")("replace")

    # -- painting ----------------------------------------------------------

    def _put(self, ch):
        if 0 <= self.y < self.rows and 0 <= self.x < self.cols:
            self.ch[self.y][self.x] = ch
            self.fg[self.y][self.x] = self.cur_fg
            self.bg[self.y][self.x] = self.cur_bg
        self.x += 1
        if self.x >= self.cols:  # notcurses positions absolutely, but be safe
            self.x = self.cols - 1

    def _erase_display(self, mode):
        if mode == 2 or mode == 3:
            cells = [(r, c) for r in range(self.rows) for c in range(self.cols)]
        elif mode == 1:
            cells = [(r, c) for r in range(self.rows) for c in range(self.cols)
                     if (r, c) <= (self.y, self.x)]
        else:
            cells = [(r, c) for r in range(self.rows) for c in range(self.cols)
                     if (r, c) >= (self.y, self.x)]
        for r, c in cells:
            self.ch[r][c] = " "
            self.fg[r][c] = DEFAULT_FG
            self.bg[r][c] = DEFAULT_FG

    def _erase_line(self, mode):
        if mode == 1:
            span = range(0, min(self.x + 1, self.cols))
        elif mode == 2:
            span = range(0, self.cols)
        else:
            span = range(self.x, self.cols)
        for c in span:
            if 0 <= self.y < self.rows:
                self.ch[self.y][c] = " "
                self.fg[self.y][c] = DEFAULT_FG
                self.bg[self.y][c] = DEFAULT_FG

    def _sgr(self, nums):
        if not nums:
            nums = [0]
        i = 0
        while i < len(nums):
            n = nums[i]
            if n == 0:
                self.cur_fg = self.cur_bg = DEFAULT_FG
            elif n == 39:
                self.cur_fg = DEFAULT_FG
            elif n == 49:
                self.cur_bg = DEFAULT_FG
            elif n in (38, 48) and i + 4 < len(nums) and nums[i + 1] == 2:
                rgb = (nums[i + 2], nums[i + 3], nums[i + 4])
                if n == 38:
                    self.cur_fg = rgb
                else:
                    self.cur_bg = rgb
                i += 4
            i += 1

    # -- feeding -----------------------------------------------------------

    def feed(self, data):
        """Absorb a chunk of output, carrying any split escape over to the next."""
        text = self._pending + self._decoder.decode(data)
        self._pending = ""
        i = 0
        while i < len(text):
            ch = text[i]
            if ch == "\x1b":
                rest = text[i:]
                for pat in (CSI, OSC, DCS, ESC2, ESC1):
                    m = pat.match(rest)
                    if m:
                        if pat is CSI:
                            self._csi(m.group(1), m.group(2))
                        i += m.end()
                        break
                else:
                    # An escape that hasn't finished arriving: hold it back and
                    # resume when the rest of it lands.
                    self._pending = rest
                    return
                continue
            if ch == "\n":
                self.y += 1
                self.x = 0
            elif ch == "\r":
                self.x = 0
            elif ch == "\b":
                self.x = max(0, self.x - 1)
            elif ch == "\t":
                self.x = min(self.cols - 1, (self.x // 8 + 1) * 8)
            elif ch >= " ":
                self._put(ch)
            i += 1

    def _csi(self, params, cmd):
        if params.startswith(("?", ">", "<", "=", "!")):
            return  # private modes: nothing that changes the picture
        nums = [int(p) if p.isdigit() else 0 for p in params.split(";")] if params else []

        def arg(idx, default=1):
            return nums[idx] if idx < len(nums) and nums[idx] else default

        if cmd in "Hf":
            self.y = arg(0) - 1
            self.x = arg(1) - 1
        elif cmd == "A":
            self.y -= arg(0)
        elif cmd == "B":
            self.y += arg(0)
        elif cmd == "C":
            self.x += arg(0)
        elif cmd == "D":
            self.x -= arg(0)
        elif cmd == "G":
            self.x = arg(0) - 1
        elif cmd == "d":
            self.y = arg(0) - 1
        elif cmd == "J":
            self._erase_display(nums[0] if nums else 0)
        elif cmd == "K":
            self._erase_line(nums[0] if nums else 0)
        elif cmd == "m":
            self._sgr(nums)
        self.y = max(0, min(self.rows - 1, self.y))
        self.x = max(0, min(self.cols - 1, self.x))

    # -- reading back ------------------------------------------------------

    def line(self, r):
        return "".join(self.ch[r]).rstrip()

    def lines(self):
        return [self.line(r) for r in range(self.rows)]

    def text(self):
        return "\n".join(self.lines())

    def find(self, needle):
        """(row, col) of the first line containing `needle`, or None."""
        for r in range(self.rows):
            c = self.line(r).find(needle)
            if c >= 0:
                return (r, c)
        return None

    def find_cell(self, chars):
        """(row, col) of the first cell drawn with one of `chars`, or None."""
        for r in range(self.rows):
            for c in range(self.cols):
                if self.ch[r][c] in chars:
                    return (r, c)
        return None

    def __contains__(self, needle):
        return needle in self.text()

    def __str__(self):
        return "\n".join(ln for ln in self.lines() if ln.strip())


# ---------------------------------------------------------------------------
# input encodings
# ---------------------------------------------------------------------------

UP = b"\x1b[A"
DOWN = b"\x1b[B"
RIGHT = b"\x1b[C"
LEFT = b"\x1b[D"
ENTER = b"\r"
ESC = b"\x1b"
TAB = b"\t"

# xterm modifier encoding: 1 + (1 shift | 2 alt | 4 ctrl)
MOD_SHIFT, MOD_ALT, MOD_CTRL = 1, 2, 4

_ARROW_FINAL = {"up": "A", "down": "B", "right": "C", "left": "D"}


def arrow(name, mods=0):
    """An arrow key, optionally modified: arrow('right', MOD_ALT | MOD_SHIFT)."""
    final = _ARROW_FINAL[name]
    if not mods:
        return f"\x1b[{final}".encode()
    return f"\x1b[1;{1 + mods}{final}".encode()


def alt(ch):
    """Alt+<char>, as an ESC prefix - what viewpoint's default chords use."""
    return b"\x1b" + ch.encode()


def click(y, x, button=0):
    """SGR mouse press+release at 0-based cell (y, x)."""
    return (f"\x1b[<{button};{x + 1};{y + 1}M"
            f"\x1b[<{button};{x + 1};{y + 1}m").encode()


# ---------------------------------------------------------------------------
# the driven app
# ---------------------------------------------------------------------------

class Timeout(RuntimeError):
    """A predicate never came true before the deadline."""


class Session:
    """A running viewpoint on a private pty, with private config and daemon."""

    def __init__(self, rows=DEFAULT_ROWS, cols=DEFAULT_COLS, binary=BINARY,
                 config=None, env=None):
        self.rows = rows
        self.cols = cols
        self.binary = binary
        self.config = config  # text to seed viewpoint.conf with, or None
        self.extra_env = env or {}
        self.screen = Screen(rows, cols)
        self.pid = None
        self.fd = None
        self.tmp = None

    # -- lifecycle ---------------------------------------------------------

    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *exc):
        self.stop()
        return False

    def start(self):
        if not os.path.exists(self.binary):
            raise RuntimeError(
                f"{self.binary} not built - run `make` in {ROOT} first")

        self.tmp = tempfile.mkdtemp(prefix="vpdrive-")
        cfg_home = os.path.join(self.tmp, "config")
        run_dir = os.path.join(self.tmp, "run")
        os.makedirs(os.path.join(cfg_home, "viewpoint"))
        os.makedirs(run_dir)
        if self.config is not None:
            path = os.path.join(cfg_home, "viewpoint", "viewpoint.conf")
            with open(path, "w") as f:
                f.write(self.config)

        env = dict(os.environ)
        env.update(
            TERM="xterm-256color",
            COLORTERM="truecolor",
            SHELL="/bin/sh",
            XDG_CONFIG_HOME=cfg_home,
            XDG_RUNTIME_DIR=run_dir,  # a session daemon of our very own
        )
        env.update(self.extra_env)

        self.pid, self.fd = pty.fork()
        if self.pid == 0:
            try:
                os.environ.clear()
                os.environ.update(env)
                os.execv(self.binary, [self.binary])
            finally:
                os._exit(127)

        fcntl.ioctl(self.fd, termios.TIOCSWINSZ,
                    struct.pack("HHHH", self.rows, self.cols, 0, 0))
        # Let it finish interrogating the terminal and paint a first frame.
        self.wait_for(lambda s: s.find_cell("╭┌") is not None,
                      timeout=10, what="the desktop to appear")
        return self

    def stop(self):
        if self.pid:
            for sig in (signal.SIGTERM, signal.SIGKILL):
                try:
                    os.kill(self.pid, sig)
                except ProcessLookupError:
                    break
                if self._reap(1.0):
                    break
            self.pid = None
        if self.fd is not None:
            try:
                os.close(self.fd)
            except OSError:
                pass
            self.fd = None
        if self.tmp:
            self._kill_daemon(os.path.join(self.tmp, "run"))
            shutil.rmtree(self.tmp, ignore_errors=True)
            self.tmp = None

    def _reap(self, timeout):
        end = time.time() + timeout
        while time.time() < end:
            try:
                pid, _ = os.waitpid(self.pid, os.WNOHANG)
            except ChildProcessError:
                return True
            if pid:
                return True
            time.sleep(0.02)
        return False

    @staticmethod
    def _kill_daemon(run_dir):
        """Kill the `viewpoint --server` that setsid()'d out of our reach.

        It is identified by the private XDG_RUNTIME_DIR it inherited, so this
        can never touch a daemon belonging to the developer's own desktop.
        """
        needle = f"XDG_RUNTIME_DIR={run_dir}\0".encode()
        for entry in os.listdir("/proc"):
            if not entry.isdigit():
                continue
            try:
                with open(f"/proc/{entry}/environ", "rb") as f:
                    if needle not in f.read():
                        continue
                os.kill(int(entry), signal.SIGKILL)
            except (OSError, ValueError, PermissionError):
                continue

    # -- the terminal's side of the conversation ---------------------------

    @staticmethod
    def respond(chunk):
        """Answer the capability queries notcurses blocks on at startup."""
        out = b""
        if re.search(rb"\x1b\[\d*n", chunk):                     # DSR
            out += b"\x1b[1;1R"
        if re.search(rb"\x1b\[[>=]?\d*c", chunk):                # DA1/2/3
            out += b"\x1b[?62;22c"
        if re.search(rb"\x1b\[>\d*q", chunk):                    # XTVERSION
            out += b"\x1bP>|xterm(370)\x1b\\"
        for _ in re.finditer(rb"\x1bP\+q[0-9a-fA-F;]+\x1b\\", chunk):  # XTGETTCAP
            out += b"\x1bP0+r\x1b\\"
        for m in re.finditer(rb"\x1b\]4;(\d+);\?(?:\x07|\x1b\\)", chunk):
            n = m.group(1).decode()                              # palette query
            out += f"\x1b]4;{n};rgb:0000/0000/0000\x1b\\".encode()
        return out

    # -- i/o ---------------------------------------------------------------

    def send(self, data):
        os.write(self.fd, data)
        return self

    def click(self, y, x):
        return self.send(click(y, x))

    def pump(self, seconds):
        """Read for a fixed span, feeding the screen and answering queries."""
        end = time.time() + seconds
        while True:
            remaining = end - time.time()
            if remaining <= 0:
                return self.screen
            r, _, _ = select.select([self.fd], [], [], min(remaining, 0.05))
            if not r:
                continue
            try:
                chunk = os.read(self.fd, 65536)
            except OSError:
                return self.screen
            if not chunk:
                return self.screen
            self.screen.feed(chunk)
            reply = self.respond(chunk)
            if reply:
                os.write(self.fd, reply)

    def wait_for(self, predicate, timeout=5.0, what=None):
        """Pump until `predicate(screen)` holds. Raises Timeout if it never does."""
        end = time.time() + timeout
        while time.time() < end:
            if predicate(self.screen):
                return self.screen
            self.pump(0.05)
        if predicate(self.screen):
            return self.screen
        raise Timeout(f"timed out waiting for {what or predicate}\n"
                      f"--- last screen ---\n{self.screen}")

    def wait_text(self, needle, timeout=5.0):
        """Pump until `needle` appears on screen."""
        return self.wait_for(lambda s: needle in s.text(), timeout,
                             what=f"{needle!r} on screen")

    def settle(self, quiet=0.25, timeout=5.0):
        """Pump until the app stops drawing for `quiet` seconds."""
        end = time.time() + timeout
        last = None
        while time.time() < end:
            before = self.screen.text()
            self.pump(quiet)
            if self.screen.text() == before and last == before:
                break
            last = before
        return self.screen

    # -- convenience for the settings panel --------------------------------

    def open_settings(self):
        """Click the Settings desktop icon and wait for the tile grid."""
        self.click(2, 5)
        return self.wait_text("Keybindings")

    def read_config(self):
        """The config file as viewpoint has written it, or '' if there is none."""
        path = os.path.join(self.tmp, "config", "viewpoint", "viewpoint.conf")
        try:
            with open(path) as f:
                return f.read()
        except FileNotFoundError:
            return ""


# ---------------------------------------------------------------------------
# a tiny check helper, so the example scripts can report pass/fail uniformly
# ---------------------------------------------------------------------------

class Checks:
    def __init__(self):
        self.failed = 0

    def check(self, ok, label):
        print(f"  {'ok  ' if ok else 'FAIL'}: {label}")
        if not ok:
            self.failed += 1
        return ok

    def done(self):
        print()
        if self.failed:
            print(f"{self.failed} check(s) FAILED")
            return 1
        print("all checks passed")
        return 0


def main_guard(fn):
    """Run `fn`, turning a Timeout into a readable failure rather than a trace."""
    try:
        sys.exit(fn())
    except Timeout as e:
        print(f"TIMEOUT: {e}", file=sys.stderr)
        sys.exit(1)
