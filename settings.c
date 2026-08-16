// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie <robin413@protonmail.com>
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
#include <stdlib.h>
#include <string.h>

#define ICON_Y 1
#define ICON_X 1
#define ICON_W 12
#define ICON_H 4

#define PANEL_W 56
#define PANEL_CHROME 4 /* top border + status + hint + bottom border */

/* Control-Panel grid: a fixed lattice of tiles (à la a classic Control Panel),
 * the panel's landing page. Only the first GRID_ENTRY_COUNT cells are populated;
 * the rest are drawn as empty placeholders for future settings. */
#define GRID_COLS 3
#define GRID_ROWS 3
#define GRID_CELLS (GRID_COLS * GRID_ROWS)
#define TILE_W 16
#define TILE_H 5
#define TILE_GAP 1
/* The grid sits inside the panel border with a one-cell margin; a title row and
 * a status row bracket it. */
#define GRID_ORIGIN_Y 2 /* first tile row (row 0 border, row 1 gap) */
#define GRID_ORIGIN_X 2 /* first tile col (col 0 border, col 1 margin) */

typedef struct {
	const char *glyph;
	const char *label;
	settings_view view;
} grid_entry;

static const grid_entry g_grid_entries[] = {
	{ "⌨", "Keybindings", SETTINGS_VIEW_KEYBINDINGS },
	{ "▤", "Terminal", SETTINGS_VIEW_TERMINAL },
	{ "▦", "Appearance", SETTINGS_VIEW_APPEARANCE },
};
#define GRID_ENTRY_COUNT \
	((int)(sizeof(g_grid_entries) / sizeof(g_grid_entries[0])))

/* ----- the Terminal view's rows -----
 * A data-driven list of integer settings. Each row points at a VpConfig field
 * (by offset, so it works against any WM's config), carries its bounds/step and
 * the vp_setting it maps to (for the manual-shadow warning), and an optional
 * live-apply hook. Adding a numeric Terminal setting is one row here plus its
 * config plumbing - the editor UI, clamping, and shadow warning all follow. */
typedef struct {
	const char *label;
	vp_setting setting; /* for config_manual_shadows_setting() */
	size_t field_off; /* offsetof(VpConfig, <int field>) */
	int min, max, step;
	const char *zero_label; /* shown in place of "0" (e.g. "off"), or NULL */
	void (*on_change)(WM *); /* live-apply to open windows, or NULL */
} term_row;

static void term_apply_scrollback(WM *wm); /* defined below */

static const term_row g_term_rows[] = {
	{ "Scrollback lines", SETTING_SCROLLBACK,
	  offsetof(VpConfig, scrollback_max), 0, VP_SCROLLBACK_LIMIT, 100,
	  "off", term_apply_scrollback },
	{ "Scroll step (lines)", SETTING_SCROLL_STEP,
	  offsetof(VpConfig, scroll_step), 1, VP_SCROLL_STEP_MAX, 1, NULL,
	  NULL },
};
#define TERM_ROWS ((int)(sizeof(g_term_rows) / sizeof(g_term_rows[0])))

static int *term_field(WM *wm, int i)
{
	return (int *)((char *)&wm->config + g_term_rows[i].field_off);
}

static void clamp_icon_pos(const WM *wm, int h, int w, int *y, int *x)
{
	int maxy = (int)wm->scr_rows - (wm->taskbar ? 1 : 0) - h;
	int maxx = (int)wm->scr_cols - w;
	if (*y > maxy)
		*y = maxy;
	if (*x > maxx)
		*x = maxx;
	if (*y < 0)
		*y = 0;
	if (*x < 0)
		*x = 0;
}

static int app_total_rows(void); /* Appearance view row count (defined below) */

static int total_rows(void)
{
	return keymap_action_count() + 1;
}
static bool is_toggle_row(int r)
{
	return r == keymap_action_count();
}

static int grid_inner_w(void)
{
	return GRID_COLS * TILE_W + (GRID_COLS - 1) * TILE_GAP;
}
static int grid_inner_h(void)
{
	return GRID_ROWS * TILE_H + (GRID_ROWS - 1) * TILE_GAP;
}

static void tile_rect(int i, int *ty, int *tx)
{
	int row = i / GRID_COLS;
	int col = i % GRID_COLS;
	*ty = GRID_ORIGIN_Y + row * (TILE_H + TILE_GAP);
	*tx = GRID_ORIGIN_X + col * (TILE_W + TILE_GAP);
}

static void panel_geom(const WM *wm, int *y, int *x, int *h, int *w)
{
	int W, H;
	if (wm->settings.view == SETTINGS_VIEW_GRID) {
		/* border + margin on each side, a title row above and status row below. */
		W = grid_inner_w() + 2 * GRID_ORIGIN_X;
		H = grid_inner_h() + GRID_ORIGIN_Y +
		    2; /* +status row +bottom border */
	} else {
		W = PANEL_W;
		if (W < 24)
			W = 24;
		int rows = total_rows();
		if (wm->settings.view == SETTINGS_VIEW_TERMINAL) {
			rows = TERM_ROWS;
		} else if (wm->settings.view == SETTINGS_VIEW_APPEARANCE) {
			rows = app_total_rows();
		}
		H = rows + PANEL_CHROME;
		if (H < PANEL_CHROME + 1)
			H = PANEL_CHROME + 1;
	}
	if (W > (int)wm->scr_cols - 2)
		W = (int)wm->scr_cols - 2;
	if (H > (int)wm->scr_rows - 2)
		H = (int)wm->scr_rows - 2;

	int py = ((int)wm->scr_rows - H) / 2;
	int px = ((int)wm->scr_cols - W) / 2;
	*y = py < 0 ? 0 : py;
	*x = px < 0 ? 0 : px;
	*h = H;
	*w = W;
}

static int viewport_rows(const WM *wm)
{
	int H = wm->settings.panel ? comp_layer_rect(wm->settings.panel).h : 0;
	int v = H - PANEL_CHROME;
	return v < 1 ? 1 : v;
}

/* The Appearance view's fixed rows (a row per themeable color follows them). */
enum {
	APP_ROW_THEME = 0,
	APP_ROW_BG,
	APP_ROW_FIT,
	APP_ROW_IMAGE,
	APP_ROW_KEEP,
	APP_FIXED_ROWS,
};
static int app_total_rows(void)
{
	return APP_FIXED_ROWS + vp_theme_field_count();
}

static void clamp_scroll_n(WM *wm, int n)
{
	Settings *s = &wm->settings;
	int v = viewport_rows(wm);
	if (s->sel < 0)
		s->sel = 0;
	if (s->sel >= n)
		s->sel = n - 1;
	if (s->sel < s->scroll)
		s->scroll = s->sel;
	if (s->sel >= s->scroll + v)
		s->scroll = s->sel - v + 1;
	int maxscroll = n - v;
	if (maxscroll < 0)
		maxscroll = 0;
	if (s->scroll > maxscroll)
		s->scroll = maxscroll;
	if (s->scroll < 0)
		s->scroll = 0;
}

static void clamp_scroll(WM *wm)
{
	Settings *s = &wm->settings;
	int v = viewport_rows(wm);
	int n = total_rows();
	if (s->sel < 0)
		s->sel = 0;
	if (s->sel >= n)
		s->sel = n - 1;
	if (s->sel < s->scroll)
		s->scroll = s->sel;
	if (s->sel >= s->scroll + v)
		s->scroll = s->sel - v + 1;
	int maxscroll = n - v;
	if (maxscroll < 0)
		maxscroll = 0;
	if (s->scroll > maxscroll)
		s->scroll = maxscroll;
	if (s->scroll < 0)
		s->scroll = 0;
}

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

static void settings_icon_paint(struct ncplane *p, bool full, void *user);

void settings_init(WM *wm)
{
	int iy, ix;
	settings_icon_geom(wm, &iy, &ix);
	vp_rect r = { iy, ix, ICON_H, ICON_W };
	wm->settings.icon = comp_layer_new(wm->comp, VP_BAND_DESKTOP, r,
					   settings_icon_paint, wm);
}

/* The launcher tiles repaint from the live theme, so recolouring them is just a
 * damage - there is no separate "redraw now" path that could disagree with what
 * the compositor last put on screen. */
void settings_icon_redraw(WM *wm)
{
	comp_layer_damage(wm->settings.icon);
}

static void settings_icon_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	WM *wm = user;

	uint64_t base = 0;
	vp_rgb fg = wm->theme.icon_fg, bg = wm->theme.icon_bg;
	ncchannels_set_fg_rgb8(&base, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
			       fg & 0xff);
	ncchannels_set_bg_rgb8(&base, (bg >> 16) & 0xff, (bg >> 8) & 0xff,
			       bg & 0xff);
	ncplane_set_base(p, " ", 0, base);
	vp_setbg(p, wm->theme.icon_bg);
	ncplane_erase(p);

	vp_setfg(p, wm->theme.icon_border);
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

	vp_setfg(p, wm->theme.icon_glyph);
	ncplane_putegc_yx(p, 1, ICON_W / 2 - 1, "⚙", NULL);

	vp_setfg(p, wm->theme.icon_fg);
	ncplane_putstr_yx(p, 2, 2, "Settings");
}

void settings_icon_reflow(WM *wm)
{
	if (!wm->settings.icon) {
		return;
	}
	int iy, ix;
	settings_icon_geom(wm, &iy, &ix);
	comp_layer_move(wm->settings.icon, iy, ix);
}

/* Hit tests read the compositor's geometry rather than the plane's, so they stay
 * correct for a layer that is currently hidden or mid-drag. */
static bool layer_hit(const vp_layer *l, int y, int x)
{
	if (!l) {
		return false;
	}
	vp_rect r = comp_layer_abs(l);
	return y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w;
}

bool settings_icon_hit(WM *wm, int y, int x)
{
	return layer_hit(wm->settings.icon, y, x);
}

/* Siblings of the Settings tile, anchored to the bottom-right corner just above
 * the taskbar. Persist detaches the UI; Die kills the daemon-owned PTYs too. */
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
		int bottom = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
		*y = bottom - EXIT_H -
		     1; /* leave a blank row between the icon and taskbar */
		*x = (int)wm->scr_cols - EXIT_W - 1;
	}
	clamp_icon_pos(wm, EXIT_H, EXIT_W, y, x);
}

static bool icon_rect_overlap(int ay, int ax, int by, int bx)
{
	return ax < bx + EXIT_W && ax + EXIT_W > bx && ay < by + EXIT_H &&
	       ay + EXIT_H > by;
}

static void place_next_to_exit(const WM *wm, int ey, int ex, int *y, int *x)
{
	int bottom = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);

	*y = ey;
	*x = ex - EXIT_W - 1;
	if (*x >= 0) {
		return;
	}

	*x = ex + EXIT_W + 1;
	if (*x + EXIT_W <= (int)wm->scr_cols) {
		return;
	}

	*x = ex;
	*y = ey - EXIT_H - 1;
	if (*y >= 0) {
		return;
	}

	*y = ey + EXIT_H + 1;
	if (*y + EXIT_H <= bottom) {
		return;
	}

	*y = ey;
	*x = ex;
}

static void die_icon_geom(const WM *wm, int *y, int *x)
{
	int ey, ex;
	exit_icon_geom(wm, &ey, &ex);
	if (wm->config.die_icon_y >= 0) {
		*y = wm->config.die_icon_y;
		*x = wm->config.die_icon_x;
		clamp_icon_pos(wm, EXIT_H, EXIT_W, y, x);
		if (!icon_rect_overlap(*y, *x, ey, ex)) {
			return;
		}
	} else {
		place_next_to_exit(wm, ey, ex, y, x);
	}
	clamp_icon_pos(wm, EXIT_H, EXIT_W, y, x);
	if (icon_rect_overlap(*y, *x, ey, ex)) {
		place_next_to_exit(wm, ey, ex, y, x);
		clamp_icon_pos(wm, EXIT_H, EXIT_W, y, x);
	}
}

static void draw_exit_tile(WM *wm, struct ncplane *p, const char *glyph,
			   const char *label)
{
	if (!p) {
		return;
	}

	uint64_t base = 0;
	vp_rgb fg = wm->theme.exit_fg, bg = wm->theme.exit_bg;
	ncchannels_set_fg_rgb8(&base, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
			       fg & 0xff);
	ncchannels_set_bg_rgb8(&base, (bg >> 16) & 0xff, (bg >> 8) & 0xff,
			       bg & 0xff);
	ncplane_set_base(p, " ", 0, base);
	vp_setbg(p, wm->theme.exit_bg);
	ncplane_erase(p);

	vp_setfg(p, wm->theme.exit_border);
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

	vp_setfg(p, wm->theme.exit_glyph);
	ncplane_putegc_yx(p, 1, EXIT_W / 2 - 1, glyph, NULL);

	vp_setfg(p, wm->theme.exit_fg);
	ncplane_putstr_yx(p, 2, (EXIT_W - (int)strlen(label)) / 2, label);
}

/* Painters for the two exit tiles: same tile, different glyph and label. */
static void exit_icon_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	draw_exit_tile(user, p, "⏻", "Persist");
}

static void die_icon_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	draw_exit_tile(user, p, "×", "Die");
}

void exit_icon_init(WM *wm)
{
	int iy, ix;
	exit_icon_geom(wm, &iy, &ix);
	vp_rect r = { iy, ix, EXIT_H, EXIT_W };
	wm->exit_icon = comp_layer_new(wm->comp, VP_BAND_DESKTOP, r,
				       exit_icon_paint, wm);

	die_icon_geom(wm, &iy, &ix);
	r.y = iy;
	r.x = ix;
	wm->die_icon = comp_layer_new(wm->comp, VP_BAND_DESKTOP, r,
				      die_icon_paint, wm);
}

void exit_icon_redraw(WM *wm)
{
	comp_layer_damage(wm->exit_icon);
}

void die_icon_redraw(WM *wm)
{
	comp_layer_damage(wm->die_icon);
}

void exit_icon_reflow(WM *wm)
{
	if (!wm->exit_icon) {
		return;
	}
	int iy, ix;
	exit_icon_geom(wm, &iy, &ix);
	comp_layer_move(wm->exit_icon, iy, ix);
}

void die_icon_reflow(WM *wm)
{
	if (!wm->die_icon) {
		return;
	}
	int iy, ix;
	die_icon_geom(wm, &iy, &ix);
	comp_layer_move(wm->die_icon, iy, ix);
}

bool exit_icon_hit(WM *wm, int y, int x)
{
	return layer_hit(wm->exit_icon, y, x);
}

bool die_icon_hit(WM *wm, int y, int x)
{
	return layer_hit(wm->die_icon, y, x);
}

void exit_icon_teardown(WM *wm)
{
	comp_layer_destroy(wm->exit_icon);
	wm->exit_icon = NULL;
	comp_layer_destroy(wm->die_icon);
	wm->die_icon = NULL;
}

static void panel_paint(struct ncplane *p, bool full, void *user);

void settings_open(WM *wm)
{
	Settings *s = &wm->settings;
	if (s->open) {
		return;
	}

	int py, px, H, W;
	panel_geom(wm, &py, &px, &H, &W);

	/* VP_BAND_MODAL is above every window and above the taskbar, which is
	 * the whole of what "modal" means to the display. */
	vp_rect r = { py, px, H, W };
	s->panel = comp_layer_new(wm->comp, VP_BAND_MODAL, r, panel_paint, wm);
	if (!s->panel) {
		return;
	}

	s->open = true;
	s->view = SETTINGS_VIEW_GRID;
	s->grid_sel = 0;
	s->capturing = false;
	s->sel = 0;
	s->scroll = 0;
	snprintf(s->status, sizeof(s->status),
		 "↑/↓/←/→: select   Enter: open   Esc: close");
	vp_log("settings: open\n");
}

void settings_damage(Settings *s)
{
	comp_layer_damage(s->panel);
}

/* Switch the panel to a sub-view (or back to the grid), seeding its UI state. */
static void settings_set_view(WM *wm, settings_view v)
{
	Settings *s = &wm->settings;
	s->view = v;
	s->capturing = false;
	s->editing = false;
	settings_damage(s);
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
	} else if (v == SETTINGS_VIEW_APPEARANCE) {
		s->sel = 0;
		s->scroll = 0;
		snprintf(s->status, sizeof(s->status),
			 "←/→: change   Enter: edit   D: reset   S: save");
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
	s->editing = false;
	comp_layer_destroy(s->panel);
	s->panel = NULL;
	config_save(&wm->config);
	vp_log("settings: close (saved)\n");
}

void settings_teardown(WM *wm)
{
	Settings *s = &wm->settings;
	comp_layer_destroy(s->panel);
	s->panel = NULL;
	comp_layer_destroy(s->icon);
	s->icon = NULL;
}

static void apply_capture(WM *wm, uint32_t id, unsigned mods)
{
	Settings *s = &wm->settings;
	char cb[32];
	keymap_format_chord(id, mods, cb, sizeof(cb));

	if (is_toggle_row(s->sel)) {
		wm->config.toggle_key =
			id; /* modifiers ignored for the toggle */
		if (config_manual_shadows_setting(&wm->config,
						  SETTING_TOGGLE_KEY)) {
			snprintf(
				s->status, sizeof(s->status),
				"Toggle set to %s - your manual config overrides it on restart",
				cb);
		} else {
			snprintf(s->status, sizeof(s->status),
				 "Toggle key set to %s", cb);
		}
	} else {
		vp_action act;
		const char *label = NULL;
		keymap_action_info(s->sel, &act, &label);
		keymap_rebind_action(&wm->config, act, id, mods);
		if (config_manual_shadows_action(&wm->config, act, id, mods)) {
			snprintf(
				s->status, sizeof(s->status),
				"%s = %s - your manual config overrides it on restart",
				label ? label : "", cb);
		} else {
			snprintf(s->status, sizeof(s->status), "%s = %s",
				 label ? label : "", cb);
		}
	}
	s->capturing = false;
	settings_damage(s);
}

static void do_unbind(WM *wm)
{
	Settings *s = &wm->settings;
	if (is_toggle_row(s->sel)) {
		snprintf(s->status, sizeof(s->status),
			 "The toggle key cannot be unbound");
	} else {
		vp_action act;
		const char *label = NULL;
		keymap_action_info(s->sel, &act, &label);
		keymap_unbind_action(&wm->config, act);
		if (config_manual_shadows_action(&wm->config, act, 0, 0)) {
			snprintf(
				s->status, sizeof(s->status),
				"%s unbound - your manual config rebinds it on restart",
				label ? label : "");
		} else {
			snprintf(s->status, sizeof(s->status), "%s unbound",
				 label ? label : "");
		}
	}
	settings_damage(s);
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
	if (v < tr->min)
		v = tr->min;
	if (v > tr->max)
		v = tr->max;
	if (v != *field) {
		*field = v;
		if (tr->on_change) {
			tr->on_change(wm);
		}
	}

	if (config_manual_shadows_setting(&wm->config, tr->setting)) {
		snprintf(s->status, sizeof(s->status),
			 "%s - your manual config overrides it on restart",
			 tr->label);
	} else if (tr->zero_label && *field == 0) {
		snprintf(s->status, sizeof(s->status), "%s = %s", tr->label,
			 tr->zero_label);
	} else {
		snprintf(s->status, sizeof(s->status), "%s = %d", tr->label,
			 *field);
	}
	settings_damage(s);
}

static void grid_move(WM *wm, int drow, int dcol)
{
	Settings *s = &wm->settings;
	int row = s->grid_sel / GRID_COLS + drow;
	int col = s->grid_sel % GRID_COLS + dcol;
	if (row < 0)
		row = 0;
	if (col < 0)
		col = 0;
	if (col > GRID_COLS - 1)
		col = GRID_COLS - 1;
	int idx = row * GRID_COLS + col;
	if (idx >= 0 && idx < GRID_ENTRY_COUNT) {
		s->grid_sel = idx;
		settings_damage(s);
	}
}

static void settings_grid_key(WM *wm, const ncinput *ni)
{
	Settings *s = &wm->settings;
	switch (ni->id) {
	case NCKEY_UP:
		grid_move(wm, -1, 0);
		break;
	case NCKEY_DOWN:
		grid_move(wm, +1, 0);
		break;
	case NCKEY_LEFT:
		grid_move(wm, 0, -1);
		break;
	case NCKEY_RIGHT:
		grid_move(wm, 0, +1);
		break;
	case NCKEY_ENTER:
	case ' ':
		if (s->grid_sel >= 0 && s->grid_sel < GRID_ENTRY_COUNT) {
			settings_set_view(wm, g_grid_entries[s->grid_sel].view);
		}
		break;
	case NCKEY_ESC:
	case 'q':
	case 'Q':
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
	case NCKEY_UP:
		if (s->sel > 0) {
			s->sel--;
			settings_damage(s);
		}
		break;
	case NCKEY_DOWN:
		if (s->sel < TERM_ROWS - 1) {
			s->sel++;
			settings_damage(s);
		}
		break;
	case NCKEY_LEFT:
	case '-':
	case '_':
		term_adjust(wm, s->sel, -1);
		break;
	case NCKEY_RIGHT:
	case '+':
	case '=':
		term_adjust(wm, s->sel, +1);
		break;
	case 's':
	case 'S':
		snprintf(s->status, sizeof(s->status),
			 config_save(&wm->config) ?
				 "Saved" :
				 "Save failed (see VP_DEBUG)");
		settings_damage(s);
		break;
	case NCKEY_ESC:
	case 'q':
	case 'Q':
		settings_back(wm);
		break;
	default:
		break;
	}
}

static const char *bg_mode_name(vp_bg_mode m)
{
	switch (m) {
	case BG_SOLID:
		return "solid";
	case BG_PATTERN:
		return "pattern";
	case BG_IMAGE:
		return "image";
	}
	return "?";
}

static const char *bg_fit_name(vp_bg_fit f)
{
	switch (f) {
	case FIT_STRETCH:
		return "stretch";
	case FIT_SCALE:
		return "scale";
	case FIT_CENTER:
		return "center";
	case FIT_TILE:
		return "tile";
	}
	return "?";
}

/* Index of the active preset (the one config.theme_name names, else default). */
static int theme_current_index(WM *wm)
{
	const char *want = wm->config.theme_name ? wm->config.theme_name :
						   vp_theme_default()->name;
	for (int i = 0; i < vp_theme_count(); i++) {
		if (strcmp(vp_theme_by_index(i)->name, want) == 0) {
			return i;
		}
	}
	return 0;
}

/* Re-apply the live theme after a change. A change made here is authoritative:
 * drop any manual-section line that would otherwise reassert itself on the next
 * load, so the menu choice wins (colors are dropped per-element by the caller). */
static void appearance_applied(WM *wm, vp_setting setting, const char *what)
{
	Settings *s = &wm->settings;
	if (setting != SETTING_COLORS) {
		config_manual_override(&wm->config, setting);
	}
	theme_apply(wm);
	snprintf(s->status, sizeof(s->status), "%s", what);
	settings_damage(s);
}

static void appearance_cycle_theme(WM *wm, int dir)
{
	int n = vp_theme_count();
	int next = ((theme_current_index(wm) + dir) % n + n) % n;
	free(wm->config.theme_name);
	wm->config.theme_name = strdup(vp_theme_by_index(next)->name);

	/* Unless the user opted to keep customizations, picking a preset is a fresh
	 * start: discard the per-color and background tweaks layered on the previous
	 * theme so the new one applies in full instead of being shadowed. */
	if (!wm->config.keep_customizations) {
		wm->config.n_color_overrides = 0;
		wm->config.bg_mode = -1;
		wm->config.bg_fit = FIT_STRETCH;
		free(wm->config.bg_glyph);
		wm->config.bg_glyph = NULL;
		free(wm->config.bg_image_path);
		wm->config.bg_image_path = NULL;
	}

	char msg[64];
	snprintf(msg, sizeof(msg), "Theme = %s", vp_theme_by_index(next)->name);
	appearance_applied(wm, SETTING_THEME, msg);
}

static void appearance_toggle_keep(WM *wm)
{
	wm->config.keep_customizations = !wm->config.keep_customizations;
	appearance_applied(wm, SETTING_KEEP_CUSTOM,
			   wm->config.keep_customizations ?
				   "Keep customizations on theme switch = yes" :
				   "Keep customizations on theme switch = no");
}

static void appearance_cycle_bg(WM *wm, int dir)
{
	int m = wm->config.bg_mode < 0 ? (int)wm->theme.bg_mode :
					 wm->config.bg_mode;
	m = ((m + dir) % 3 + 3) % 3;
	wm->config.bg_mode = m;
	char msg[64];
	snprintf(msg, sizeof(msg), "Background = %s",
		 bg_mode_name((vp_bg_mode)m));
	appearance_applied(wm, SETTING_BACKGROUND, msg);
}

static void appearance_cycle_fit(WM *wm, int dir)
{
	wm->config.bg_fit = ((wm->config.bg_fit + dir) % 4 + 4) % 4;
	char msg[64];
	snprintf(msg, sizeof(msg), "Image fit = %s",
		 bg_fit_name((vp_bg_fit)wm->config.bg_fit));
	appearance_applied(wm, SETTING_BACKGROUND, msg);
}

/* Begin in-app text entry. kind 0 = image path, 1 = color hex (color_idx). */
static void appearance_edit_start(WM *wm, int kind, int color_idx,
				  const char *initial)
{
	Settings *s = &wm->settings;
	s->editing = true;
	s->edit_kind = kind;
	s->edit_color_idx = color_idx;
	snprintf(s->input, sizeof(s->input), "%s", initial ? initial : "");
	s->input_len = (int)strlen(s->input);
	snprintf(
		s->status, sizeof(s->status),
		kind == 0 ?
			"Type an image path · Enter: apply · Esc: cancel" :
			"Type a hex color (RRGGBB) · Enter: apply · Esc: cancel");
	settings_damage(s);
}

static void appearance_edit_commit(WM *wm)
{
	Settings *s = &wm->settings;
	s->editing = false;
	if (s->edit_kind == 0) {
		free(wm->config.bg_image_path);
		wm->config.bg_image_path = s->input[0] ? strdup(s->input) :
							 NULL;
		if (s->input[0]) {
			wm->config.bg_mode = BG_IMAGE;
		}
		appearance_applied(wm, SETTING_BACKGROUND,
				   s->input[0] ? "Background image set" :
						 "Background image cleared");
		if (s->input[0] && !comp_graphics_ok(wm->comp)) {
			snprintf(
				s->status, sizeof(s->status),
				"Image set, but this terminal has no pixel support");
		}
	} else {
		const char *t = s->input;
		if (*t == '#') {
			t++;
		}
		char *end = NULL;
		unsigned long v = strtoul(t, &end, 16);
		if (end == t || *end != '\0' || v > 0xffffffUL) {
			snprintf(s->status, sizeof(s->status),
				 "Bad hex color '%.32s'", s->input);
			settings_damage(s);
			return;
		}
		config_set_color_override(&wm->config, s->edit_color_idx,
					  (vp_rgb)v);
		config_manual_override_color(&wm->config, s->edit_color_idx);
		appearance_applied(wm, SETTING_COLORS, "Color set");
	}
	settings_damage(s);
}

static void appearance_edit_key(WM *wm, const ncinput *ni)
{
	Settings *s = &wm->settings;
	if (ni->id == NCKEY_ESC) {
		s->editing = false;
		snprintf(s->status, sizeof(s->status), "Edit cancelled");
		settings_damage(s);
		return;
	}
	if (ni->id == NCKEY_ENTER) {
		appearance_edit_commit(wm);
		return;
	}
	if (ni->id == NCKEY_BACKSPACE) {
		if (s->input_len > 0) {
			s->input[--s->input_len] = '\0';
			settings_damage(s);
		}
		return;
	}
	if (ni->id >= 0x20 && ni->id < 0x7f &&
	    s->input_len < (int)sizeof(s->input) - 1) {
		s->input[s->input_len++] = (char)ni->id;
		s->input[s->input_len] = '\0';
		settings_damage(s);
	}
}

/* ←/→ on a row: cycle the fixed rows; color rows are edited via Enter. */
static void appearance_adjust(WM *wm, int dir)
{
	switch (wm->settings.sel) {
	case APP_ROW_THEME:
		appearance_cycle_theme(wm, dir);
		break;
	case APP_ROW_BG:
		appearance_cycle_bg(wm, dir);
		break;
	case APP_ROW_FIT:
		appearance_cycle_fit(wm, dir);
		break;
	case APP_ROW_KEEP:
		appearance_toggle_keep(wm);
		break;
	default:
		break;
	}
}

/* Enter on a row: cycle/toggle fixed rows; open text entry for path / color. */
static void appearance_enter(WM *wm)
{
	int sel = wm->settings.sel;
	if (sel == APP_ROW_IMAGE) {
		appearance_edit_start(wm, 0, 0, wm->config.bg_image_path);
	} else if (sel < APP_FIXED_ROWS) {
		appearance_adjust(wm, +1);
	} else {
		int idx = sel - APP_FIXED_ROWS;
		char cur[8];
		snprintf(cur, sizeof(cur), "%06x",
			 vp_theme_field_get(&wm->theme, idx));
		appearance_edit_start(wm, 1, idx, cur);
	}
}

/* D on a row: clear the image path / a color override back to the preset. */
static void appearance_clear(WM *wm)
{
	int sel = wm->settings.sel;
	if (sel == APP_ROW_IMAGE) {
		free(wm->config.bg_image_path);
		wm->config.bg_image_path = NULL;
		appearance_applied(wm, SETTING_BACKGROUND,
				   "Background image cleared");
	} else if (sel >= APP_FIXED_ROWS) {
		int idx = sel - APP_FIXED_ROWS;
		config_clear_color_override(&wm->config, idx);
		config_manual_override_color(&wm->config, idx);
		appearance_applied(wm, SETTING_COLORS, "Color reset to preset");
	}
}

static void settings_appearance_key(WM *wm, const ncinput *ni)
{
	Settings *s = &wm->settings;
	if (s->editing) {
		appearance_edit_key(wm, ni);
		return;
	}
	switch (ni->id) {
	case NCKEY_UP:
		s->sel--;
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_DOWN:
		s->sel++;
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_PGUP:
		s->sel -= viewport_rows(wm);
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_PGDOWN:
		s->sel += viewport_rows(wm);
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_HOME:
		s->sel = 0;
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_END:
		s->sel = app_total_rows() - 1;
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		break;
	case NCKEY_LEFT:
		appearance_adjust(wm, -1);
		break;
	case NCKEY_RIGHT:
		appearance_adjust(wm, +1);
		break;
	case NCKEY_ENTER:
		appearance_enter(wm);
		break;
	case 'd':
	case 'D':
	case NCKEY_DEL:
		appearance_clear(wm);
		break;
	case 's':
	case 'S':
		snprintf(s->status, sizeof(s->status),
			 config_save(&wm->config) ?
				 "Saved" :
				 "Save failed (see VP_DEBUG)");
		settings_damage(s);
		break;
	case NCKEY_ESC:
	case 'q':
	case 'Q':
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
	if (s->view == SETTINGS_VIEW_APPEARANCE) {
		settings_appearance_key(wm, ni);
		return;
	}

	if (s->capturing) {
		if (ni->id == NCKEY_ESC) {
			s->capturing = false;
			snprintf(s->status, sizeof(s->status),
				 "Rebind cancelled");
			settings_damage(s);
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
	case NCKEY_UP:
		s->sel--;
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_DOWN:
		s->sel++;
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_PGUP:
		s->sel -= viewport_rows(wm);
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_PGDOWN:
		s->sel += viewport_rows(wm);
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_HOME:
		s->sel = 0;
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_END:
		s->sel = total_rows() - 1;
		clamp_scroll(wm);
		settings_damage(s);
		break;
	case NCKEY_ENTER:
	case ' ':
		s->capturing = true;
		snprintf(s->status, sizeof(s->status),
			 "Press a key chord…  (Esc cancels)");
		settings_damage(s);
		break;
	case 'd':
	case 'D':
	case NCKEY_DEL:
	case NCKEY_BACKSPACE:
		do_unbind(wm);
		break;
	case 's':
	case 'S':
		snprintf(s->status, sizeof(s->status),
			 config_save(&wm->config) ?
				 "Saved" :
				 "Save failed (see VP_DEBUG)");
		settings_damage(s);
		break;
	case NCKEY_ESC:
	case 'q':
	case 'Q':
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
	vp_rect pr = comp_layer_abs(s->panel);
	int ay = pr.y, ax = pr.x;

	/* Click outside the panel acts like Esc: step back one level (grid → close). */
	if (y < ay || y >= ay + pr.h || x < ax || x >= ax + pr.w) {
		settings_back(wm);
		return;
	}

	int rely = y - ay;
	int relx = x - ax;

	if (s->view == SETTINGS_VIEW_GRID) {
		for (int i = 0; i < GRID_ENTRY_COUNT; i++) {
			int ty, tx;
			tile_rect(i, &ty, &tx);
			if (rely >= ty && rely < ty + TILE_H && relx >= tx &&
			    relx < tx + TILE_W) {
				s->grid_sel = i;
				settings_set_view(wm, g_grid_entries[i].view);
				return;
			}
		}
		return;
	}

	if (s->view == SETTINGS_VIEW_TERMINAL) {
		int listrow = rely - 1; /* row 0 is the top border */
		if (listrow >= 0 && listrow < TERM_ROWS) {
			s->sel = listrow;
			settings_damage(s);
		}
		return;
	}

	if (s->view == SETTINGS_VIEW_APPEARANCE) {
		if (s->editing) {
			return; /* ignore clicks while typing */
		}
		int listrow = rely - 1; /* row 0 is the top border */
		if (listrow >= 0 && listrow < viewport_rows(wm)) {
			int row = s->scroll + listrow;
			if (row >= 0 && row < app_total_rows()) {
				s->sel = row;
				settings_damage(s);
			}
		}
		return;
	}

	int listrow = rely - 1; /* row 0 is the top border */
	if (listrow >= 0 && listrow < viewport_rows(wm)) {
		int row = s->scroll + listrow;
		if (row >= 0 && row < total_rows()) {
			s->sel = row;
			s->capturing = true;
			snprintf(s->status, sizeof(s->status),
				 "Press a key chord…  (Esc cancels)");
			settings_damage(s);
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
		term_adjust(wm, s->sel,
			    dir < 0 ? +1 : -1); /* wheel up raises the value */
		return;
	}
	if (s->view == SETTINGS_VIEW_APPEARANCE) {
		if (s->editing) {
			return;
		}
		s->sel += dir;
		clamp_scroll_n(wm, app_total_rows());
		settings_damage(s);
		return;
	}
	if (s->view != SETTINGS_VIEW_KEYBINDINGS) {
		return;
	}
	s->sel += dir;
	clamp_scroll(wm);
	settings_damage(s);
}

static void draw_border(WM *wm, struct ncplane *p, int W, int H,
			const char *title)
{
	/* The frame is the first thing drawn after ncplane_erase(), which keeps
	 * the plane's channels - so set the background here rather than
	 * inheriting whatever the previous paint left behind (a theme switch
	 * would otherwise frame the panel in the old theme's panel_bg). */
	vp_setbg(p, wm->theme.panel_bg);
	vp_setfg(p, wm->theme.panel_accent);
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
	/* panel_fg, not panel_sel_fg: the title sits on panel_bg, and the
	 * selection foreground is only guaranteed to contrast with panel_sel_bg
	 * (on a light preset it is white-on-white). */
	vp_setfg(p, wm->theme.panel_fg);
	ncplane_putstr_yx(p, 0, 2, title);
}

static void draw_status(WM *wm, struct ncplane *p, int W, int H,
			const char *status)
{
	vp_setbg(p, wm->theme.panel_bg);
	vp_setfg(p, wm->theme.panel_status);
	char sbuf[128];
	snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, status);
	ncplane_putstr_yx(p, H - 2, 2, sbuf);
}

/* ----- the Control-Panel grid -------------------------------------------- */

/* One tile: rounded box, a glyph, and a caption. `populated` tiles that aren't
 * selected read as live; empty cells are drawn dim. The selected tile glows. */
static void draw_tile(WM *wm, struct ncplane *p, int ty, int tx,
		      const char *glyph, const char *label, bool populated,
		      bool selected)
{
	if (selected) {
		vp_setfg(p, wm->theme.panel_sel_fg);
		vp_setbg(p, wm->theme.panel_sel_bg);
		for (int r = 1; r < TILE_H - 1; r++)
			for (int c = 1; c < TILE_W - 1; c++)
				ncplane_putchar_yx(p, ty + r, tx + c, ' ');
	} else if (populated) {
		vp_setfg(p, wm->theme.panel_accent);
		vp_setbg(p, wm->theme.panel_bg);
	} else {
		vp_setfg(p, wm->theme.panel_hint);
		vp_setbg(p, wm->theme.panel_bg);
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
		vp_setfg(p, wm->theme.panel_sel_fg);
	} else {
		vp_setfg(p, wm->theme.panel_accent);
	}
	if (glyph) {
		ncplane_putegc_yx(p, ty + 1, tx + TILE_W / 2 - 1, glyph, NULL);
	}
	if (label) {
		int len = (int)strlen(label);
		int lx = tx + (TILE_W - len) / 2;
		if (lx < tx + 1)
			lx = tx + 1;
		ncplane_putstr_yx(p, ty + TILE_H - 2, lx, label);
	}
}

static void draw_grid(WM *wm, struct ncplane *p, int W, int H)
{
	Settings *s = &wm->settings;
	ncplane_erase(p);
	draw_border(wm, p, W, H, " Settings ");

	for (int i = 0; i < GRID_CELLS; i++) {
		int ty, tx;
		tile_rect(i, &ty, &tx);
		bool populated = i < GRID_ENTRY_COUNT;
		const char *glyph = populated ? g_grid_entries[i].glyph : NULL;
		const char *label = populated ? g_grid_entries[i].label : NULL;
		draw_tile(wm, p, ty, tx, glyph, label, populated,
			  i == s->grid_sel && populated);
	}

	draw_status(wm, p, W, H, s->status);
}

/* ----- the keybinding editor --------------------------------------------- */

static void draw_row(WM *wm, struct ncplane *p, int prow, int W,
		     const char *label, const char *chord, bool selected,
		     bool capturing)
{
	if (selected) {
		if (capturing) {
			vp_setfg(p, wm->theme.panel_cap_fg);
			vp_setbg(p, wm->theme.panel_cap_bg);
		} else {
			vp_setfg(p, wm->theme.panel_sel_fg);
			vp_setbg(p, wm->theme.panel_sel_bg);
		}
		for (int c = 1; c < W - 1; c++) {
			ncplane_putchar_yx(p, prow, c, ' ');
		}
	} else {
		vp_setfg(p, wm->theme.panel_fg);
		vp_setbg(p, wm->theme.panel_bg);
	}

	const char *shown = capturing ? "‹press a key…›" : chord;
	int chord_x = W - 2 - (int)strlen(shown);
	if (chord_x < 2)
		chord_x = 2;

	/* label, truncated so it can't run into the chord column */
	char lbuf[128];
	int maxlabel = chord_x - 2 - 1;
	if (maxlabel < 0)
		maxlabel = 0;
	if (maxlabel > (int)sizeof(lbuf) - 1)
		maxlabel = (int)sizeof(lbuf) - 1;
	snprintf(lbuf, sizeof(lbuf), "%-*.*s", maxlabel, maxlabel, label);
	ncplane_putstr_yx(p, prow, 2, lbuf);

	if (!selected) {
		vp_setfg(p,
			 wm->theme.panel_accent); /* dim accent for the chord */
	}
	ncplane_putstr_yx(p, prow, chord_x, shown);
}

static void draw_keybindings(WM *wm, struct ncplane *p, int W, int H)
{
	Settings *s = &wm->settings;
	int v = viewport_rows(wm);

	clamp_scroll(wm);
	ncplane_erase(p);
	draw_border(wm, p, W, H, " Keybindings ");

	for (int i = 0; i < v; i++) {
		int row = s->scroll + i;
		if (row >= total_rows()) {
			break;
		}
		char chord[64];
		const char *label;
		if (is_toggle_row(row)) {
			label = "Toggle INTERPRET / PASSTHROUGH";
			keymap_format_chord(wm->config.toggle_key, 0, chord,
					    sizeof(chord));
		} else {
			vp_action act;
			keymap_action_info(row, &act, &label);
			keymap_chord_for_action(&wm->config, act, chord,
						sizeof(chord));
		}
		draw_row(wm, p, 1 + i, W, label, chord, row == s->sel,
			 s->capturing && row == s->sel);
	}

	/* Status + hint lines. */
	vp_setbg(p, wm->theme.panel_bg);
	vp_setfg(p, wm->theme.panel_status);
	char sbuf[128];
	snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, s->status);
	ncplane_putstr_yx(p, H - 3, 2, sbuf);

	vp_setfg(p, wm->theme.panel_hint);
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
	draw_border(wm, p, W, H, " Terminal ");

	for (int i = 0; i < TERM_ROWS; i++) {
		const term_row *tr = &g_term_rows[i];
		int v = *term_field(wm, i);
		char val[32];
		if (tr->zero_label && v == 0) {
			snprintf(val, sizeof(val), "%s", tr->zero_label);
		} else {
			snprintf(val, sizeof(val), "%d", v);
		}
		draw_row(wm, p, 1 + i, W, tr->label, val, i == s->sel, false);
	}

	/* Status + hint lines (mirrors the keybinding editor's footer). */
	vp_setbg(p, wm->theme.panel_bg);
	vp_setfg(p, wm->theme.panel_status);
	char sbuf[128];
	snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, s->status);
	ncplane_putstr_yx(p, H - 3, 2, sbuf);

	vp_setfg(p, wm->theme.panel_hint);
	char hbuf[128];
	snprintf(hbuf, sizeof(hbuf), "%-*.*s", W - 4, W - 4,
		 "↑/↓ select · ←/→ adjust · S save · Esc back");
	ncplane_putstr_yx(p, H - 2, 2, hbuf);
}

/* ----- the Appearance view ----------------------------------------------- */

/* Fill label/value for Appearance row `row`. */
static void appearance_row_text(WM *wm, int row, char *label, size_t llen,
				char *val, size_t vlen)
{
	switch (row) {
	case APP_ROW_THEME:
		snprintf(label, llen, "Theme");
		snprintf(val, vlen, "%s",
			 wm->config.theme_name ? wm->config.theme_name :
						 vp_theme_default()->name);
		return;
	case APP_ROW_BG:
		snprintf(label, llen, "Background");
		snprintf(val, vlen, "%s%s", bg_mode_name(wm->theme.bg_mode),
			 wm->config.bg_mode < 0 ? " (theme)" : "");
		return;
	case APP_ROW_FIT:
		snprintf(label, llen, "Image fit");
		snprintf(val, vlen, "%s", bg_fit_name(wm->theme.bg_fit));
		return;
	case APP_ROW_IMAGE:
		snprintf(label, llen, "Image path");
		if (wm->config.bg_image_path) {
			snprintf(val, vlen, "%s", wm->config.bg_image_path);
		} else {
			snprintf(val, vlen, "(none)");
		}
		return;
	case APP_ROW_KEEP:
		snprintf(label, llen, "Keep tweaks on theme switch");
		snprintf(val, vlen, "%s",
			 wm->config.keep_customizations ? "yes" : "no");
		return;
	default: {
		int idx = row - APP_FIXED_ROWS;
		snprintf(label, llen, "%s", vp_theme_field_name(idx));
		snprintf(val, vlen, "%06x%s", vp_theme_field_get(&wm->theme, idx),
			 config_has_color_override(&wm->config, idx, NULL) ? " *" :
									    "");
		return;
	}
	}
}

static void draw_appearance(WM *wm, struct ncplane *p, int W, int H)
{
	Settings *s = &wm->settings;
	int v = viewport_rows(wm);

	clamp_scroll_n(wm, app_total_rows());
	ncplane_erase(p);
	draw_border(wm, p, W, H, " Appearance ");

	for (int i = 0; i < v; i++) {
		int row = s->scroll + i;
		if (row >= app_total_rows()) {
			break;
		}
		char label[64], val[160];
		appearance_row_text(wm, row, label, sizeof(label), val,
				    sizeof(val));

		/* Show the live edit buffer (with a cursor) on the row being typed. */
		bool editing_this =
			s->editing &&
			((s->edit_kind == 0 && row == APP_ROW_IMAGE) ||
			 (s->edit_kind == 1 &&
			  row == APP_FIXED_ROWS + s->edit_color_idx));
		if (editing_this) {
			/* Show the live edit buffer with a cursor. capturing=false so
			 * draw_row prints our text instead of the "press a key" prompt
			 * (that prompt is only for the keybinding editor). */
			char buf[sizeof(s->input) + 2];
			snprintf(buf, sizeof(buf), "%s_", s->input);
			draw_row(wm, p, 1 + i, W, label, buf, true, false);
		} else {
			draw_row(wm, p, 1 + i, W, label, val, row == s->sel,
				 false);
		}
	}

	/* Status + hint lines (mirrors the other editors' footer). */
	vp_setbg(p, wm->theme.panel_bg);
	vp_setfg(p, wm->theme.panel_status);
	char sbuf[160];
	snprintf(sbuf, sizeof(sbuf), "%-*.*s", W - 4, W - 4, s->status);
	ncplane_putstr_yx(p, H - 3, 2, sbuf);

	vp_setfg(p, wm->theme.panel_hint);
	char hbuf[160];
	snprintf(hbuf, sizeof(hbuf), "%-*.*s", W - 4, W - 4,
		 "↑/↓ select · ←/→ change · Enter edit · D reset · S save · Esc back");
	ncplane_putstr_yx(p, H - 2, 2, hbuf);
}

/* Painter for the modal panel. Geometry is re-derived here so a screen resize or
 * a view switch that changes the panel's size is handled by the same damage that
 * asks for the redraw. */
static void panel_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	WM *wm = user;
	Settings *s = &wm->settings;

	int py, px, H, W;
	panel_geom(wm, &py, &px, &H, &W);
	comp_layer_resize(s->panel, H, W);
	comp_layer_move(s->panel, py, px);

	uint64_t base = 0;
	vp_rgb pfg = wm->theme.panel_fg, pbg = wm->theme.panel_bg;
	ncchannels_set_fg_rgb8(&base, (pfg >> 16) & 0xff, (pfg >> 8) & 0xff,
			       pfg & 0xff);
	ncchannels_set_bg_rgb8(&base, (pbg >> 16) & 0xff, (pbg >> 8) & 0xff,
			       pbg & 0xff);
	ncplane_set_base(p, " ", 0, base);

	if (s->view == SETTINGS_VIEW_GRID) {
		draw_grid(wm, p, W, H);
	} else if (s->view == SETTINGS_VIEW_TERMINAL) {
		draw_terminal(wm, p, W, H);
	} else if (s->view == SETTINGS_VIEW_APPEARANCE) {
		draw_appearance(wm, p, W, H);
	} else {
		draw_keybindings(wm, p, W, H);
	}
}
