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

"""Watch the resize size indicator live and time its fade.

Terminals have no alpha, so the box fades by ramping its pen toward its own
background - which means the fade is visible to a test only if the harness
keeps per-cell colors. It does, so this samples the box's border color over its
whole life and reports the ramp.

Run with the fade both ways (the default) to see the switch in Settings ->
Animations actually change what reaches the screen.

Usage: tests/fade_probe.py [--on | --off]
"""

import sys
import time

sys.path.insert(0, __file__.rsplit("/", 1)[0])

from vpdrive import (MOD_ALT, MOD_SHIFT, Checks, Session,  # noqa: E402
                     alt, arrow, main_guard)

BOX = "╔╗╚╝═║"          # the indicator's border, drawn in no other chrome
RESIZE = arrow("right", MOD_ALT | MOD_SHIFT)

HOLD_MS = 600           # VP_SIZEOSD_HOLD_MS
FADE_MS = 500           # VP_SIZEOSD_FADE_MS


def sample(fade_on, quiet=False):
    """Resize a window, then track the indicator's border color until it goes."""
    conf = ("# manual configuration:\n"
            f"size_indicator_fade = {'true' if fade_on else 'false'}\n")
    trace = []
    with Session(config=conf) as vp:
        vp.send(alt("n"))                       # a window to resize
        vp.settle()

        t0 = time.time()
        vp.send(RESIZE)
        deadline = t0 + (HOLD_MS + FADE_MS) / 1000 + 0.8
        last = None
        while time.time() < deadline:
            vp.pump(0.02)
            ms = int((time.time() - t0) * 1000)
            at = vp.screen.find_cell(BOX)
            colour = vp.screen.fg[at[0]][at[1]] if at else None
            if colour != last:
                trace.append((ms, colour))
                last = colour

    label = "fade ON" if fade_on else "fade OFF"
    if not quiet:
        print(f"----- {label} " + "-" * max(0, 56 - len(label)))
        for ms, colour in trace:
            print(f"  t+{ms:4d}ms  " +
                  (f"border rgb{colour}" if colour else "(box gone)"))
        print()
    return [(ms, c) for ms, c in trace if c is not None]


def run():
    c = Checks()
    want = {"--on": [True], "--off": [False]}.get(
        next((a for a in sys.argv[1:] if a.startswith("--")), None),
        [True, False])

    shades = {}
    for fade_on in want:
        seen = sample(fade_on)
        shades[fade_on] = {colour for _, colour in seen}
        c.check(bool(seen), f"the indicator appeared (fade {'on' if fade_on else 'off'})")

    if True in shades:
        c.check(len(shades[True]) > 3,
                f"fade on: the border ramps through several shades "
                f"({len(shades[True])} seen)")
    if False in shades:
        c.check(len(shades[False]) == 1,
                f"fade off: the border never changes shade "
                f"({len(shades[False])} seen)")
    if len(want) == 2:
        c.check(len(shades[True]) > len(shades[False]),
                "the switch is what makes the difference")

    return c.done()


if __name__ == "__main__":
    main_guard(run)
