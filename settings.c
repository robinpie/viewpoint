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

/* settings.c — the in-app keybinding editor.
 *
 * A small launcher "icon" sits on the desktop (top-left, just above the
 * background). Clicking it opens a modal panel that lists every WM action with
 * its current chord plus the mode-toggle key. While the panel is open it
 * captures all keyboard/mouse input (see the hooks in input.c).
 *
 * Editing: select a row (↑/↓ or click) and press Enter — the next chord you
 * press is bound to that action live. Closing the panel persists the keymap to
 * the config file via config_save(). The heavy lifting (chord parsing/format,
 * rebinding) lives in input.c/config.c; this file is just UI + state.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>
#include <string.h>

/* Desktop launcher: a roomy icon "tile" (a big gear above a label), top-left.
 * It lives on the desktop, below the windows, so windows can cover it. */
#define ICON_Y 0
#define ICON_X 1
#define ICON_W 12
#define ICON_H 4

/* Panel sizing. Height/width are clamped to the screen at open/draw time. */
#define PANEL_W      56
#define PANEL_CHROME 4   /* top border + status + hint + bottom border */

/* ----- small helpers ----------------------------------------------------- */

static int total_rows(void)        { return keymap_action_count() + 1; }
static bool is_toggle_row(int r)   { return r == keymap_action_count(); }

/* Desired centered panel geometry for the current screen size. */
static void panel_geom(const WM *wm, int *y, int *x, int *h, int *w)
{
    int W = PANEL_W;
    if (W > (int)wm->scr_cols - 2) W = (int)wm->scr_cols - 2;
    if (W < 24) W = 24;

    int H = total_rows() + PANEL_CHROME;
    if (H > (int)wm->scr_rows - 2) H = (int)wm->scr_rows - 2;
    if (H < PANEL_CHROME + 1) H = PANEL_CHROME + 1;

    int py = ((int)wm->scr_rows - H) / 2;
    int px = ((int)wm->scr_cols - W) / 2;
    *y = py < 0 ? 0 : py;
    *x = px < 0 ? 0 : px;
    *h = H;
    *w = W;
}

/* Number of list rows the panel's viewport can show. */
static int viewport_rows(const WM *wm)
{
    int H = wm->settings.panel ? (int)ncplane_dim_y(wm->settings.panel) : 0;
    int v = H - PANEL_CHROME;
    return v < 1 ? 1 : v;
}

static void clamp_scroll(WM *wm)
{
    Settings *s = &wm->settings;
    int v = viewport_rows(wm);
    int n = total_rows();
    if (s->sel < 0) s->sel = 0;
    if (s->sel >= n) s->sel = n - 1;
    if (s->sel < s->scroll)         s->scroll = s->sel;
    if (s->sel >= s->scroll + v)    s->scroll = s->sel - v + 1;
    int maxscroll = n - v;
    if (maxscroll < 0) maxscroll = 0;
    if (s->scroll > maxscroll) s->scroll = maxscroll;
    if (s->scroll < 0) s->scroll = 0;
}

/* ----- the desktop launcher icon ----------------------------------------- */

void settings_init(WM *wm)
{
    ncplane_options o = {0};
    o.y = ICON_Y;
    o.x = ICON_X;
    o.rows = ICON_H;
    o.cols = ICON_W;
    wm->settings.icon = ncplane_create(wm->std, &o);
    if (!wm->settings.icon) {
        return;
    }
    struct ncplane *p = wm->settings.icon;

    /* Tile background. */
    uint64_t base = 0;
    ncchannels_set_fg_rgb8(&base, 0xe0, 0xe0, 0xe8);
    ncchannels_set_bg_rgb8(&base, 0x2a, 0x2a, 0x3c);
    ncplane_set_base(p, " ", 0, base);
    ncplane_set_bg_rgb8(p, 0x2a, 0x2a, 0x3c);
    ncplane_erase(p);

    /* Rounded tile border. */
    ncplane_set_fg_rgb8(p, 0x60, 0x80, 0xc0);
    ncplane_putegc_yx(p, 0, 0, "╭", NULL);
    ncplane_putegc_yx(p, 0, ICON_W - 1, "╮", NULL);
    ncplane_putegc_yx(p, ICON_H - 1, 0, "╰", NULL);
    ncplane_putegc_yx(p, ICON_H - 1, ICON_W - 1, "╯", NULL);
    for (int c = 1; c < ICON_W - 1; c++) {
        ncplane_putegc_yx(p, 0, c, "─", NULL);
        ncplane_putegc_yx(p, ICON_H - 1, c, "─", NULL);
    }
    for (int r = 1; r < ICON_H - 1; r++) {
        ncplane_putegc_yx(p, r, 0, "│", NULL);
        ncplane_putegc_yx(p, r, ICON_W - 1, "│", NULL);
    }

    /* Big gear, centered on its own row. */
    ncplane_set_fg_rgb8(p, 0x9a, 0xd0, 0xff);
    ncplane_putegc_yx(p, 1, ICON_W / 2 - 1, "⚙", NULL);

    /* Label beneath it. */
    ncplane_set_fg_rgb8(p, 0xff, 0xff, 0xff);
    ncplane_putstr_yx(p, 2, 2, "Settings");
}

bool settings_icon_hit(WM *wm, int y, int x)
{
    struct ncplane *p = wm->settings.icon;
    if (!p) {
        return false;
    }
    int ay, ax;
    ncplane_abs_yx(p, &ay, &ax);
    unsigned r, c;
    ncplane_dim_yx(p, &r, &c);
    return y >= ay && y < ay + (int)r && x >= ax && x < ax + (int)c;
}

/* ----- open / close ------------------------------------------------------ */

void settings_open(WM *wm)
{
    Settings *s = &wm->settings;
    if (s->open) {
        return;
    }

    int py, px, H, W;
    panel_geom(wm, &py, &px, &H, &W);

    ncplane_options o = {0};
    o.y = py;
    o.x = px;
    o.rows = (unsigned)H;
    o.cols = (unsigned)W;
    s->panel = ncplane_create(wm->std, &o);
    if (!s->panel) {
        return;
    }
    uint64_t base = 0;
    ncchannels_set_fg_rgb8(&base, 0xd0, 0xd0, 0xd8);
    ncchannels_set_bg_rgb8(&base, 0x1c, 0x1c, 0x28);
    ncplane_set_base(s->panel, " ", 0, base);

    s->open = true;
    s->capturing = false;
    s->sel = 0;
    s->scroll = 0;
    s->dirty = true;
    snprintf(s->status, sizeof(s->status),
             "Enter: rebind   D: unbind   S: save   Esc: close");
    ncplane_move_top(s->panel);
    vp_log("settings: open\n");
}

void settings_close(WM *wm)
{
    Settings *s = &wm->settings;
    if (!s->open) {
        return;
    }
    s->open = false;
    s->capturing = false;
    if (s->panel) {
        ncplane_destroy(s->panel);
        s->panel = NULL;
    }
    config_save(&wm->config);
    vp_log("settings: close (saved)\n");
}

void settings_teardown(WM *wm)
{
    Settings *s = &wm->settings;
    if (s->panel) {
        ncplane_destroy(s->panel);
        s->panel = NULL;
    }
    if (s->icon) {
        ncplane_destroy(s->icon);
        s->icon = NULL;
    }
}

/* ----- editing actions --------------------------------------------------- */

static void apply_capture(WM *wm, uint32_t id, unsigned mods)
{
    Settings *s = &wm->settings;
    char cb[32];
    keymap_format_chord(id, mods, cb, sizeof(cb));

    if (is_toggle_row(s->sel)) {
        wm->config.toggle_key = id; /* modifiers ignored for the toggle */
        if (config_manual_shadows(&wm->config, true, 0, id, mods)) {
            snprintf(s->status, sizeof(s->status),
                     "Toggle set to %s — your manual config overrides it on restart", cb);
        } else {
            snprintf(s->status, sizeof(s->status), "Toggle key set to %s", cb);
        }
    } else {
        vp_action act;
        const char *label = NULL;
        keymap_action_info(s->sel, &act, &label);
        keymap_rebind_action(&wm->config, act, id, mods);
        if (config_manual_shadows(&wm->config, false, act, id, mods)) {
            snprintf(s->status, sizeof(s->status),
                     "%s = %s — your manual config overrides it on restart",
                     label ? label : "", cb);
        } else {
            snprintf(s->status, sizeof(s->status), "%s = %s", label ? label : "", cb);
        }
    }
    s->capturing = false;
    s->dirty = true;
}

static void do_unbind(WM *wm)
{
    Settings *s = &wm->settings;
    if (is_toggle_row(s->sel)) {
        snprintf(s->status, sizeof(s->status), "The toggle key cannot be unbound");
    } else {
        vp_action act;
        const char *label = NULL;
        keymap_action_info(s->sel, &act, &label);
        keymap_unbind_action(&wm->config, act);
        if (config_manual_shadows(&wm->config, false, act, 0, 0)) {
            snprintf(s->status, sizeof(s->status),
                     "%s unbound — your manual config rebinds it on restart",
                     label ? label : "");
        } else {
            snprintf(s->status, sizeof(s->status), "%s unbound", label ? label : "");
        }
    }
    s->dirty = true;
}

/* ----- input ------------------------------------------------------------- */

void settings_handle_key(WM *wm, const ncinput *ni)
{
    Settings *s = &wm->settings;
    if (!s->open) {
        return;
    }

    if (s->capturing) {
        if (ni->id == NCKEY_ESC) {
            s->capturing = false;
            snprintf(s->status, sizeof(s->status), "Rebind cancelled");
            s->dirty = true;
            return;
        }
        uint32_t id;
        unsigned mods;
        if (keymap_chord_from_input(ni, &id, &mods)) {
            apply_capture(wm, id, mods);
        }
        /* a bare modifier press: keep waiting */
        return;
    }

    switch (ni->id) {
    case NCKEY_UP:    s->sel--; clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_DOWN:  s->sel++; clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_PGUP:  s->sel -= viewport_rows(wm); clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_PGDOWN:s->sel += viewport_rows(wm); clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_HOME:  s->sel = 0; clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_END:   s->sel = total_rows() - 1; clamp_scroll(wm); s->dirty = true; break;
    case NCKEY_ENTER:
    case ' ':
        s->capturing = true;
        snprintf(s->status, sizeof(s->status), "Press a key chord…  (Esc cancels)");
        s->dirty = true;
        break;
    case 'd': case 'D':
    case NCKEY_DEL:
    case NCKEY_BACKSPACE:
        do_unbind(wm);
        break;
    case 's': case 'S':
        snprintf(s->status, sizeof(s->status),
                 config_save(&wm->config) ? "Saved" : "Save failed (see VP_DEBUG)");
        s->dirty = true;
        break;
    case NCKEY_ESC:
    case 'q': case 'Q':
        settings_close(wm);
        break;
    default:
        break;
    }
}

void settings_click(WM *wm, int btn, int y, int x)
{
    Settings *s = &wm->settings;
    if (!s->open || btn != 1) {
        return;
    }
    int ay, ax;
    ncplane_abs_yx(s->panel, &ay, &ax);
    unsigned ph, pw;
    ncplane_dim_yx(s->panel, &ph, &pw);

    /* Click outside the panel: dismiss (and save). */
    if (y < ay || y >= ay + (int)ph || x < ax || x >= ax + (int)pw) {
        settings_close(wm);
        return;
    }

    /* Click on a list row: select it and start capturing immediately. */
    int rely = y - ay;
    int listrow = rely - 1; /* row 0 is the top border */
    if (listrow >= 0 && listrow < viewport_rows(wm)) {
        int row = s->scroll + listrow;
        if (row >= 0 && row < total_rows()) {
            s->sel = row;
            s->capturing = true;
            snprintf(s->status, sizeof(s->status), "Press a key chord…  (Esc cancels)");
            s->dirty = true;
        }
    }
}

void settings_scroll(WM *wm, int dir)
{
    Settings *s = &wm->settings;
    if (!s->open) {
        return;
    }
    s->sel += dir;
    clamp_scroll(wm);
    s->dirty = true;
}

/* ----- drawing ----------------------------------------------------------- */

static void draw_border(struct ncplane *p, int W, int H)
{
    ncplane_set_fg_rgb8(p, 0x60, 0x80, 0xc0);
    ncplane_putegc_yx(p, 0, 0, "┌", NULL);
    ncplane_putegc_yx(p, 0, W - 1, "┐", NULL);
    ncplane_putegc_yx(p, H - 1, 0, "└", NULL);
    ncplane_putegc_yx(p, H - 1, W - 1, "┘", NULL);
    for (int c = 1; c < W - 1; c++) {
        ncplane_putegc_yx(p, 0, c, "─", NULL);
        ncplane_putegc_yx(p, H - 1, c, "─", NULL);
    }
    for (int r = 1; r < H - 1; r++) {
        ncplane_putegc_yx(p, r, 0, "│", NULL);
        ncplane_putegc_yx(p, r, W - 1, "│", NULL);
    }
    ncplane_set_fg_rgb8(p, 0xff, 0xff, 0xff);
    ncplane_putstr_yx(p, 0, 2, " Keybindings ");
}

static void draw_row(struct ncplane *p, int prow, int W,
                     const char *label, const char *chord,
                     bool selected, bool capturing)
{
    if (selected) {
        if (capturing) {
            ncplane_set_fg_rgb8(p, 0x10, 0x10, 0x10);
            ncplane_set_bg_rgb8(p, 0xe0, 0xa0, 0x30);
        } else {
            ncplane_set_fg_rgb8(p, 0xff, 0xff, 0xff);
            ncplane_set_bg_rgb8(p, 0x20, 0x50, 0xa0);
        }
        for (int c = 1; c < W - 1; c++) {
            ncplane_putchar_yx(p, prow, c, ' ');
        }
    } else {
        ncplane_set_fg_rgb8(p, 0xd0, 0xd0, 0xd8);
        ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    }

    const char *shown = capturing ? "‹press a key…›" : chord;
    int chord_x = W - 2 - (int)strlen(shown);
    if (chord_x < 2) chord_x = 2;

    /* label, truncated so it can't run into the chord column */
    char lbuf[128];
    int maxlabel = chord_x - 2 - 1;
    if (maxlabel < 0) maxlabel = 0;
    if (maxlabel > (int)sizeof(lbuf) - 1) maxlabel = (int)sizeof(lbuf) - 1;
    snprintf(lbuf, sizeof(lbuf), "%-*.*s", maxlabel, maxlabel, label);
    ncplane_putstr_yx(p, prow, 2, lbuf);

    if (!selected) {
        ncplane_set_fg_rgb8(p, 0x90, 0xb0, 0xe0); /* dim accent for the chord */
    }
    ncplane_putstr_yx(p, prow, chord_x, shown);
}

void settings_render(WM *wm)
{
    Settings *s = &wm->settings;
    if (!s->open || !s->panel || !s->dirty) {
        return;
    }
    struct ncplane *p = s->panel;

    /* Track screen resizes: re-center and re-size to fit. */
    int py, px, H, W;
    panel_geom(wm, &py, &px, &H, &W);
    if (H != (int)ncplane_dim_y(p) || W != (int)ncplane_dim_x(p)) {
        ncplane_resize_simple(p, (unsigned)H, (unsigned)W);
    }
    ncplane_move_yx(p, py, px);
    int v = viewport_rows(wm);

    clamp_scroll(wm);
    ncplane_erase(p);
    draw_border(p, W, H);

    for (int i = 0; i < v; i++) {
        int row = s->scroll + i;
        if (row >= total_rows()) {
            break;
        }
        char chord[64];
        const char *label;
        if (is_toggle_row(row)) {
            label = "Toggle INTERPRET / PASSTHROUGH";
            keymap_format_chord(wm->config.toggle_key, 0, chord, sizeof(chord));
        } else {
            vp_action act;
            keymap_action_info(row, &act, &label);
            keymap_chord_for_action(&wm->config, act, chord, sizeof(chord));
        }
        draw_row(p, 1 + i, W, label, chord,
                 row == s->sel, s->capturing && row == s->sel);
    }

    /* Status + hint lines. */
    ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    ncplane_set_fg_rgb8(p, 0xc0, 0xc0, 0x80);
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, s->status);
    ncplane_putstr_yx(p, H - 3, 2, sbuf);

    ncplane_set_fg_rgb8(p, 0x80, 0x80, 0x90);
    char hbuf[128];
    snprintf(hbuf, sizeof(hbuf), "%-*.*s", W - 4, W - 4,
             "↑/↓ select · Enter rebind · D unbind · S save · Esc close");
    ncplane_putstr_yx(p, H - 2, 2, hbuf);

    ncplane_move_top(p);
    s->dirty = false;
}
