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

/* wm.c - window-manager core: the window list, focus, z-order/stacking,
 * layout (move/resize/min/max), spawning/closing, and the render pass.
 *
 * notcurses' plane z-order is the source of truth for stacking; we raise the
 * focused window's frame (and its bound content) to the top.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void wm_init(WM *wm, struct notcurses *nc)
{
	memset(wm, 0, sizeof(*wm));
	wm->nc = nc;
	wm->std = notcurses_stdplane(nc);
	wm->focused = -1;
	wm->next_id = 1;
	wm->mode = MODE_INTERPRET;
	wm->needs_render = true; /* force the first frame */
	wm->ptr_y = wm->ptr_x = -1; /* no software pointer placed yet */
	/* Whether this terminal can render pixel bitmaps (sixel/kitty). Gates the
     * whole sixel path; notcurses auto-detects the protocol, no init flag needed. */
	int pximpl = notcurses_check_pixel_support(nc);
	wm->pixel_ok = pximpl != NCPIXEL_NONE;
	vp_log("pixel support: impl=%d pixel_ok=%d\n", pximpl, wm->pixel_ok);
	config_load(&wm->config);
	notcurses_stddim_yx(nc, &wm->scr_rows, &wm->scr_cols);

	/* Resolve the configured theme into wm->theme and paint the desktop
	 * background. Done before any chrome is created so everything reads the
	 * active palette (the taskbar/cursor are recolored by their own creators). */
	theme_apply(wm);

	/* TERM=linux means the bare Linux console: we drive GPM ourselves (see
     * main.c) and must draw a software pointer, because gpm's own pointer is a
     * cell inversion that our full-screen repaint immediately overwrites. In a
     * GUI terminal the emulator renders the real mouse cursor for us. */
	const char *term = getenv("TERM");
	wm->console = term && strncmp(term, "linux", 5) == 0;
	wm->draw_cursor = wm->console;
	if (wm->draw_cursor) {
		ncplane_options o = { 0 };
		o.rows = 1;
		o.cols = 1;
		wm->cursor = ncplane_create(wm->std, &o);
		if (wm->cursor) {
			uint64_t cc = 0;
			vp_rgb cf = wm->theme.cursor_fg, cb = wm->theme.cursor_bg;
			ncchannels_set_fg_rgb8(&cc, (cf >> 16) & 0xff,
					       (cf >> 8) & 0xff, cf & 0xff);
			ncchannels_set_bg_rgb8(&cc, (cb >> 16) & 0xff,
					       (cb >> 8) & 0xff, cb & 0xff);
			/* A solid block is unambiguous and always present in console fonts. */
			ncplane_set_channels(wm->cursor, cc);
			ncplane_putstr_yx(wm->cursor, 0, 0, "█");
			ncplane_move_top(wm->cursor);
		} else {
			wm->draw_cursor = false;
		}
	}
}

void wm_set_mouse_pos(WM *wm, int y, int x)
{
	wm->mouse_y = y;
	wm->mouse_x = x;
}

/* ----- theming + desktop background -------------------------------------- */

/* Drop the background image plane(s). Keeps the retained visual unless `visual`
 * is set (full teardown / forcing a re-decode). */
static void bg_drop_planes(WM *wm, bool visual)
{
	for (int i = 0; i < wm->bg_nplanes; i++) {
		if (wm->bg_planes[i]) {
			ncplane_destroy(wm->bg_planes[i]);
		}
	}
	free(wm->bg_planes);
	wm->bg_planes = NULL;
	wm->bg_nplanes = 0;
	if (visual && wm->bg_visual) {
		ncvisual_destroy(wm->bg_visual);
		wm->bg_visual = NULL;
	}
}

void background_free(WM *wm)
{
	bg_drop_planes(wm, true);
}

/* Track a background plane for later teardown (z-order is handled in one shot by
 * move_family_bottom on the container, so this doesn't restack). */
static bool bg_track(WM *wm, struct ncplane *p)
{
	if (!p) {
		return false;
	}
	struct ncplane **n = realloc(
		wm->bg_planes, (size_t)(wm->bg_nplanes + 1) * sizeof(*n));
	if (!n) {
		ncplane_destroy(p);
		return false;
	}
	wm->bg_planes = n;
	wm->bg_planes[wm->bg_nplanes++] = p;
	return true;
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

/* Blit the retained visual as a pixel-bitmap child of `parent` at (y,x). */
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

/* Compose the source image tiled across the whole desktop area into one RGBA
 * buffer and blit it as a *single* pixel-bitmap plane. Tiling with many separate
 * adjacent bitmap planes cascades/overlaps under kitty backends (e.g. Konsole),
 * which only reliably composite a single sprixel behind the text planes; one
 * desktop-sized bitmap behaves exactly like the stretch/scale modes. The buffer
 * is sized to the area in cells (ah*cdy x aw*cdx px), so it never reaches the
 * last physical row. Returns the plane (child of `area`), or NULL on failure. */
static struct ncplane *bg_blit_tiled(WM *wm, struct ncplane *area, int ah, int aw)
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

	/* Pull the source out once as packed RGBA bytes. */
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

	/* Tile it into a desktop-sized buffer (repeat each source row across,
	 * and repeat rows down, both by modulo). */
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

/* Center plane `p` within a bound_h x bound_w box (its parent's area). */
static void bg_center(struct ncplane *p, int bound_h, int bound_w)
{
	unsigned ph, pw;
	ncplane_dim_yx(p, &ph, &pw);
	ncplane_move_yx(p, (bound_h - (int)ph) / 2, (bound_w - (int)pw) / 2);
}

void background_apply(WM *wm)
{
	bg_drop_planes(wm, false); /* keep any retained visual for re-blit */

	vp_bg_mode mode = wm->theme.bg_mode;

	/* An image background needs pixel support and a loadable file; otherwise
	 * fall back to a solid fill in the theme's background colors. */
	if (mode == BG_IMAGE && !wm->pixel_ok) {
		mode = BG_SOLID;
	}
	if (mode == BG_IMAGE) {
		const char *path = wm->config.bg_image_path;
		if (!path || !*path) {
			mode = BG_SOLID;
		} else {
			if (!wm->bg_visual) {
				char pbuf[1024];
				const char *load =
					expand_tilde(path, pbuf, sizeof(pbuf));
				wm->bg_visual = ncvisual_from_file(load);
				if (!wm->bg_visual) {
					vp_log("background: cannot load image %s\n",
					       load);
				}
			}
			if (!wm->bg_visual) {
				mode = BG_SOLID;
			}
		}
	}

	/* Std base cell: the desktop glyph for SOLID, a blank in the background
	 * color otherwise (so PATTERN gaps / IMAGE letterbox read as the theme bg). */
	uint64_t ch = 0;
	vp_rgb fg = wm->theme.bg_fg, bg = wm->theme.bg_bg;
	ncchannels_set_fg_rgb8(&ch, (fg >> 16) & 0xff, (fg >> 8) & 0xff,
			       fg & 0xff);
	ncchannels_set_bg_rgb8(&ch, (bg >> 16) & 0xff, (bg >> 8) & 0xff,
			       bg & 0xff);
	const char *glyph = (wm->theme.bg_glyph && *wm->theme.bg_glyph) ?
				    wm->theme.bg_glyph :
				    " ";
	ncplane_set_base(wm->std, mode == BG_SOLID ? glyph : " ", 0, ch);
	ncplane_erase(wm->std); /* clear any glyphs a previous PATTERN painted */

	if (mode == BG_PATTERN) {
		vp_setfg(wm->std, wm->theme.bg_fg);
		vp_setbg(wm->std, wm->theme.bg_bg);
		for (int r = 0; r < (int)wm->scr_rows; r++) {
			int c = 0;
			while (c < (int)wm->scr_cols) {
				int adv = ncplane_putstr_yx(wm->std, r, c, glyph);
				if (adv <= 0) {
					break;
				}
				c += adv;
			}
		}
	} else if (mode == BG_IMAGE) {
		/* Confine the image to the desktop area above the taskbar: pixel
		 * bitmaps that reach the last physical row (or hang off an edge) make
		 * some terminals scroll the whole display. The image planes are
		 * children of `area`, so STRETCH/SCALE size to it (never the last row),
		 * and CENTER/TILE only place tiles that fit fully inside it. */
		int aw = (int)wm->scr_cols;
		int ah = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
		if (ah < 1) {
			ah = (int)wm->scr_rows;
		}
		ncplane_options ao = { 0 };
		ao.rows = (unsigned)ah;
		ao.cols = (unsigned)aw;
		struct ncplane *area = ncplane_create(wm->std, &ao);
		if (area) {
			uint64_t tb = 0;
			ncchannels_set_fg_alpha(&tb, NCALPHA_TRANSPARENT);
			ncchannels_set_bg_alpha(&tb, NCALPHA_TRANSPARENT);
			ncplane_set_base(area, "", 0, tb);
			bg_track(wm, area);

			switch (wm->theme.bg_fit) {
			case FIT_STRETCH:
				bg_track(wm, bg_blit(wm, area, 0, 0,
						     NCSCALE_STRETCH));
				break;
			case FIT_SCALE: {
				struct ncplane *p =
					bg_blit(wm, area, 0, 0, NCSCALE_SCALE);
				if (p) {
					bg_center(p, ah, aw);
					bg_track(wm, p);
				}
				break;
			}
			case FIT_CENTER: {
				struct ncplane *p =
					bg_blit(wm, area, 0, 0, NCSCALE_NONE);
				if (p) {
					unsigned ph, pw;
					ncplane_dim_yx(p, &ph, &pw);
					if ((int)ph > ah || (int)pw > aw) {
						/* Too big to center without spilling onto the
						 * taskbar row: fit it instead. */
						ncplane_destroy(p);
						p = bg_blit(wm, area, 0, 0,
							    NCSCALE_SCALE);
					}
					if (p) {
						bg_center(p, ah, aw);
						bg_track(wm, p);
					}
				}
				break;
			}
			case FIT_TILE: {
				/* One desktop-sized bitmap of the tiled image,
				 * edge to edge (Windows-style). */
				struct ncplane *p =
					bg_blit_tiled(wm, area, ah, aw);
				if (!p) {
					/* Couldn't compose: stretch one copy. */
					p = bg_blit(wm, area, 0, 0,
						    NCSCALE_STRETCH);
				}
				bg_track(wm, p);
				break;
			}
			}
			/* Sink the image family to the bottom, then drop std
			 * beneath it: move_family_bottom alone would place the
			 * image *below* std, whose opaque base cell would then
			 * paint over the whole image. Final order (bottom→top):
			 * std base, image, windows, taskbar, panel. */
			ncplane_move_family_bottom(area);
			ncplane_move_bottom(wm->std);
		}
	}

	wm->needs_render = true;
}

void theme_apply(WM *wm)
{
	/* Resolve preset, then lay config overrides on top. */
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

	/* Recolor the persistent base planes that aren't redrawn per frame. */
	if (wm->taskbar) {
		uint64_t ch = 0;
		vp_rgb f = wm->theme.bar_fg, b = wm->theme.bar_bg;
		ncchannels_set_fg_rgb8(&ch, (f >> 16) & 0xff, (f >> 8) & 0xff,
				       f & 0xff);
		ncchannels_set_bg_rgb8(&ch, (b >> 16) & 0xff, (b >> 8) & 0xff,
				       b & 0xff);
		ncplane_set_base(wm->taskbar, " ", 0, ch);
	}
	if (wm->cursor) {
		uint64_t cc = 0;
		vp_rgb cf = wm->theme.cursor_fg, cb = wm->theme.cursor_bg;
		ncchannels_set_fg_rgb8(&cc, (cf >> 16) & 0xff, (cf >> 8) & 0xff,
				       cf & 0xff);
		ncchannels_set_bg_rgb8(&cc, (cb >> 16) & 0xff, (cb >> 8) & 0xff,
				       cb & 0xff);
		ncplane_set_channels(wm->cursor, cc);
		ncplane_putstr_yx(wm->cursor, 0, 0, "█");
	}
	if (wm->settings.panel) {
		uint64_t base = 0;
		vp_rgb pf = wm->theme.panel_fg, pb = wm->theme.panel_bg;
		ncchannels_set_fg_rgb8(&base, (pf >> 16) & 0xff,
				       (pf >> 8) & 0xff, pf & 0xff);
		ncchannels_set_bg_rgb8(&base, (pb >> 16) & 0xff,
				       (pb >> 8) & 0xff, pb & 0xff);
		ncplane_set_base(wm->settings.panel, " ", 0, base);
	}
	settings_icon_redraw(wm);
	exit_icon_redraw(wm);

	/* Force a re-decode of any image background (palette/path may have changed),
	 * then repaint the desktop. */
	if (wm->bg_visual) {
		ncvisual_destroy(wm->bg_visual);
		wm->bg_visual = NULL;
	}
	background_apply(wm);

	for (int i = 0; i < wm->nwins; i++) {
		wm->wins[i]->frame_dirty = true;
		wm->wins[i]->dirty = true;
	}
	wm->taskbar_dirty = true;
	if (wm->settings.open) {
		wm->settings.dirty = true;
	}
	wm->needs_render = true;
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
	wm->taskbar_dirty = true;
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
	wm->taskbar_dirty = true;

	/* Fix up the focused index. */
	if (wm->nwins == 0) {
		wm->focused = -1;
	} else if (wm->focused == idx) {
		/* focus the now-topmost remaining window */
		wm->focused = -1;
		/* pick the highest-z non-minimized window */
		for (struct ncplane *p = notcurses_top(wm->nc); p;
		     p = ncplane_below(p)) {
			Window *cand = ncplane_userptr(p);
			for (int i = 0; i < wm->nwins; i++) {
				if (wm->wins[i] == cand && !cand->minimized) {
					wm_focus_index(wm, i);
					break;
				}
			}
			if (wm->focused >= 0) {
				break;
			}
		}
		if (wm->focused < 0 && wm->nwins > 0) {
			wm_focus_index(wm, wm->nwins - 1);
		}
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
	/* Raising a frame over another window's image occludes its sprixel, which
	 * notcurses won't reliably re-emit; bracket the z-order change so every
	 * window's bitmap is dropped and cleanly re-blitted at the new stacking. */
	sixel_drop_all(wm);
	/* Raise the focused frame and its bound content to the top. */
	ncplane_move_family_top(win->frame);
	sixel_reblit_all(wm);

	if (prev && prev != win) {
		prev->frame_dirty = true;
	}
	win->frame_dirty = true;
	wm->taskbar_dirty = true;

	/* Scroll the taskbar so the newly focused window's slot is visible. */
	taskbar_reveal(wm, idx);

	/* Keep the taskbar above all windows. */
	if (wm->taskbar) {
		ncplane_move_top(wm->taskbar);
	}
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

	Window *win = window_create(wm, x, y, w, h);
	if (!win) {
		return NULL;
	}
	if (!wm_add_window(wm, win)) {
		/* Couldn't grow the window list: destroy the orphan (plane + child)
         * rather than leak it and operate on an untracked window. */
		window_destroy(wm, win);
		return NULL;
	}
	wm_clamp_onscreen(wm, win);
	wm_focus_window(wm, win);
	vp_log("spawn id=%d nwins=%d\n", win->id, wm->nwins);
	return win;
}

void wm_close_focused(WM *wm)
{
	Window *win = wm_focused(wm);
	if (win) {
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
	/* Drop any image planes first: parking the frame off-screen would drag
	 * their pixel bitmaps off-screen too, which scrolls some terminals. The
	 * visuals are kept, so restore re-blits them. */
	sixel_planes_drop(win);
	/* Park the frame off-screen (content rides along as a bound child). */
	ncplane_move_yx(win->frame, VP_HIDDEN_Y, win->x);

	if (wm_focused(wm) == win) {
		/* shift focus to the next visible window */
		wm->focused = -1;
		for (int i = 0; i < wm->nwins; i++) {
			if (!wm->wins[i]->minimized) {
				wm_focus_index(wm, i);
				break;
			}
		}
	}
	wm->taskbar_dirty = true;
}

void wm_restore(WM *wm, Window *win)
{
	if (!win || !win->minimized) {
		return;
	}
	win->minimized = false;
	vp_log("restore id=%d\n", win->id);
	ncplane_move_yx(win->frame, win->y, win->x);
	win->frame_dirty = true;
	win->dirty = true;
	wm_focus_window(wm, win);
	wm->taskbar_dirty = true;
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
	win->dirty = true;
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

	if (wm->taskbar) {
		taskbar_reflow(wm);
	}
	background_apply(wm); /* re-blit/repaint the desktop for the new size */
	settings_icon_reflow(wm); /* keep the launcher icon on-screen */
	exit_icon_reflow(wm); /* re-anchor to the new bottom-right corner */
	for (int i = 0; i < wm->nwins; i++) {
		Window *win = wm->wins[i];
		if (win->maximized) {
			int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
			window_set_geometry(win, 0, 0, (int)wm->scr_cols,
					    avail_h);
		} else if (!win->minimized) {
			wm_clamp_onscreen(wm, win);
		}
		win->dirty = true;
		win->frame_dirty = true;
	}
	wm->taskbar_dirty = true;
	if (wm->settings.open) {
		wm->settings.dirty =
			true; /* re-center/redraw the panel for the new size */
	}
}

Window *wm_window_at(WM *wm, int y, int x)
{
	/* Walk the pile from top to bottom; first frame that contains (y,x) and
     * isn't minimized wins. */
	for (struct ncplane *p = notcurses_top(wm->nc); p;
	     p = ncplane_below(p)) {
		Window *win = ncplane_userptr(p);
		if (!win) {
			continue;
		}
		/* userptr is only set on frame planes; confirm it's a live window */
		if (win->frame != p || win->minimized) {
			continue;
		}
		if (y >= win->y && y < win->y + win->h && x >= win->x &&
		    x < win->x + win->w) {
			return win;
		}
	}
	return NULL;
}

void wm_render(WM *wm)
{
	/* Track whether anything actually changed this pass; if not, we skip the
     * (expensive) notcurses_render entirely. needs_render carries signals from
     * mutations that move planes without a per-object dirty flag (dragged icons,
     * the snap outline, a closed settings panel). */
	bool drew = wm->needs_render;
	wm->needs_render = false;

	for (int i = 0; i < wm->nwins; i++) {
		Window *win = wm->wins[i];
		/* Keep titles in step with the running program / shell cwd. Done for
         * every window (not just visible ones) so the taskbar stays accurate. */
		if (window_refresh_title(win)) {
			wm->taskbar_dirty = true;
		}
		if (win->minimized) {
			continue;
		}
		if (win->frame_dirty) {
			window_draw_frame(wm, win);
			drew = true;
		}
		if (win->dirty) {
			vt_render(win);
			drew = true;
		}
	}

	if (wm->taskbar && wm->taskbar_dirty) {
		taskbar_draw(wm);
		drew = true;
	}

	if (settings_render(wm)) {
		drew = true;
	}

	/* Inner cursor only for the focused window (suppressed while the modal
     * settings editor is up). Compute the desired state, then compare against
     * what we last applied: a bare cursor move (no content damage) still needs a
     * render, while a pass that changes nothing visible can be skipped. */
	Window *f = wm_focused(wm);
	bool want_cursor = (!wm->settings.open && f && !f->minimized &&
			    f->cursor_visible && f->sb_offset == 0 &&
			    f->currow >= 0 && f->currow < f->rows &&
			    f->curcol >= 0 && f->curcol < f->cols);
	int cy = 0, cx = 0;
	if (want_cursor) {
		int ay, ax;
		ncplane_abs_yx(f->content, &ay, &ax);
		cy = ay + f->currow;
		cx = ax + f->curcol;
	}
	if (want_cursor != wm->cursor_on ||
	    (want_cursor && (cy != wm->cursor_y || cx != wm->cursor_x))) {
		drew = true;
	}

	/* Software pointer (console only): a hover move shifts the cell it sits on. */
	bool want_ptr = wm->draw_cursor && wm->cursor;
	if (want_ptr &&
	    (wm->mouse_y != wm->ptr_y || wm->mouse_x != wm->ptr_x)) {
		drew = true;
	}

	if (!drew) {
		return; /* nothing changed; don't pay for a render */
	}

	if (want_cursor) {
		notcurses_cursor_enable(wm->nc, cy, cx);
	} else {
		notcurses_cursor_disable(wm->nc);
	}
	wm->cursor_on = want_cursor;
	wm->cursor_y = cy;
	wm->cursor_x = cx;

	/* Park the software pointer over the last mouse cell, on top of everything
     * else (taskbar/settings raise themselves, so re-assert top each frame). */
	if (want_ptr) {
		ncplane_move_yx(wm->cursor, wm->mouse_y, wm->mouse_x);
		ncplane_move_top(wm->cursor);
		wm->ptr_y = wm->mouse_y;
		wm->ptr_x = wm->mouse_x;
	}

	notcurses_render(wm->nc);
}
