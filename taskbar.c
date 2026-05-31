// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie
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

/* taskbar.c — a full-width bar pinned to the bottom row, above all windows.
 *
 * Shows one slot per window (id + short title, marked if minimized), highlights
 * the focused window, and shows the global mode indicator + clickable toggle.
 * Built as a custom plane (not ncmenu) for full layout control.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>

#define SLOT_W 18                 /* columns per window slot */
#define MODE_W 13                 /* " PASSTHROUGH " region width */

/* Computed layout, shared between draw and click so they stay in lockstep. */
typedef struct {
    int win_idx;  /* index into wm->wins */
    int xstart;
    int xend;     /* exclusive */
} Slot;

static Slot g_slots[256];
static int  g_nslots;
static int  g_mode_x0; /* start column of the mode toggle region */

static void compute_layout(WM *wm)
{
    g_nslots = 0;
    g_mode_x0 = (int)wm->scr_cols - MODE_W;

    int x = 0;
    for (int i = 0; i < wm->nwins && g_nslots < (int)(sizeof(g_slots) / sizeof(g_slots[0])); i++) {
        if (x + SLOT_W > g_mode_x0) {
            break; /* no more room before the mode region */
        }
        g_slots[g_nslots].win_idx = i;
        g_slots[g_nslots].xstart = x;
        g_slots[g_nslots].xend = x + SLOT_W;
        g_nslots++;
        x += SLOT_W;
    }
}

void taskbar_create(WM *wm)
{
    ncplane_options o = {0};
    o.y = (int)wm->scr_rows - 1;
    o.x = 0;
    o.rows = 1;
    o.cols = wm->scr_cols;
    wm->taskbar = ncplane_create(wm->std, &o);
    if (wm->taskbar) {
        ncplane_move_top(wm->taskbar);
        wm->taskbar_dirty = true;
    }
}

void taskbar_reflow(WM *wm)
{
    if (!wm->taskbar) {
        return;
    }
    ncplane_resize_simple(wm->taskbar, 1, wm->scr_cols);
    ncplane_move_yx(wm->taskbar, (int)wm->scr_rows - 1, 0);
    ncplane_move_top(wm->taskbar);
    wm->taskbar_dirty = true;
}

void taskbar_draw(WM *wm)
{
    struct ncplane *t = wm->taskbar;
    if (!t) {
        return;
    }
    compute_layout(wm);

    /* Background fill. */
    ncplane_set_fg_rgb8(t, 0xd0, 0xd0, 0xd0);
    ncplane_set_bg_rgb8(t, 0x18, 0x18, 0x20);
    ncplane_set_styles(t, NCSTYLE_NONE);
    ncplane_erase(t);
    for (unsigned c = 0; c < wm->scr_cols; c++) {
        ncplane_putchar_yx(t, 0, (int)c, ' ');
    }

    for (int s = 0; s < g_nslots; s++) {
        Window *win = wm->wins[g_slots[s].win_idx];
        bool focused = (wm_focused(wm) == win);

        if (focused) {
            ncplane_set_fg_rgb8(t, 0xff, 0xff, 0xff);
            ncplane_set_bg_rgb8(t, 0x20, 0x40, 0x80);
        } else if (win->minimized) {
            ncplane_set_fg_rgb8(t, 0x80, 0x80, 0x80);
            ncplane_set_bg_rgb8(t, 0x18, 0x18, 0x20);
        } else {
            ncplane_set_fg_rgb8(t, 0xd0, 0xd0, 0xd0);
            ncplane_set_bg_rgb8(t, 0x28, 0x28, 0x30);
        }

        char buf[SLOT_W + 1];
        /* "N:title" — wrap minimized titles in brackets. The slot field width
         * below truncates; this buffer just holds the untruncated string. */
        char title[VP_TITLE_MAX + 16];
        snprintf(title, sizeof(title), "%d:%s", win->id, win->title);
        if (win->minimized) {
            snprintf(buf, sizeof(buf), " [%-*.*s] ", SLOT_W - 4, SLOT_W - 4, title);
        } else {
            snprintf(buf, sizeof(buf), " %-*.*s ", SLOT_W - 2, SLOT_W - 2, title);
        }
        ncplane_putstr_yx(t, 0, g_slots[s].xstart, buf);
    }

    /* Mode indicator + clickable toggle at the far right. */
    if (g_mode_x0 >= 0) {
        ncplane_set_fg_rgb8(t, 0x10, 0x10, 0x10);
        if (wm->mode == MODE_PASSTHROUGH) {
            ncplane_set_bg_rgb8(t, 0xe0, 0xa0, 0x30); /* amber: passthrough */
            ncplane_putstr_yx(t, 0, g_mode_x0, " PASSTHROUGH ");
        } else {
            ncplane_set_bg_rgb8(t, 0x40, 0xc0, 0x60); /* green: interpret */
            ncplane_putstr_yx(t, 0, g_mode_x0, "  INTERPRET  ");
        }
    }

    wm->taskbar_dirty = false;
}

bool taskbar_click(WM *wm, int y, int x)
{
    if (!wm->taskbar || y != (int)wm->scr_rows - 1) {
        return false;
    }
    compute_layout(wm);

    /* Mode toggle region. */
    if (x >= g_mode_x0) {
        wm->mode = (wm->mode == MODE_INTERPRET) ? MODE_PASSTHROUGH : MODE_INTERPRET;
        wm->taskbar_dirty = true;
        for (int i = 0; i < wm->nwins; i++) {
            wm->wins[i]->frame_dirty = true; /* per-window indicator changes */
        }
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
