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

/* sizeosd.c - the resize size indicator: a small centered box reporting the
 * focused window's grid size while it is being resized.
 *
 * It is the one piece of chrome driven by the clock rather than by an event,
 * so it owns both halves of that: sizeosd_timeout_ms tells the event loop how
 * long it may sleep, and sizeosd_tick decides what the elapsed time meant.
 * The box holds at full strength, then fades - terminals give us no alpha, so
 * "fading" is the pen ramping toward the box's own background - and is hidden
 * the moment the ramp completes, so the emptied box is never left standing.
 *
 * It centers on the window it describes, and sizeosd_tick re-places it every
 * pass, so it stays put over that window as the window is dragged around or
 * resized from a top/left edge (which moves the frame as well as sizing it).
 * Placement is idempotent - the compositor's move/resize/raise all no-op when
 * nothing changed - so re-deciding it every frame costs nothing.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

/* Fixed-point denominator for the fade ramp; 0 = untouched, RAMP = fully
 * sunk into the background. */
#define FADE_RAMP 256u

/* Border + one column of padding on each side of the text. */
#define BOX_PAD 4
#define BOX_H 3

static uint64_t mono_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000ull +
	       (uint64_t)ts.tv_nsec / 1000000ull;
}

/* By id, not by pointer: the window can be closed while the box is still
 * fading over where it used to be. */
static Window *osd_window(WM *wm)
{
	for (int i = 0; i < wm->nwins; i++) {
		if (wm->wins[i]->id == wm->sizeosd_win) {
			return wm->wins[i];
		}
	}
	return NULL;
}

static uint64_t osd_age_ms(const WM *wm)
{
	uint64_t now = mono_ms();
	return now > wm->sizeosd_at_ms ? now - wm->sizeosd_at_ms : 0;
}

/* How far into the fade we are. Derived from the clock rather than counted in
 * ticks, so a repaint forced on us mid-fade (a bitmap torn down underneath)
 * lands on the shade the elapsed time calls for, not the next one in sequence. */
static unsigned fade_frac(const WM *wm)
{
	uint64_t age = osd_age_ms(wm);
	if (age <= VP_SIZEOSD_HOLD_MS) {
		return 0;
	}
	age -= VP_SIZEOSD_HOLD_MS;
	if (age >= VP_SIZEOSD_FADE_MS) {
		return FADE_RAMP;
	}
	return (unsigned)(age * FADE_RAMP / VP_SIZEOSD_FADE_MS);
}

/* `a` blended toward `b` by t/FADE_RAMP, per channel. */
static vp_rgb rgb_mix(vp_rgb a, vp_rgb b, unsigned t)
{
	vp_rgb out = 0;
	for (int sh = 16; sh >= 0; sh -= 8) {
		unsigned ca = (a >> sh) & 0xff;
		unsigned cb = (b >> sh) & 0xff;
		unsigned c = (ca * (FADE_RAMP - t) + cb * t) / FADE_RAMP;
		out |= (vp_rgb)c << sh;
	}
	return out;
}

static void sizeosd_hide(WM *wm)
{
	wm->sizeosd_active = false;
	if (wm->sizeosd) {
		comp_layer_show(wm->sizeosd, false);
	}
}

static void sizeosd_paint(struct ncplane *p, bool full, void *user)
{
	(void)full; /* three rows of chrome; always redrawn whole */
	WM *wm = user;
	vp_rect r = comp_layer_rect(wm->sizeosd);
	int h = r.h, w = r.w;

	unsigned t = fade_frac(wm);
	vp_rgb bg = wm->theme.panel_bg;
	vp_rgb border = rgb_mix(wm->theme.panel_accent, bg, t);
	vp_rgb text = rgb_mix(wm->theme.panel_fg, bg, t);

	/* Centered in the interior, space-padded to its full width so the row is
	 * written in one go and no cell is left showing the plane's base. */
	char mid[sizeof(wm->sizeosd_text) + BOX_PAD];
	int inner = w - 2;
	int len = (int)strlen(wm->sizeosd_text);
	int lpad = (inner - len) / 2;
	snprintf(mid, sizeof(mid), "%*s%s%*s", lpad, "", wm->sizeosd_text,
		 inner - len - lpad, "");

	ncplane_erase(p);
	vp_setbg(p, bg);
	ncplane_set_styles(p, NCSTYLE_NONE);

	vp_setfg(p, border);
	ncplane_putegc_yx(p, 0, 0, "╔", NULL);
	ncplane_putegc_yx(p, 0, w - 1, "╗", NULL);
	ncplane_putegc_yx(p, h - 1, 0, "╚", NULL);
	ncplane_putegc_yx(p, h - 1, w - 1, "╝", NULL);
	for (int c = 1; c < w - 1; c++) {
		ncplane_putegc_yx(p, 0, c, "═", NULL);
		ncplane_putegc_yx(p, h - 1, c, "═", NULL);
	}
	for (int row = 1; row < h - 1; row++) {
		ncplane_putegc_yx(p, row, 0, "║", NULL);
		ncplane_putegc_yx(p, row, w - 1, "║", NULL);
	}

	vp_setfg(p, text);
	ncplane_putstr_yx(p, 1, 1, mid);
}

/* Size the box to the current text and center it on its window. False if
 * there is nothing to center it on, or no room on screen to draw it. */
static bool sizeosd_place(WM *wm)
{
	Window *win = osd_window(wm);
	if (!win || win->minimized) {
		return false;
	}
	int w = (int)strlen(wm->sizeosd_text) + BOX_PAD + 2;
	int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
	if (w > (int)wm->scr_cols || BOX_H > avail_h) {
		return false;
	}

	int x = win->x + (win->w - w) / 2;
	int y = win->y + (win->h - BOX_H) / 2;
	/* A window can be narrower than the box, or sit half off the edge, so
	 * the box is centered on it but never at the cost of being cut off:
	 * the size is the whole point of showing it. */
	if (x + w > (int)wm->scr_cols) {
		x = (int)wm->scr_cols - w;
	}
	if (y + BOX_H > avail_h) {
		y = avail_h - BOX_H;
	}
	if (x < 0) {
		x = 0;
	}
	if (y < 0) {
		y = 0;
	}

	if (!wm->sizeosd) {
		vp_rect r = { y, x, BOX_H, w };
		wm->sizeosd = comp_layer_new(wm->comp, VP_BAND_OVERLAY, r,
					     sizeosd_paint, wm);
		if (!wm->sizeosd) {
			return false;
		}
		comp_layer_damage(wm->sizeosd);
	} else {
		/* Each of these no-ops when it would change nothing, which is
		 * what makes calling this every frame free. */
		comp_layer_resize(wm->sizeosd, BOX_H, w);
		comp_layer_move(wm->sizeosd, y, x);
		comp_layer_show(wm->sizeosd, true);
	}
	comp_layer_raise(wm->sizeosd); /* above the taskbar, within the band */
	return true;
}

void sizeosd_show(WM *wm, Window *win)
{
	/* Only for the window the user is working on. A terminal resize re-lays
	 * out every window at once, and one box per window would just leave
	 * whichever ran last standing in the middle of the screen. */
	if (!wm->comp || !win || win->minimized || wm_focused(wm) != win) {
		return;
	}
	/* The grid the child program sees, which is what "80x24" means for a
	 * terminal - not the frame, which is two cells larger each way. */
	snprintf(wm->sizeosd_text, sizeof(wm->sizeosd_text), "%dx%d", win->cols,
		 win->rows);
	wm->sizeosd_at_ms = mono_ms();
	wm->sizeosd_win = win->id;
	wm->sizeosd_active = true;
	if (!sizeosd_place(wm)) {
		sizeosd_hide(wm);
		return;
	}
	comp_layer_damage(wm->sizeosd); /* new size, and back to full strength */
}

void sizeosd_tick(WM *wm)
{
	if (!wm->sizeosd_active) {
		return;
	}
	uint64_t age = osd_age_ms(wm);
	if (age >= (uint64_t)VP_SIZEOSD_HOLD_MS + VP_SIZEOSD_FADE_MS) {
		sizeosd_hide(wm);
		return;
	}
	/* Follow the window, and go with it if it was closed or minimized out
	 * from under us. */
	if (!sizeosd_place(wm)) {
		sizeosd_hide(wm);
		return;
	}
	if (age >= VP_SIZEOSD_HOLD_MS) {
		comp_layer_damage(wm->sizeosd); /* next shade of the fade */
	}
}

int sizeosd_timeout_ms(const WM *wm)
{
	if (!wm->sizeosd_active) {
		return -1; /* nothing pending: the loop may sleep indefinitely */
	}
	uint64_t age = osd_age_ms(wm);
	if (age < VP_SIZEOSD_HOLD_MS) {
		/* Nothing to do until the hold runs out; sleep through it. */
		return (int)(VP_SIZEOSD_HOLD_MS - age);
	}
	if (age < (uint64_t)VP_SIZEOSD_HOLD_MS + VP_SIZEOSD_FADE_MS) {
		return VP_SIZEOSD_STEP_MS;
	}
	return 0;
}
