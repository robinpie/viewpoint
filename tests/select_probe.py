#!/usr/bin/env python3
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

"""Select text with the mouse and paste it back, in the real app.

The unit checks (tests/unit/sel_test.c) already cover what a selection covers
and what comes out of it. What they cannot see is the two ends of the feature:
that a mouse drag arriving as escape sequences becomes a highlight on screen,
and that the text then makes the round trip out to the clipboard and back into
a shell.

Usage: tests/select_probe.py [--quiet]
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from vpdrive import Checks, Session, alt, main_guard  # noqa: E402

QUIET = "--quiet" in sys.argv

WORD = "SELECTME"


def press(y, x):
    return f"\x1b[<0;{x + 1};{y + 1}M".encode()


def motion(y, x):
    """Motion with button 1 held (SGR adds 32 to the button for drags)."""
    return f"\x1b[<32;{x + 1};{y + 1}M".encode()


def release(y, x):
    return f"\x1b[<0;{x + 1};{y + 1}m".encode()


def drag(vp, y, x0, x1):
    vp.send(press(y, x0))
    vp.pump(0.05)
    for x in range(x0 + 1, x1 + 1):
        vp.send(motion(y, x))
        vp.pump(0.02)
    vp.send(release(y, x1))
    return vp.settle()


def show(vp, label):
    vp.settle()
    if not QUIET:
        print(f"----- {label} " + "-" * max(0, 56 - len(label)))
        print(vp.screen)
        print()
    return vp.screen


def run():
    c = Checks()
    with Session() as vp:
        vp.settle()
        # Put a word on the focused window's screen, on a line of its own.
        vp.send(f"printf '%s\\n' {WORD}\r".encode())

        # Wait for the *output* line, not just the command echo: both carry the
        # word, and the echo shows up first.
        def rows_with_word(s):
            return [(r, s.line(r).find(WORD)) for r in range(vp.rows)
                    if s.line(r).find(WORD) >= 0]

        vp.wait_for(lambda s: len(rows_with_word(s)) >= 2,
                    what=f"{WORD} echoed and printed")
        scr = show(vp, f"{WORD} printed")

        hits = rows_with_word(scr)
        c.check(len(hits) >= 2, f"the word is on screen ({len(hits)} rows)")
        row, col = hits[-1]

        base = scr.bg[row][col]
        drag(vp, row, col, col + len(WORD) - 1)
        scr = show(vp, "dragged across the word")
        inverted = sum(1 for i in range(len(WORD))
                       if scr.bg[row][col + i] != base)
        c.check(inverted == len(WORD),
                f"every cell of the drag is highlighted ({inverted}/{len(WORD)})")
        after = col + len(WORD)
        c.check(after >= vp.cols or scr.bg[row][after] == base,
                "the cell just past it is not")

        # A double-click takes the whole word without a drag.
        vp.send(press(row, col + 2) + release(row, col + 2))
        vp.pump(0.05)
        vp.send(press(row, col + 2) + release(row, col + 2))
        scr = show(vp, "double-clicked the word")
        whole = sum(1 for i in range(len(WORD))
                    if scr.bg[row][col + i] != base)
        c.check(whole == len(WORD),
                f"a double-click selects the whole word ({whole}/{len(WORD)})")

        # Releasing the button selects but does not copy: the clipboard is
        # only spent when the copy chord asks for it.
        before = vp.screen.text().count(WORD)
        vp.send(alt("v"))
        vp.pump(0.4)
        c.check(vp.screen.text().count(WORD) == before,
                "pasting before any copy puts nothing on screen")

        vp.send(alt("c"))
        vp.pump(0.2)
        vp.send(alt("v"))
        vp.wait_for(lambda s: s.text().count(WORD) > before,
                    what="the pasted word")
        show(vp, "pasted back at the prompt")
        c.check(True, "alt+c then alt+v round-trips the selection")

        # A press anywhere on the text drops the old highlight.
        vp.send(press(row, col + 2) + release(row, col + 2))
        scr = show(vp, "after a plain click")
        c.check(all(scr.bg[row][col + i] == base for i in range(len(WORD))),
                "a plain click clears the highlight, leaving no stray cell")

    return c.done()


main_guard(run)
