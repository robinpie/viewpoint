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

"""Walk the settings panel, printing each screen and checking what it shows.

Doubles as the worked example for vpdrive: opening the panel, navigating the
tile grid, flipping a setting and confirming it reached the config file are all
the things a UI change usually needs proving.

Usage: tests/ui_shots.py [--quiet]
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from vpdrive import (DOWN, ENTER, ESC, RIGHT, UP, Checks,  # noqa: E402
                     Session, main_guard)

QUIET = "--quiet" in sys.argv


def shot(session, label):
    session.settle()
    if not QUIET:
        print(f"----- {label} " + "-" * max(0, 56 - len(label)))
        print(session.screen)
        print()
    return session.screen


def run():
    c = Checks()
    with Session() as vp:
        shot(vp, "desktop")

        vp.open_settings()
        scr = shot(vp, "settings grid")
        for tile in ("Keybindings", "Terminal", "Appearance", "Desktop Icons",
                     "Animations"):
            c.check(tile in scr, f"grid shows the {tile} tile")

        # The Animations tile is the fifth entry: second row, second column.
        vp.send(DOWN + RIGHT)
        vp.settle()
        vp.send(ENTER)
        scr = vp.wait_text("Size indicator fade-out")
        shot(vp, "Animations view")
        c.check("Size indicator fade-out" in scr, "the fade switch is listed")
        c.check(scr.find("on") is not None, "it reads 'on' by default")

        vp.send(RIGHT)
        scr = vp.wait_text("= off")
        shot(vp, "after toggling the switch")
        c.check("= off" in scr, "toggling reports the new value")

        vp.send(ESC)   # back to the grid
        vp.settle()

        # The Terminal tile (top row, second column - one up from Animations).
        vp.send(UP)
        vp.settle()
        vp.send(ENTER)
        scr = vp.wait_text("Copy also runs")
        shot(vp, "Terminal view")
        c.check("Copy also runs" in scr, "the clipboard command row is listed")
        c.check("(none)" in scr, "with no command set to begin with")

        vp.send(DOWN + DOWN + ENTER)     # onto the row, then open it for typing
        vp.wait_text("Type a shell command")
        vp.send(b"wl-copy" + ENTER)
        scr = vp.wait_text("Clipboard command set")
        shot(vp, "after typing a clipboard command")
        c.check("wl-copy" in scr, "the row shows what was typed")

        vp.send(ESC)   # back to the grid
        vp.settle()
        vp.send(ESC)   # close the panel, which saves
        vp.settle()

        conf = vp.read_config()
        c.check("size_indicator_fade = false" in conf,
                "the choice was written to viewpoint.conf")
        c.check("clipboard_command = wl-copy" in conf,
                "and so was the clipboard command")
        if not QUIET:
            print("----- viewpoint.conf " + "-" * 42)
            print(conf)

    return c.done()


if __name__ == "__main__":
    main_guard(run)
