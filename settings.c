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

/* settings.c - the in-app settings panel.
 *
 * A small launcher "icon" sits on the desktop (top-left, just above the
 * background). Clicking it opens a modal panel that lands on a Control-Panel
 * grid of tiles; each tile opens a sub-view. Populated tiles are "Keybindings"
 * (the keybinding editor) and "Terminal" (scrollback size / scroll step); the
 * rest are empty placeholders, ready for future settings. While the panel is
 * open it captures all keyboard/mouse input (see the hooks in input.c).
 *
 * Keybinding editor: select a row (↑/↓ or click) and press Enter - the next
 * chord you press is bound to that action live. Esc returns to the grid; Esc on
 * the grid closes the panel and persists the keymap via config_save(). The heavy
 * lifting (chord parsing/format, rebinding) lives in input.c/config.c; this file
 * is just UI + state.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stddef.h>
#include <stdio.h>
#include <string.h>

/* Desktop launcher: a roomy icon "tile" (a big gear above a label), top-left.
 * It lives on the desktop, below the windows, so windows can cover it. */
#define ICON_Y 1
#define ICON_X 1
#define ICON_W 12
#define ICON_H 4

/* Keybinding-view panel sizing. Clamped to the screen at open/draw time. */
#define PANEL_W      56
#define PANEL_CHROME 4   /* top border + status + hint + bottom border */

/* Control-Panel grid: a fixed lattice of tiles (à la a classic Control Panel),
 * the panel's landing page. Only the first GRID_ENTRY_COUNT cells are populated;
 * the rest are drawn as empty placeholders for future settings. */
#define GRID_COLS  3
#define GRID_ROWS  3
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define TILE_W     16
#define TILE_H     5
#define TILE_GAP   1
/* The grid sits inside the panel border with a one-cell margin; a title row and
 * a status row bracket it. */
#define GRID_ORIGIN_Y 2   /* first tile row (row 0 border, row 1 gap) */
#define GRID_ORIGIN_X 2   /* first tile col (col 0 border, col 1 margin) */

/* A populated Control-Panel tile: a glyph, a caption, and the sub-view it opens. */
typedef struct {
    const char   *glyph;
    const char   *label;
    settings_view view;
} grid_entry;

static const grid_entry g_grid_entries[] = {
    { "⌨", "Keybindings", SETTINGS_VIEW_KEYBINDINGS },
    { "▤", "Terminal",    SETTINGS_VIEW_TERMINAL },
};
#define GRID_ENTRY_COUNT ((int)(sizeof(g_grid_entries) / sizeof(g_grid_entries[0])))

/* ----- the Terminal view's rows -----
 * A data-driven list of integer settings. Each row points at a VpConfig field
 * (by offset, so it works against any WM's config), carries its bounds/step and
 * the vp_setting it maps to (for the manual-shadow warning), and an optional
 * live-apply hook. Adding a numeric Terminal setting is one row here plus its
 * config plumbing - the editor UI, clamping, and shadow warning all follow. */
typedef struct {
    const char *label;
    vp_setting  setting;       /* for config_manual_shadows_setting() */
    size_t      field_off;     /* offsetof(VpConfig, <int field>) */
    int         min, max, step;
    const char *zero_label;    /* shown in place of "0" (e.g. "off"), or NULL */
    void      (*on_change)(WM *); /* live-apply to open windows, or NULL */
} term_row;

static void term_apply_scrollback(WM *wm); /* defined below */

static const term_row g_term_rows[] = {
    { "Scrollback lines",    SETTING_SCROLLBACK,  offsetof(VpConfig, scrollback_max),
      0, VP_SCROLLBACK_LIMIT, 100, "off", term_apply_scrollback },
    { "Scroll step (lines)", SETTING_SCROLL_STEP, offsetof(VpConfig, scroll_step),
      1, VP_SCROLL_STEP_MAX,    1, NULL,  NULL },
};
#define TERM_ROWS ((int)(sizeof(g_term_rows) / sizeof(g_term_rows[0])))

/* The VpConfig int field backing row `i`. */
static int *term_field(WM *wm, int i)
{
    return (int *)((char *)&wm->config + g_term_rows[i].field_off);
}

/* ----- small helpers ----------------------------------------------------- */

/* Clamp a desktop icon's top-left (y,x) so the whole h×w tile stays on-screen
 * and above the taskbar row. */
static void clamp_icon_pos(const WM *wm, int h, int w, int *y, int *x)
{
    int maxy = (int)wm->scr_rows - (wm->taskbar ? 1 : 0) - h;
    int maxx = (int)wm->scr_cols - w;
    if (*y > maxy) *y = maxy;
    if (*x > maxx) *x = maxx;
    if (*y < 0) *y = 0;
    if (*x < 0) *x = 0;
}

static int total_rows(void)        { return keymap_action_count() + 1; }
static bool is_toggle_row(int r)   { return r == keymap_action_count(); }

/* Full Control-Panel grid dimensions (interior tiles only, no panel chrome). */
static int grid_inner_w(void) { return GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP; }
static int grid_inner_h(void) { return GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP; }

/* Top-left of grid cell `i` (row-major) within the panel. */
static void tile_rect(int i, int *ty, int *tx)
{
    int row = i / GRID_COLS;
    int col = i % GRID_COLS;
    *ty = GRID_ORIGIN_Y + row * (TILE_H + TILE_GAP);
    *tx = GRID_ORIGIN_X + col * (TILE_W + TILE_GAP);
}

/* Desired centered panel geometry for the current screen size and view. */
static void panel_geom(const WM *wm, int *y, int *x, int *h, int *w)
{
    int W, H;
    if (wm->settings.view == SETTINGS_VIEW_GRID) {
        /* border + margin on each side, a title row above and status row below. */
        W = grid_inner_w() + 2 * GRID_ORIGIN_X;
        H = grid_inner_h() + GRID_ORIGIN_Y + 2; /* +status row +bottom border */
    } else {
        W = PANEL_W;
        if (W < 24) W = 24;
        int rows = (wm->settings.view == SETTINGS_VIEW_TERMINAL)
                       ? TERM_ROWS : total_rows();
        H = rows + PANEL_CHROME;
        if (H < PANEL_CHROME + 1) H = PANEL_CHROME + 1;
    }
    if (W > (int)wm->scr_cols - 2) W = (int)wm->scr_cols - 2;
    if (H > (int)wm->scr_rows - 2) H = (int)wm->scr_rows - 2;

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

/* Resting top-left of the Settings tile: a user-dragged position from the
 * config if there is one, otherwise the built-in top-left corner. Always
 * clamped onto the current screen. */
static void settings_icon_geom(const WM *wm, int *y, int *x)
{
    if (wm->config.settings_icon_y >= 0) {
        *y = wm->config.settings_icon_y;
        *x = wm->config.settings_icon_x;
    } else {
        *y = ICON_Y;
        *x = ICON_X;
    }
    clamp_icon_pos(wm, ICON_H, ICON_W, y, x);
}

void settings_init(WM *wm)
{
    int iy, ix;
    settings_icon_geom(wm, &iy, &ix);

    ncplane_options o = {0};
    o.y = iy;
    o.x = ix;
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

void settings_icon_reflow(WM *wm)
{
    if (!wm->settings.icon) {
        return;
    }
    int iy, ix;
    settings_icon_geom(wm, &iy, &ix);
    ncplane_move_yx(wm->settings.icon, iy, ix);
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

/* ----- the desktop "Exit" launcher icon ---------------------------------- */
/* A sibling of the Settings tile, anchored to the bottom-right corner just
 * above the taskbar. viewpoint no longer quits when the last window closes, so
 * this is the explicit way out. */

#define EXIT_W 12
#define EXIT_H 4

/* Resting top-left of the Exit tile. If the user dragged it, the stored config
 * position wins; otherwise it auto-anchors to the bottom-right, one cell in from
 * the right edge with a one-row gap above the taskbar. Always clamped on-screen. */
static void exit_icon_geom(const WM *wm, int *y, int *x)
{
    if (wm->config.exit_icon_y >= 0) {
        *y = wm->config.exit_icon_y;
        *x = wm->config.exit_icon_x;
    } else {
        int bottom = (int)wm->scr_rows - (wm->taskbar ? 1 : 0); /* first row below us */
        *y = bottom - EXIT_H - 1; /* leave a blank row between the icon and taskbar */
        *x = (int)wm->scr_cols - EXIT_W - 1;
    }
    clamp_icon_pos(wm, EXIT_H, EXIT_W, y, x);
}

void exit_icon_init(WM *wm)
{
    int iy, ix;
    exit_icon_geom(wm, &iy, &ix);

    ncplane_options o = {0};
    o.y = iy;
    o.x = ix;
    o.rows = EXIT_H;
    o.cols = EXIT_W;
    wm->exit_icon = ncplane_create(wm->std, &o);
    if (!wm->exit_icon) {
        return;
    }
    struct ncplane *p = wm->exit_icon;

    /* Tile background (a warm red-grey to read as "quit"). */
    uint64_t base = 0;
    ncchannels_set_fg_rgb8(&base, 0xf0, 0xe0, 0xe0);
    ncchannels_set_bg_rgb8(&base, 0x3c, 0x2a, 0x2a);
    ncplane_set_base(p, " ", 0, base);
    ncplane_set_bg_rgb8(p, 0x3c, 0x2a, 0x2a);
    ncplane_erase(p);

    /* Rounded tile border. */
    ncplane_set_fg_rgb8(p, 0xc0, 0x60, 0x60);
    ncplane_putegc_yx(p, 0, 0, "╭", NULL);
    ncplane_putegc_yx(p, 0, EXIT_W - 1, "╮", NULL);
    ncplane_putegc_yx(p, EXIT_H - 1, 0, "╰", NULL);
    ncplane_putegc_yx(p, EXIT_H - 1, EXIT_W - 1, "╯", NULL);
    for (int c = 1; c < EXIT_W - 1; c++) {
        ncplane_putegc_yx(p, 0, c, "─", NULL);
        ncplane_putegc_yx(p, EXIT_H - 1, c, "─", NULL);
    }
    for (int r = 1; r < EXIT_H - 1; r++) {
        ncplane_putegc_yx(p, r, 0, "│", NULL);
        ncplane_putegc_yx(p, r, EXIT_W - 1, "│", NULL);
    }

    /* Big power symbol, centered on its own row. */
    ncplane_set_fg_rgb8(p, 0xff, 0x9a, 0x9a);
    ncplane_putegc_yx(p, 1, EXIT_W / 2 - 1, "⏻", NULL);

    /* Label beneath it, centered in the tile. */
    ncplane_set_fg_rgb8(p, 0xff, 0xff, 0xff);
    static const char label[] = "Exit";
    ncplane_putstr_yx(p, 2, (EXIT_W - (int)(sizeof(label) - 1)) / 2, label);
}

void exit_icon_reflow(WM *wm)
{
    if (!wm->exit_icon) {
        return;
    }
    int iy, ix;
    exit_icon_geom(wm, &iy, &ix);
    ncplane_move_yx(wm->exit_icon, iy, ix);
}

bool exit_icon_hit(WM *wm, int y, int x)
{
    struct ncplane *p = wm->exit_icon;
    if (!p) {
        return false;
    }
    int ay, ax;
    ncplane_abs_yx(p, &ay, &ax);
    unsigned r, c;
    ncplane_dim_yx(p, &r, &c);
    return y >= ay && y < ay + (int)r && x >= ax && x < ax + (int)c;
}

void exit_icon_teardown(WM *wm)
{
    if (wm->exit_icon) {
        ncplane_destroy(wm->exit_icon);
        wm->exit_icon = NULL;
    }
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
    s->view = SETTINGS_VIEW_GRID;
    s->grid_sel = 0;
    s->capturing = false;
    s->sel = 0;
    s->scroll = 0;
    s->dirty = true;
    snprintf(s->status, sizeof(s->status),
             "↑/↓/←/→: select   Enter: open   Esc: close");
    ncplane_move_top(s->panel);
    vp_log("settings: open\n");
}

/* Switch the panel to a sub-view (or back to the grid), seeding its UI state. */
static void settings_set_view(WM *wm, settings_view v)
{
    Settings *s = &wm->settings;
    s->view = v;
    s->capturing = false;
    s->dirty = true;
    if (v == SETTINGS_VIEW_KEYBINDINGS) {
        s->sel = 0;
        s->scroll = 0;
        snprintf(s->status, sizeof(s->status),
                 "Enter: rebind   D: unbind   S: save   Esc: back");
    } else if (v == SETTINGS_VIEW_TERMINAL) {
        s->sel = 0;
        s->scroll = 0;
        snprintf(s->status, sizeof(s->status),
                 "←/→: adjust   S: save   Esc: back");
    } else {
        snprintf(s->status, sizeof(s->status),
                 "↑/↓/←/→: select   Enter: open   Esc: close");
    }
    vp_log("settings: view -> %d\n", (int)v);
}

/* "Back": from a sub-view, return to the grid; from the grid, close the panel. */
static void settings_back(WM *wm)
{
    if (wm->settings.view == SETTINGS_VIEW_GRID) {
        settings_close(wm);
    } else {
        settings_set_view(wm, SETTINGS_VIEW_GRID);
    }
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
    wm->needs_render = true; /* recomposite the desktop without the panel */
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
        if (config_manual_shadows_setting(&wm->config, SETTING_TOGGLE_KEY)) {
            snprintf(s->status, sizeof(s->status),
                     "Toggle set to %s - your manual config overrides it on restart", cb);
        } else {
            snprintf(s->status, sizeof(s->status), "Toggle key set to %s", cb);
        }
    } else {
        vp_action act;
        const char *label = NULL;
        keymap_action_info(s->sel, &act, &label);
        keymap_rebind_action(&wm->config, act, id, mods);
        if (config_manual_shadows_action(&wm->config, act, id, mods)) {
            snprintf(s->status, sizeof(s->status),
                     "%s = %s - your manual config overrides it on restart",
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
        if (config_manual_shadows_action(&wm->config, act, 0, 0)) {
            snprintf(s->status, sizeof(s->status),
                     "%s unbound - your manual config rebinds it on restart",
                     label ? label : "");
        } else {
            snprintf(s->status, sizeof(s->status), "%s unbound", label ? label : "");
        }
    }
    s->dirty = true;
}

/* Push the live scrollback cap to every open window, trimming history to fit. */
static void term_apply_scrollback(WM *wm)
{
    for (int i = 0; i < wm->nwins; i++) {
        vt_set_scrollback_max(wm->wins[i], wm->config.scrollback_max);
    }
}

/* Nudge the Terminal-view setting on `row` by `dir` of its notches (clamped),
 * apply it live, and report - warning when the manual config will shadow it. */
static void term_adjust(WM *wm, int row, int dir)
{
    if (row < 0 || row >= TERM_ROWS) {
        return;
    }
    const term_row *tr = &g_term_rows[row];
    Settings *s = &wm->settings;
    int *field = term_field(wm, row);

    int v = *field + dir * tr->step;
    if (v < tr->min) v = tr->min;
    if (v > tr->max) v = tr->max;
    if (v != *field) {
        *field = v;
        if (tr->on_change) {
            tr->on_change(wm);
        }
    }

    if (config_manual_shadows_setting(&wm->config, tr->setting)) {
        snprintf(s->status, sizeof(s->status),
                 "%s - your manual config overrides it on restart", tr->label);
    } else if (tr->zero_label && *field == 0) {
        snprintf(s->status, sizeof(s->status), "%s = %s", tr->label, tr->zero_label);
    } else {
        snprintf(s->status, sizeof(s->status), "%s = %d", tr->label, *field);
    }
    s->dirty = true;
}

/* ----- input ------------------------------------------------------------- */

/* Move the grid selection by (drow, dcol), clamped to the populated tiles. */
static void grid_move(WM *wm, int drow, int dcol)
{
    Settings *s = &wm->settings;
    int row = s->grid_sel / GRID_COLS + drow;
    int col = s->grid_sel % GRID_COLS + dcol;
    if (row < 0) row = 0;
    if (col < 0) col = 0;
    if (col > GRID_COLS - 1) col = GRID_COLS - 1;
    int idx = row * GRID_COLS + col;
    if (idx >= 0 && idx < GRID_ENTRY_COUNT) {
        s->grid_sel = idx;
        s->dirty = true;
    }
}

static void settings_grid_key(WM *wm, const ncinput *ni)
{
    Settings *s = &wm->settings;
    switch (ni->id) {
    case NCKEY_UP:    grid_move(wm, -1, 0); break;
    case NCKEY_DOWN:  grid_move(wm, +1, 0); break;
    case NCKEY_LEFT:  grid_move(wm, 0, -1); break;
    case NCKEY_RIGHT: grid_move(wm, 0, +1); break;
    case NCKEY_ENTER:
    case ' ':
        if (s->grid_sel >= 0 && s->grid_sel < GRID_ENTRY_COUNT) {
            settings_set_view(wm, g_grid_entries[s->grid_sel].view);
        }
        break;
    case NCKEY_ESC:
    case 'q': case 'Q':
        settings_close(wm);
        break;
    default:
        break;
    }
}

static void settings_terminal_key(WM *wm, const ncinput *ni)
{
    Settings *s = &wm->settings;
    switch (ni->id) {
    case NCKEY_UP:    if (s->sel > 0)             { s->sel--; s->dirty = true; } break;
    case NCKEY_DOWN:  if (s->sel < TERM_ROWS - 1) { s->sel++; s->dirty = true; } break;
    case NCKEY_LEFT:  case '-': case '_': term_adjust(wm, s->sel, -1); break;
    case NCKEY_RIGHT: case '+': case '=': term_adjust(wm, s->sel, +1); break;
    case 's': case 'S':
        snprintf(s->status, sizeof(s->status),
                 config_save(&wm->config) ? "Saved" : "Save failed (see VP_DEBUG)");
        s->dirty = true;
        break;
    case NCKEY_ESC:
    case 'q': case 'Q':
        settings_back(wm);
        break;
    default:
        break;
    }
}

void settings_handle_key(WM *wm, const ncinput *ni)
{
    Settings *s = &wm->settings;
    if (!s->open) {
        return;
    }

    if (s->view == SETTINGS_VIEW_GRID) {
        settings_grid_key(wm, ni);
        return;
    }
    if (s->view == SETTINGS_VIEW_TERMINAL) {
        settings_terminal_key(wm, ni);
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
        settings_back(wm);
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

    /* Click outside the panel acts like Esc: step back one level (grid → close). */
    if (y < ay || y >= ay + (int)ph || x < ax || x >= ax + (int)pw) {
        settings_back(wm);
        return;
    }

    int rely = y - ay;
    int relx = x - ax;

    /* Grid view: open whichever populated tile was clicked. */
    if (s->view == SETTINGS_VIEW_GRID) {
        for (int i = 0; i < GRID_ENTRY_COUNT; i++) {
            int ty, tx;
            tile_rect(i, &ty, &tx);
            if (rely >= ty && rely < ty + TILE_H && relx >= tx && relx < tx + TILE_W) {
                s->grid_sel = i;
                settings_set_view(wm, g_grid_entries[i].view);
                return;
            }
        }
        return;
    }

    /* Terminal view: clicking a row selects it (adjust with ←/→ or the wheel). */
    if (s->view == SETTINGS_VIEW_TERMINAL) {
        int listrow = rely - 1; /* row 0 is the top border */
        if (listrow >= 0 && listrow < TERM_ROWS) {
            s->sel = listrow;
            s->dirty = true;
        }
        return;
    }

    /* Click on a list row: select it and start capturing immediately. */
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
    if (s->view == SETTINGS_VIEW_TERMINAL) {
        term_adjust(wm, s->sel, dir < 0 ? +1 : -1); /* wheel up raises the value */
        return;
    }
    if (s->view != SETTINGS_VIEW_KEYBINDINGS) {
        return;
    }
    s->sel += dir;
    clamp_scroll(wm);
    s->dirty = true;
}

/* ----- drawing ----------------------------------------------------------- */

static void draw_border(struct ncplane *p, int W, int H, const char *title)
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
    ncplane_putstr_yx(p, 0, 2, title);
}

/* Draw the bottom status line shared by both views. */
static void draw_status(struct ncplane *p, int W, int H, const char *status)
{
    ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    ncplane_set_fg_rgb8(p, 0xc0, 0xc0, 0x80);
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, status);
    ncplane_putstr_yx(p, H - 2, 2, sbuf);
}

/* ----- the Control-Panel grid -------------------------------------------- */

/* One tile: rounded box, a glyph, and a caption. `populated` tiles that aren't
 * selected read as live; empty cells are drawn dim. The selected tile glows. */
static void draw_tile(struct ncplane *p, int ty, int tx,
                      const char *glyph, const char *label,
                      bool populated, bool selected)
{
    if (selected) {
        ncplane_set_fg_rgb8(p, 0x9a, 0xd0, 0xff);
        ncplane_set_bg_rgb8(p, 0x20, 0x50, 0xa0);
        for (int r = 1; r < TILE_H - 1; r++)
            for (int c = 1; c < TILE_W - 1; c++)
                ncplane_putchar_yx(p, ty + r, tx + c, ' ');
    } else if (populated) {
        ncplane_set_fg_rgb8(p, 0x60, 0x80, 0xc0);
        ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    } else {
        ncplane_set_fg_rgb8(p, 0x3a, 0x3a, 0x4a);
        ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    }

    ncplane_putegc_yx(p, ty, tx, "╭", NULL);
    ncplane_putegc_yx(p, ty, tx + TILE_W - 1, "╮", NULL);
    ncplane_putegc_yx(p, ty + TILE_H - 1, tx, "╰", NULL);
    ncplane_putegc_yx(p, ty + TILE_H - 1, tx + TILE_W - 1, "╯", NULL);
    for (int c = 1; c < TILE_W - 1; c++) {
        ncplane_putegc_yx(p, ty, tx + c, "─", NULL);
        ncplane_putegc_yx(p, ty + TILE_H - 1, tx + c, "─", NULL);
    }
    for (int r = 1; r < TILE_H - 1; r++) {
        ncplane_putegc_yx(p, ty + r, tx, "│", NULL);
        ncplane_putegc_yx(p, ty + r, tx + TILE_W - 1, "│", NULL);
    }

    if (!populated) {
        return;
    }

    /* Glyph centered near the top, caption beneath it. */
    if (selected) {
        ncplane_set_fg_rgb8(p, 0xff, 0xff, 0xff);
    } else {
        ncplane_set_fg_rgb8(p, 0x9a, 0xd0, 0xff);
    }
    if (glyph) {
        ncplane_putegc_yx(p, ty + 1, tx + TILE_W / 2 - 1, glyph, NULL);
    }
    if (label) {
        int len = (int)strlen(label);
        int lx = tx + (TILE_W - len) / 2;
        if (lx < tx + 1) lx = tx + 1;
        ncplane_putstr_yx(p, ty + TILE_H - 2, lx, label);
    }
}

static void draw_grid(WM *wm, struct ncplane *p, int W, int H)
{
    Settings *s = &wm->settings;
    ncplane_erase(p);
    draw_border(p, W, H, " Settings ");

    for (int i = 0; i < GRID_CELLS; i++) {
        int ty, tx;
        tile_rect(i, &ty, &tx);
        bool populated = i < GRID_ENTRY_COUNT;
        const char *glyph = populated ? g_grid_entries[i].glyph : NULL;
        const char *label = populated ? g_grid_entries[i].label : NULL;
        draw_tile(p, ty, tx, glyph, label, populated, i == s->grid_sel && populated);
    }

    draw_status(p, W, H, s->status);
}

/* ----- the keybinding editor --------------------------------------------- */

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

static void draw_keybindings(WM *wm, struct ncplane *p, int W, int H)
{
    Settings *s = &wm->settings;
    int v = viewport_rows(wm);

    clamp_scroll(wm);
    ncplane_erase(p);
    draw_border(p, W, H, " Keybindings ");

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
             "↑/↓ select · Enter rebind · D unbind · S save · Esc back");
    ncplane_putstr_yx(p, H - 2, 2, hbuf);
}

/* ----- the Terminal settings view ---------------------------------------- */

static void draw_terminal(WM *wm, struct ncplane *p, int W, int H)
{
    Settings *s = &wm->settings;
    ncplane_erase(p);
    draw_border(p, W, H, " Terminal ");

    for (int i = 0; i < TERM_ROWS; i++) {
        const term_row *tr = &g_term_rows[i];
        int v = *term_field(wm, i);
        char val[32];
        if (tr->zero_label && v == 0) {
            snprintf(val, sizeof(val), "%s", tr->zero_label);
        } else {
            snprintf(val, sizeof(val), "%d", v);
        }
        draw_row(p, 1 + i, W, tr->label, val, i == s->sel, false);
    }

    /* Status + hint lines (mirrors the keybinding editor's footer). */
    ncplane_set_bg_rgb8(p, 0x1c, 0x1c, 0x28);
    ncplane_set_fg_rgb8(p, 0xc0, 0xc0, 0x80);
    char sbuf[128];
    snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, s->status);
    ncplane_putstr_yx(p, H - 3, 2, sbuf);

    ncplane_set_fg_rgb8(p, 0x80, 0x80, 0x90);
    char hbuf[128];
    snprintf(hbuf, sizeof(hbuf), "%-*.*s", W - 4, W - 4,
             "↑/↓ select · ←/→ adjust · S save · Esc back");
    ncplane_putstr_yx(p, H - 2, 2, hbuf);
}

bool settings_render(WM *wm)
{
    Settings *s = &wm->settings;
    if (!s->open || !s->panel || !s->dirty) {
        return false;
    }
    struct ncplane *p = s->panel;

    /* Track screen resizes and view switches: re-center and re-size to fit. */
    int py, px, H, W;
    panel_geom(wm, &py, &px, &H, &W);
    if (H != (int)ncplane_dim_y(p) || W != (int)ncplane_dim_x(p)) {
        ncplane_resize_simple(p, (unsigned)H, (unsigned)W);
    }
    ncplane_move_yx(p, py, px);

    if (s->view == SETTINGS_VIEW_GRID) {
        draw_grid(wm, p, W, H);
    } else if (s->view == SETTINGS_VIEW_TERMINAL) {
        draw_terminal(wm, p, W, H);
    } else {
        draw_keybindings(wm, p, W, H);
    }

    ncplane_move_top(p);
    s->dirty = false;
    return true;
}
