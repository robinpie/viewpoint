// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie <robin@dreamstation.systems>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 3.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* fade_test.c - the size indicator's clock.
 *
 * sizeosd.c decides everything from one number - how long ago the box last
 * (re)appeared - so its whole timeline can be checked by winding that number
 * back and asking what it thinks. sizeosd.c is #included rather than linked so
 * the file-static helpers behind that decision are reachable.
 */
#define _GNU_SOURCE
#include "../../sizeosd.c"

#include "check.h"

#include <string.h>

static WM wm;

/* Pose the WM as though the box appeared `age` milliseconds ago. */
static void at_age(uint64_t age, bool fade)
{
	memset(&wm, 0, sizeof(wm));
	wm.config.sizeosd_fade = fade;
	wm.sizeosd_active = true;
	wm.sizeosd_at_ms = mono_ms() - age;
}

int main(void)
{
	const uint64_t H = VP_SIZEOSD_HOLD_MS, F = VP_SIZEOSD_FADE_MS;

	printf("fade on (the default):\n");
	at_age(0, true);
	CHECK(fade_frac(&wm) == 0, "age 0     -> full strength");
	at_age(H - 1, true);
	CHECK(fade_frac(&wm) == 0, "age H-1   -> still holding at full strength");
	at_age(H + F / 2, true);
	CHECK(fade_frac(&wm) > 100 && fade_frac(&wm) < 156,
	      "age H+F/2 -> about halfway sunk (%u/%u)", fade_frac(&wm),
	      FADE_RAMP);
	at_age(H + F, true);
	CHECK(fade_frac(&wm) == FADE_RAMP, "age H+F   -> fully sunk into the bg");

	at_age(H - 1, true);
	CHECK(sizeosd_timeout_ms(&wm) == 1,
	      "age H-1   -> loop sleeps out the rest of the hold");
	at_age(H + 1, true);
	CHECK(sizeosd_timeout_ms(&wm) == VP_SIZEOSD_STEP_MS,
	      "age H+1   -> loop wakes every %dms to repaint", VP_SIZEOSD_STEP_MS);
	at_age(H + F - 1, true);
	CHECK(sizeosd_timeout_ms(&wm) == VP_SIZEOSD_STEP_MS,
	      "age H+F-1 -> still stepping");
	at_age(H + F, true);
	CHECK(sizeosd_timeout_ms(&wm) == 0, "age H+F   -> expired");

	printf("fade off:\n");
	at_age(0, false);
	CHECK(fade_frac(&wm) == 0, "age 0     -> full strength");
	at_age(H - 1, false);
	CHECK(fade_frac(&wm) == 0,
	      "age H-1   -> the hold is not cut short, only the fade");
	at_age(H - 1, false);
	CHECK(sizeosd_timeout_ms(&wm) == 1,
	      "age H-1   -> loop sleeps out the rest of the hold");
	at_age(H, false);
	CHECK(sizeosd_timeout_ms(&wm) == 0, "age H     -> expired the moment it ends");
	at_age(H + 1, false);
	CHECK(sizeosd_timeout_ms(&wm) == 0, "age H+1   -> never a fade step");
	at_age(H + F / 2, false);
	CHECK(fade_frac(&wm) == FADE_RAMP,
	      "age H+F/2 -> a zero-length ramp divides by nothing");

	/* With no windows on the list there is nothing to center the box on, so
	 * a live tick always hides it; only the expiry branch is exercised here. */
	printf("tick:\n");
	at_age(H, false);
	sizeosd_tick(&wm);
	CHECK(!wm.sizeosd_active, "fade off at age H   -> deactivated");
	at_age(H + F, true);
	sizeosd_tick(&wm);
	CHECK(!wm.sizeosd_active, "fade on at age H+F  -> deactivated");

	printf("idle:\n");
	memset(&wm, 0, sizeof(wm));
	CHECK(sizeosd_timeout_ms(&wm) == -1,
	      "inactive  -> the loop may sleep indefinitely");

	return vp_test_report();
}
