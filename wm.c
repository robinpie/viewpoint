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

/* wm.c - window-manager core: the window list, focus, stacking policy,
 * layout (move/resize/min/max), spawning/closing, and the per-frame update.
 * Stacking is expressed, not performed: a window is a layer in the
 * compositor's VP_BAND_WINDOW band, and focusing one just raises it there.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void desktop_paint(struct ncplane *p, bool full, void *user);
static void pointer_paint(struct ncplane *p, bool full, void *user);

void wm_init(WM *wm, struct notcurses *nc)
{
	memset(wm, 0, sizeof(*wm));
	wm->nc = nc;
	wm->focused = -1;
	wm->next_id = 1;
	wm->mode = MODE_INTERPRET;

	wm->comp = comp_create(nc);
	if (!wm->comp) {
		return; /* main() checks for this and reports it */
	}
	vp_log("pixel support: graphics=%d\n", comp_graphics_ok(wm->comp));
	config_load(&wm->config);
	notcurses_stddim_yx(nc, &wm->scr_rows, &wm->scr_cols);

	/* The desktop surface is the compositor's root layer; painting it is our
	 * job, deciding when is not. */
	comp_set_root_painter(wm->comp, desktop_paint, wm);

	/* Resolve the configured theme into wm->theme and paint the desktop
	 * background. Done before any chrome is created so everything reads the
	 * active palette (the taskbar/cursor are recolored by their own creators). */
	theme_apply(wm);

	/* TERM=linux means the bare console: we drive GPM ourselves and must draw a
	 * software pointer, since our full-screen repaint overwrites gpm's own
	 * cell-inversion cursor. GUI terminals render the real cursor for us. */
	const char *term = getenv("TERM");
	wm->console = term && strncmp(term, "linux", 5) == 0;
	wm->draw_cursor = wm->console;
	if (wm->draw_cursor) {
		vp_rect r = { 0, 0, 1, 1 };
		wm->pointer = comp_layer_new(wm->comp, VP_BAND_POINTER, r,
					     pointer_paint, wm);
		/* Deliberately not opaque. It covers one cell, and treating it
		 * as covering would make any image it passed over be torn down
		 * and re-blitted - a whole picture flickering to show a
		 * one-cell pointer that the bitmap would hide anyway. */
		comp_layer_set_opaque(wm->pointer, false);
		wm->draw_cursor = wm->pointer != NULL;
	}
}

/* The software mouse pointer: a solid block, unambiguous and present in every
 * console font. Its band keeps it over everything without anyone re-raising it. */
static void pointer_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	WM *wm = user;
	uint64_t cc = 0;
	vp_rgb cf = wm->theme.cursor_fg, cb = wm->theme.cursor_bg;
	ncchannels_set_fg_rgb8(&cc, (cf >> 16) & 0xff, (cf >> 8) & 0xff,
			       cf & 0xff);
	ncchannels_set_bg_rgb8(&cc, (cb >> 16) & 0xff, (cb >> 8) & 0xff,
			       cb & 0xff);
	ncplane_set_channels(p, cc);
	ncplane_putstr_yx(p, 0, 0, "█");
}

void wm_set_mouse_pos(WM *wm, int y, int x)
{
	wm->mouse_y = y;
	wm->mouse_x = x;
	if (wm->pointer) {
		comp_layer_move(wm->pointer, y, x);
	}
}

/* Drop the background image layer. Keeps the retained visual unless `visual` is
 * set (full teardown / forcing a re-decode). */
static void bg_drop(WM *wm, bool visual)
{
	if (wm->bg_layer) {
		comp_layer_destroy(
			wm->bg_layer); /* takes the blitted sublayer */
		wm->bg_layer = NULL;
	}
	if (visual && wm->bg_visual) {
		ncvisual_destroy(wm->bg_visual);
		wm->bg_visual = NULL;
	}
}

void background_free(WM *wm)
{
	bg_drop(wm, true);
}

/* Expand a leading "~" / "~/" to $HOME (ncvisual_from_file won't). Returns
 * `path` unchanged, or `buf` holding the expanded form. */
static const char *expand_tilde(const char *path, char *buf, size_t buflen)
{
	if (path[0] == '~' && (path[1] == '/' || path[1] == '\0')) {
		const char *home = getenv("HOME");
		if (home && *home) {
			snprintf(buf, buflen, "%s%s", home, path + 1);
			return buf;
		}
	}
	return path;
}

static struct ncplane *bg_blit(WM *wm, struct ncplane *parent, int y, int x,
			       ncscale_e scale)
{
	struct ncvisual_options o = { 0 };
	o.n = parent;
	o.y = y;
	o.x = x;
	o.scaling = scale;
	o.blitter = NCBLIT_PIXEL;
	o.flags = NCVISUAL_OPTION_CHILDPLANE;
	return ncvisual_blit(wm->nc, wm->bg_visual, &o);
}

/* Compose the source image tiled across the desktop into one RGBA buffer and
 * blit it as a single pixel-bitmap plane - many separate tiled planes cascade
 * under kitty backends (e.g. Konsole), which only reliably composite one
 * sprixel. Returns the plane (child of `area`), or NULL on failure. */
static struct ncplane *bg_blit_tiled(WM *wm, struct ncplane *area, int ah,
				     int aw)
{
	ncvgeom g;
	struct ncvisual_options probe = { .blitter = NCBLIT_PIXEL,
					  .scaling = NCSCALE_NONE };
	if (ncvisual_geom(wm->nc, wm->bg_visual, &probe, &g) != 0 ||
	    g.pixy == 0 || g.pixx == 0 || g.cdimy == 0 || g.cdimx == 0) {
		return NULL;
	}
	int sh = (int)g.pixy, sw = (int)g.pixx;
	int dh = ah * (int)g.cdimy, dw = aw * (int)g.cdimx;
	if (dh <= 0 || dw <= 0) {
		return NULL;
	}

	size_t srow = (size_t)sw * 4;
	unsigned char *src = malloc((size_t)sh * srow);
	if (!src) {
		return NULL;
	}
	for (int y = 0; y < sh; y++) {
		for (int x = 0; x < sw; x++) {
			uint32_t px = 0;
			ncvisual_at_yx(wm->bg_visual, (unsigned)y, (unsigned)x,
				       &px);
			unsigned char *d = src + (size_t)y * srow + (size_t)x * 4;
			d[0] = ncpixel_r(px);
			d[1] = ncpixel_g(px);
			d[2] = ncpixel_b(px);
			d[3] = ncpixel_a(px);
		}
	}

	size_t drow = (size_t)dw * 4;
	unsigned char *dst = malloc((size_t)dh * drow);
	if (!dst) {
		free(src);
		return NULL;
	}
	for (int y = 0; y < dh; y++) {
		unsigned char *s = src + (size_t)(y % sh) * srow;
		unsigned char *d = dst + (size_t)y * drow;
		size_t filled = 0;
		while (filled < drow) {
			size_t chunk = srow;
			if (chunk > drow - filled) {
				chunk = drow - filled;
			}
			memcpy(d + filled, s, chunk);
			filled += chunk;
		}
	}
	free(src);

	struct ncvisual *tiled = ncvisual_from_rgba(dst, dh, (int)drow, dw);
	free(dst);
	if (!tiled) {
		return NULL;
	}
	struct ncvisual_options o = { .n = area,
				      .scaling = NCSCALE_NONE,
				      .blitter = NCBLIT_PIXEL,
				      .flags = NCVISUAL_OPTION_CHILDPLANE };
	struct ncplane *p = ncvisual_blit(wm->nc, tiled, &o);
	ncvisual_destroy(tiled);
	return p;
}

static void bg_center(struct ncplane *p, int bound_h, int bound_w)
{
	unsigned ph, pw;
	ncplane_dim_yx(p, &ph, &pw);
	ncplane_move_yx(p, (bound_h - (int)ph) / 2, (bound_w - (int)pw) / 2);
}

/* The background mode we can actually honour: an image needs pixel support and
 * a loadable file, falling back to a solid fill without them. */
static vp_bg_mode bg_resolve_mode(WM *wm)
{
	vp_bg_mode mode = wm->theme.bg_mode;
	if (mode != BG_IMAGE) {
		return mode;
	}
	if (!comp_graphics_ok(wm->comp)) {
		return BG_SOLID;
	}
	const char *path = wm->config.bg_image_path;
	if (!path || !*path) {
		return BG_SOLID;
	}
	if (!wm->bg_visual) {
		char pbuf[1024];
		const char *load = expand_tilde(path, pbuf, sizeof(pbuf));
		wm->bg_visual = ncvisual_from_file(load);
		if (!wm->bg_visual) {
			vp_log("background: cannot load image %s\n", load);
		}
	}
	return wm->bg_visual ? BG_IMAGE : BG_SOLID;
}

/* Painter for the desktop surface (the compositor's root layer). The base
 * cell carries the desktop glyph for SOLID, a blank otherwise, so PATTERN
 * gaps and IMAGE letterboxing read as the theme background. */
static void desktop_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	WM *wm = user;
	const char *glyph = (wm->theme.bg_glyph && *wm->theme.bg_glyph) ?
				    wm->theme.bg_glyph :
				    " ";
	uint64_t ch = 0;
	vp_rgb fg = wm->theme.bg_fg, bg = wm->theme.bg_bg;
	ncchannels_set_fg_rgb8(&ch, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
			       fg & 0xff);
	ncchannels_set_bg_rgb8(&ch, (bg >> 16) & 0xff, (bg >> 8) & 0xff,
			       bg & 0xff);
	ncplane_set_base(p, wm->bg_mode == BG_SOLID ? glyph : " ", 0, ch);
	ncplane_erase(p); /* clear any glyphs a previous PATTERN painted */

	if (wm->bg_mode != BG_PATTERN) {
		return;
	}
	vp_setfg(p, wm->theme.bg_fg);
	vp_setbg(p, wm->theme.bg_bg);
	for (int r = 0; r < (int)wm->scr_rows; r++) {
		int c = 0;
		while (c < (int)wm->scr_cols) {
			int adv = ncplane_putstr_yx(p, r, c, glyph);
			if (adv <= 0) {
				break;
			}
			c += adv;
		}
	}
}

/* (Re)build the desktop background. For BG_IMAGE this creates one backdrop-band
 * layer covering the desktop with the blitted bitmap as a sublayer, so a
 * single destroy takes it all down and the band keeps it under every window. */
void background_apply(WM *wm)
{
	bg_drop(wm, false); /* keep any retained visual for re-blit */
	wm->bg_mode = bg_resolve_mode(wm);
	comp_layer_damage_full(comp_root_layer(wm->comp));

	if (wm->bg_mode != BG_IMAGE) {
		return;
	}

	/* Confine the image to the desktop area above the taskbar: a pixel bitmap
	 * that reaches the last physical row (or hangs off an edge) makes some
	 * terminals scroll the whole display. The bitmap is a child of `area`, so
	 * STRETCH/SCALE size to it and CENTER/TILE stay inside it. */
	int aw = (int)wm->scr_cols;
	int ah = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
	if (ah < 1) {
		ah = (int)wm->scr_rows;
	}
	vp_rect ar = { 0, 0, ah, aw };
	wm->bg_layer = comp_layer_new(wm->comp, VP_BAND_BACKDROP, ar, NULL, wm);
	if (!wm->bg_layer) {
		return;
	}
	struct ncplane *area = comp_layer_plane(wm->bg_layer);
	uint64_t tb = 0;
	ncchannels_set_fg_alpha(&tb, NCALPHA_TRANSPARENT);
	ncchannels_set_bg_alpha(&tb, NCALPHA_TRANSPARENT);
	ncplane_set_base(area, "", 0, tb);

	struct ncplane *img = NULL;
	switch (wm->theme.bg_fit) {
	case FIT_STRETCH:
		img = bg_blit(wm, area, 0, 0, NCSCALE_STRETCH);
		break;
	case FIT_SCALE:
		img = bg_blit(wm, area, 0, 0, NCSCALE_SCALE);
		if (img) {
			bg_center(img, ah, aw);
		}
		break;
	case FIT_CENTER: {
		img = bg_blit(wm, area, 0, 0, NCSCALE_NONE);
		if (img) {
			unsigned ph, pw;
			ncplane_dim_yx(img, &ph, &pw);
			if ((int)ph > ah || (int)pw > aw) {
				/* Too big to centre without spilling onto the
				 * taskbar row: fit it instead. */
				ncplane_destroy(img);
				img = bg_blit(wm, area, 0, 0, NCSCALE_SCALE);
			}
		}
		if (img) {
			bg_center(img, ah, aw);
		}
		break;
	}
	case FIT_TILE:
		/* One desktop-sized bitmap of the tiled image, edge to edge. */
		img = bg_blit_tiled(wm, area, ah, aw);
		if (!img) {
			img = bg_blit(wm, area, 0, 0, NCSCALE_STRETCH);
		}
		break;
	}
	if (img) {
		comp_sublayer_adopt(wm->bg_layer, img, wm);
	}
}

void theme_apply(WM *wm)
{
	const VpTheme *preset = vp_theme_builtin(wm->config.theme_name);
	wm->theme = preset ? *preset : *vp_theme_default();

	if (wm->config.bg_mode >= 0) {
		wm->theme.bg_mode = (vp_bg_mode)wm->config.bg_mode;
	}
	if (wm->config.bg_fit >= 0) {
		wm->theme.bg_fit = (vp_bg_fit)wm->config.bg_fit;
	}
	if (wm->config.bg_glyph) {
		wm->theme.bg_glyph = wm->config.bg_glyph;
	}
	for (int i = 0; i < wm->config.n_color_overrides; i++) {
		vp_theme_field_set(&wm->theme,
				   wm->config.color_overrides[i].field_idx,
				   wm->config.color_overrides[i].color);
	}

	/* Everything below is just "this now looks different": each surface is
	 * marked for repaint and the compositor re-runs its painter, which reads
	 * the new palette. */
	taskbar_damage(wm);
	comp_layer_damage(wm->pointer);
	settings_damage(&wm->settings);
	settings_icon_redraw(wm);
	exit_icon_redraw(wm);
	die_icon_redraw(wm);
	launcher_icons_redraw(wm);

	/* Force a re-decode of any image background (palette/path may have changed),
	 * then repaint the desktop. */
	if (wm->bg_visual) {
		ncvisual_destroy(wm->bg_visual);
		wm->bg_visual = NULL;
	}
	background_apply(wm);

	for (int i = 0; i < wm->nwins; i++) {
		window_damage_frame(wm->wins[i]);
		window_damage_content(wm->wins[i], true);
	}
}

bool wm_add_window(WM *wm, Window *win)
{
	if (wm->nwins == wm->cap) {
		int ncap = wm->cap ? wm->cap * 2 : 8;
		Window **n = realloc(wm->wins, (size_t)ncap * sizeof(*n));
		if (!n) {
			return false; /* caller owns `win` and must tear it down */
		}
		wm->wins = n;
		wm->cap = ncap;
	}
	wm->wins[wm->nwins++] = win;
	taskbar_damage(wm);
	return true;
}

void wm_remove_window(WM *wm, Window *win)
{
	int idx = wm_index_of(wm, win);
	if (idx < 0) {
		return;
	}
	for (int i = idx; i < wm->nwins - 1; i++) {
		wm->wins[i] = wm->wins[i + 1];
	}
	wm->nwins--;
	taskbar_damage(wm);

	if (wm->nwins == 0) {
		wm->focused = -1;
	} else if (wm->focused == idx) {
		/* Focus falls to whichever visible window is frontmost. The
		 * compositor's stacking key answers that directly - no walking
		 * the terminal's plane list looking for one we recognise. */
		wm->focused = -1;
		int best = -1, best_order = 0;
		for (int i = 0; i < wm->nwins; i++) {
			if (wm->wins[i]->minimized) {
				continue;
			}
			int o = comp_layer_order(wm->wins[i]->frame);
			if (best < 0 || o > best_order) {
				best = i;
				best_order = o;
			}
		}
		wm_focus_index(wm, best >= 0 ? best : wm->nwins - 1);
	} else if (wm->focused > idx) {
		wm->focused--;
	}
}

int wm_index_of(WM *wm, const Window *win)
{
	for (int i = 0; i < wm->nwins; i++) {
		if (wm->wins[i] == win) {
			return i;
		}
	}
	return -1;
}

Window *wm_focused(WM *wm)
{
	if (wm->focused < 0 || wm->focused >= wm->nwins) {
		return NULL;
	}
	return wm->wins[wm->focused];
}

void wm_focus_index(WM *wm, int idx)
{
	if (idx < 0 || idx >= wm->nwins) {
		return;
	}
	Window *prev = wm_focused(wm);
	Window *win = wm->wins[idx];

	wm->focused = idx;
	vp_log("focus id=%d idx=%d\n", win->id, idx);
	/* Frontmost within the window band - which the band itself keeps below
	 * the taskbar and the modal panel. Any image the raise now covers, or
	 * uncovers, is the compositor's to sort out. */
	comp_layer_raise(win->frame);

	if (prev && prev != win) {
		window_damage_frame(prev);
	}
	window_damage_frame(win);
	taskbar_damage(wm);
	taskbar_reveal(wm, idx);
}

void wm_focus_window(WM *wm, Window *win)
{
	int idx = wm_index_of(wm, win);
	if (idx >= 0) {
		wm_focus_index(wm, idx);
	}
}

void wm_focus_next(WM *wm, int dir)
{
	if (wm->nwins == 0) {
		return;
	}
	int start = wm->focused < 0 ? 0 : wm->focused;
	for (int step = 1; step <= wm->nwins; step++) {
		int i = ((start + dir * step) % wm->nwins + wm->nwins) %
			wm->nwins;
		if (!wm->wins[i]->minimized) {
			wm_focus_index(wm, i);
			return;
		}
	}
}

Window *wm_spawn_window(WM *wm)
{
	return wm_spawn_command(wm, NULL);
}

Window *wm_spawn_command(WM *wm, const char *cmd)
{
	/* Cascade new windows from the top-left, but clear of the desktop settings
     * icon (which sits in roughly the leftmost ~13 columns). */
	int n = wm->nwins;
	int x = 16 + (n % 6) * 4;
	int y = 1 + (n % 6) * 2;
	int w = (int)wm->scr_cols * 2 / 3;
	int h = (int)wm->scr_rows * 2 / 3;
	if (w < VP_MIN_W * 2)
		w = (int)wm->scr_cols - x - 1;
	if (h < VP_MIN_H * 2)
		h = (int)wm->scr_rows - y - 1;

	if (!session_request_new_cmd(wm, h - 2 * VP_BORDER, w - 2 * VP_BORDER,
				     cmd)) {
		return NULL;
	}
	session_drain(wm);
	return wm_focused(wm);
}

void wm_close_focused(WM *wm)
{
	Window *win = wm_focused(wm);
	if (win) {
		session_close(win);
		win->dead = true; /* destroyed at end of the loop pass */
		vp_log("close id=%d\n", win->id);
	}
}

void wm_minimize(WM *wm, Window *win)
{
	if (!win || win->minimized) {
		return;
	}
	win->minimized = true;
	vp_log("minimize id=%d\n", win->id);
	/* Hidden is a state, not a position: the window keeps its geometry, and
	 * the compositor tears its bitmaps down rather than dragging them
	 * off-screen (which scrolls some terminals). */
	comp_layer_show(win->frame, false);

	if (wm_focused(wm) == win) {
		wm->focused = -1;
		for (int i = 0; i < wm->nwins; i++) {
			if (!wm->wins[i]->minimized) {
				wm_focus_index(wm, i);
				break;
			}
		}
	}
	taskbar_damage(wm);
}

void wm_restore(WM *wm, Window *win)
{
	if (!win || !win->minimized) {
		return;
	}
	win->minimized = false;
	vp_log("restore id=%d\n", win->id);
	comp_layer_show(win->frame, true);
	window_damage_frame(win);
	window_damage_content(win, false);
	wm_focus_window(wm, win);
	taskbar_damage(wm);
}

void wm_toggle_maximize(WM *wm, Window *win)
{
	if (!win) {
		return;
	}
	int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
	vp_log("maxtoggle id=%d was_max=%d\n", win->id, win->maximized);
	if (!win->maximized) {
		win->sx = win->x;
		win->sy = win->y;
		win->sw = win->w;
		win->sh = win->h;
		win->maximized = true;
		window_set_geometry(win, 0, 0, (int)wm->scr_cols, avail_h);
	} else {
		win->maximized = false;
		window_set_geometry(win, win->sx, win->sy, win->sw, win->sh);
	}
	window_damage_content(win, false);
}

void wm_move_focused(WM *wm, int dx, int dy)
{
	Window *win = wm_focused(wm);
	if (!win || win->minimized) {
		return;
	}
	if (win->maximized) {
		win->maximized = false; /* moving un-maximizes */
	}
	window_set_geometry(win, win->x + dx, win->y + dy, win->w, win->h);
	wm_clamp_onscreen(wm, win);
}

void wm_resize_focused(WM *wm, int dw, int dh)
{
	Window *win = wm_focused(wm);
	if (!win || win->minimized) {
		return;
	}
	if (win->maximized) {
		win->maximized = false;
	}
	window_set_geometry(win, win->x, win->y, win->w + dw, win->h + dh);
	wm_clamp_onscreen(wm, win);
}

void wm_clamp_onscreen(WM *wm, Window *win)
{
	int maxw = (int)wm->scr_cols;
	int maxh = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);

	int w = win->w, h = win->h;
	if (w > maxw)
		w = maxw;
	if (h > maxh)
		h = maxh;

	int x = win->x, y = win->y;
	if (x + w > maxw)
		x = maxw - w;
	if (y + h > maxh)
		y = maxh - h;
	if (x < 0)
		x = 0;
	if (y < 0)
		y = 0;

	if (x != win->x || y != win->y || w != win->w || h != win->h) {
		window_set_geometry(win, x, y, w, h);
	}
}

void wm_handle_resize(WM *wm)
{
	notcurses_refresh(wm->nc, NULL, NULL);
	notcurses_stddim_yx(wm->nc, &wm->scr_rows, &wm->scr_cols);
	comp_resize(wm->comp);

	if (wm->taskbar) {
		taskbar_reflow(wm);
	}
	background_apply(wm);
	settings_icon_reflow(wm);
	exit_icon_reflow(wm);
	die_icon_reflow(wm);
	launcher_icons_reflow(wm);
	for (int i = 0; i < wm->nwins; i++) {
		Window *win = wm->wins[i];
		if (win->maximized) {
			int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
			window_set_geometry(win, 0, 0, (int)wm->scr_cols,
					    avail_h);
		} else if (!win->minimized) {
			wm_clamp_onscreen(wm, win);
		}
		window_damage_content(win, false);
		window_damage_frame(win);
	}
	taskbar_damage(wm);
	if (wm->settings.open) {
		settings_damage(&wm->settings);
	}
}

Window *wm_window_at(WM *wm, int y, int x)
{
	/* Hit testing is a scene question, so the scene answers it: the
	 * compositor knows the true stacking and which windows are hidden. */
	vp_layer *l = comp_layer_at(wm->comp, VP_BAND_WINDOW, y, x);
	return l ? comp_layer_user(l) : NULL;
}

/* Where the hardware text cursor should be: inside the focused window's grid,
 * on the live screen, and not while the modal settings editor has the input. */
static void update_text_cursor(WM *wm)
{
	Window *f = wm_focused(wm);
	bool on = (!wm->settings.open && f && !f->minimized &&
		   f->cursor_visible && f->sb_offset == 0 && f->currow >= 0 &&
		   f->currow < f->rows && f->curcol >= 0 &&
		   f->curcol < f->cols);
	if (!on) {
		comp_set_cursor(wm->comp, false, 0, 0);
		return;
	}
	vp_rect r = comp_layer_abs(f->content);
	comp_set_cursor(wm->comp, true, r.y + f->currow, r.x + f->curcol);
}

/* One pass of the event loop's display half: state intent (titles, cursor)
 * and let the compositor decide what repainting it actually costs. */
void wm_render(WM *wm)
{
	for (int i = 0; i < wm->nwins; i++) {
		/* Keep titles in step with the running program / shell cwd. Done
		 * for every window (not just visible ones) so the taskbar stays
		 * accurate. */
		if (window_refresh_title(wm->wins[i])) {
			taskbar_damage(wm);
		}
	}
	update_text_cursor(wm);
	comp_frame(wm->comp);
}
