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

/* taskbar.c - a full-width bar pinned to the bottom row, above all windows.
 *
 * Shows one slot per window (id + short title, marked if minimized), highlights
 * the focused window, and shows the global mode indicator + clickable toggle.
 * Built as a custom plane (not ncmenu) for full layout control.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>

#define SLOT_W 18 /* columns per window slot */
#define MODE_W 13 /* " PASSTHROUGH " region width */
#define ARROW_W 3 /* " ◄ " / " ► " clickable scroll arrows */

/* Computed layout, shared between draw and click so they stay in lockstep. */
typedef struct {
	int win_idx; /* index into wm->wins */
	int xstart;
	int xend; /* exclusive */
} Slot;

static Slot g_slots[256];
static int g_nslots;
static int g_mode_x0; /* start column of the mode toggle region */
static int g_arrow_l_x0; /* start column of the ◄ arrow, or -1 if not shown */
static int g_arrow_r_x0; /* start column of the ► arrow, or -1 if not shown */
static int g_maxscroll; /* largest valid wm->taskbar_scroll for this layout */
static int g_maxfit; /* number of slots that fit (the visible page size) */

static void compute_layout(WM *wm)
{
	g_nslots = 0;
	g_mode_x0 = (int)wm->scr_cols - MODE_W;
	g_arrow_l_x0 = g_arrow_r_x0 = -1;
	g_maxscroll = 0;

	int maxfit = g_mode_x0 / SLOT_W;
	if (maxfit < 0) {
		maxfit = 0;
	}

	/* Overflow: more windows than fit. Reserve a ◄ / ► arrow region at each end
     * (only if the bar is wide enough for the arrows plus at least one slot) and
     * shrink the slot strip accordingly. */
	int slot_x0 = 0;
	int slot_x1 = g_mode_x0; /* exclusive */
	if (wm->nwins > maxfit && g_mode_x0 >= ARROW_W * 2 + SLOT_W) {
		g_arrow_l_x0 = 0;
		g_arrow_r_x0 = g_mode_x0 - ARROW_W;
		slot_x0 = ARROW_W;
		slot_x1 = g_arrow_r_x0;
		maxfit = (slot_x1 - slot_x0) / SLOT_W;
	}

	g_maxfit = maxfit;
	g_maxscroll = wm->nwins - maxfit;
	if (g_maxscroll < 0) {
		g_maxscroll = 0;
	}
	if (wm->taskbar_scroll > g_maxscroll) {
		wm->taskbar_scroll = g_maxscroll;
	}
	if (wm->taskbar_scroll < 0) {
		wm->taskbar_scroll = 0;
	}

	int x = slot_x0;
	for (int i = wm->taskbar_scroll;
	     i < wm->nwins &&
	     g_nslots < (int)(sizeof(g_slots) / sizeof(g_slots[0]));
	     i++) {
		if (x + SLOT_W > slot_x1) {
			break; /* no more room in the slot strip */
		}
		g_slots[g_nslots].win_idx = i;
		g_slots[g_nslots].xstart = x;
		g_slots[g_nslots].xend = x + SLOT_W;
		g_nslots++;
		x += SLOT_W;
	}
}

static void taskbar_paint(struct ncplane *t, bool full, void *user);

void taskbar_create(WM *wm)
{
	/* VP_BAND_OVERLAY is what keeps the bar above every window. It is stated
	 * once, here, instead of being re-asserted with a move_top on every
	 * focus change and every render pass. */
	vp_rect r = { (int)wm->scr_rows - 1, 0, 1, (int)wm->scr_cols };
	wm->taskbar =
		comp_layer_new(wm->comp, VP_BAND_OVERLAY, r, taskbar_paint, wm);
}

void taskbar_reflow(WM *wm)
{
	if (!wm->taskbar) {
		return;
	}
	comp_layer_resize(wm->taskbar, 1, (int)wm->scr_cols);
	comp_layer_move(wm->taskbar, (int)wm->scr_rows - 1, 0);
	taskbar_damage(wm);
}

void taskbar_damage(WM *wm)
{
	comp_layer_damage(wm->taskbar);
}

static void taskbar_paint(struct ncplane *t, bool full, void *user)
{
	(void)full;
	WM *wm = user;
	compute_layout(wm);

	/* Base cell = the bar's background, so the erase below fills the whole
	 * row with it in one shot. Re-applied every paint so a theme change
	 * needs nothing but a damage. */
	uint64_t ch = 0;
	vp_rgb bfg = wm->theme.bar_fg, bbg = wm->theme.bar_bg;
	ncchannels_set_fg_rgb8(&ch, (bfg >> 16) & 0xff, (bfg >> 8) & 0xff,
			       bfg & 0xff);
	ncchannels_set_bg_rgb8(&ch, (bbg >> 16) & 0xff, (bbg >> 8) & 0xff,
			       bbg & 0xff);
	ncplane_set_base(t, " ", 0, ch);

	ncplane_set_styles(t, NCSTYLE_NONE);
	ncplane_erase(t);

	for (int s = 0; s < g_nslots; s++) {
		Window *win = wm->wins[g_slots[s].win_idx];
		bool focused = (wm_focused(wm) == win);

		if (focused) {
			vp_setfg(t, wm->theme.bar_focus_fg);
			vp_setbg(t, wm->theme.bar_focus_bg);
		} else if (win->minimized) {
			vp_setfg(t, wm->theme.bar_min_fg);
			vp_setbg(t, wm->theme.bar_min_bg);
		} else {
			vp_setfg(t, wm->theme.bar_slot_fg);
			vp_setbg(t, wm->theme.bar_slot_bg);
		}

		char buf[SLOT_W + 1];
		/* "N:title" - wrap minimized titles in brackets. The slot field width
         * below truncates; this buffer just holds the untruncated string. */
		char title[VP_TITLE_MAX + 16];
		snprintf(title, sizeof(title), "%d:%s", win->id, win->title);
		if (win->minimized) {
			snprintf(buf, sizeof(buf), " [%-*.*s] ", SLOT_W - 4,
				 SLOT_W - 4, title);
		} else {
			snprintf(buf, sizeof(buf), " %-*.*s ", SLOT_W - 2,
				 SLOT_W - 2, title);
		}
		ncplane_putstr_yx(t, 0, g_slots[s].xstart, buf);
	}

	/* ◄ / ► scroll arrows (only present when the slots overflow). Each is dimmed
     * when there's nothing more to reveal in that direction. */
	if (g_arrow_l_x0 >= 0) {
		bool live = wm->taskbar_scroll > 0;
		unsigned char g = live ? 0xff : 0x55;
		ncplane_set_fg_rgb8(t, g, g, g);
		vp_setbg(t, wm->theme.bar_arrow_bg);
		ncplane_putstr_yx(t, 0, g_arrow_l_x0, " ◄ ");
	}
	if (g_arrow_r_x0 >= 0) {
		bool live = wm->taskbar_scroll < g_maxscroll;
		unsigned char g = live ? 0xff : 0x55;
		ncplane_set_fg_rgb8(t, g, g, g);
		vp_setbg(t, wm->theme.bar_arrow_bg);
		ncplane_putstr_yx(t, 0, g_arrow_r_x0, " ► ");
	}

	if (g_mode_x0 >= 0) {
		vp_setfg(t, wm->theme.bar_mode_fg);
		if (wm->mode == MODE_PASSTHROUGH) {
			vp_setbg(t, wm->theme.bar_pass_bg);
			ncplane_putstr_yx(t, 0, g_mode_x0, " PASSTHROUGH ");
		} else {
			vp_setbg(t, wm->theme.bar_interp_bg);
			ncplane_putstr_yx(t, 0, g_mode_x0, "  INTERPRET  ");
		}
	}
}

bool taskbar_click(WM *wm, int y, int x)
{
	if (!wm->taskbar || y != (int)wm->scr_rows - 1) {
		return false;
	}
	compute_layout(wm);

	if (x >= g_mode_x0) {
		wm->mode = (wm->mode == MODE_INTERPRET) ? MODE_PASSTHROUGH :
							  MODE_INTERPRET;
		taskbar_damage(wm);
		for (int i = 0; i < wm->nwins; i++) {
			window_damage_frame(
				wm->wins[i]); /* per-window indicator changes */
		}
		return true;
	}

	if (g_arrow_l_x0 >= 0 && x >= g_arrow_l_x0 &&
	    x < g_arrow_l_x0 + ARROW_W) {
		taskbar_scroll_by(wm, -1);
		return true;
	}
	if (g_arrow_r_x0 >= 0 && x >= g_arrow_r_x0 &&
	    x < g_arrow_r_x0 + ARROW_W) {
		taskbar_scroll_by(wm, +1);
		return true;
	}

	for (int s = 0; s < g_nslots; s++) {
		if (x >= g_slots[s].xstart && x < g_slots[s].xend) {
			Window *win = wm->wins[g_slots[s].win_idx];
			if (win->minimized) {
				wm_restore(wm, win);
			} else {
				wm_focus_window(wm, win);
			}
			return true;
		}
	}
	return true; /* a click anywhere on the bar is consumed */
}

void taskbar_scroll_by(WM *wm, int delta)
{
	if (!wm->taskbar) {
		return;
	}
	compute_layout(wm); /* refresh g_maxscroll for the current geometry */
	int s = wm->taskbar_scroll + delta;
	if (s < 0) {
		s = 0;
	}
	if (s > g_maxscroll) {
		s = g_maxscroll;
	}
	if (s != wm->taskbar_scroll) {
		wm->taskbar_scroll = s;
		taskbar_damage(wm);
	}
}

void taskbar_reveal(WM *wm, int win_idx)
{
	if (!wm->taskbar || win_idx < 0 || win_idx >= wm->nwins) {
		return;
	}
	compute_layout(
		wm); /* refresh g_maxfit / g_maxscroll for the current geometry */
	if (g_maxfit < 1 || wm->nwins <= g_maxfit) {
		return; /* everything fits - nothing to scroll into view */
	}
	int s = wm->taskbar_scroll;
	if (win_idx < s) {
		s = win_idx; /* off the left edge: make it leftmost */
	} else if (win_idx >= s + g_maxfit) {
		s = win_idx - g_maxfit +
		    1; /* off the right edge: make it rightmost */
	}
	if (s < 0) {
		s = 0;
	}
	if (s > g_maxscroll) {
		s = g_maxscroll;
	}
	if (s != wm->taskbar_scroll) {
		wm->taskbar_scroll = s;
		taskbar_damage(wm);
	}
}
