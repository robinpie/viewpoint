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

/* sixel.c - inline sixel graphics.
 *
 * notcurses owns the screen, so we cannot pass raw sixel bytes through to the
 * host terminal. Instead the libvterm DCS fallback (in vt_bridge.c) hands us the
 * sixel payload, we decode it with libsixel, and composite the result as a
 * notcurses pixel bitmap (NCBLIT_PIXEL) on a dedicated child plane of the
 * window's content plane.
 *
 * Each image is anchored to an absolute scrollback row (Window.scroll_base) so
 * it scrolls and persists with its text: the pixels are blitted once and only
 * the plane's position is updated as the view moves (sixel_reposition).
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <sixel.h>
#include <stdlib.h>
#include <string.h>

/* Discard a runaway DCS payload rather than buffer it without bound. */
#define SIX_BUF_CAP_MAX (16u * 1024u * 1024u)
/* Reject absurdly large decoded images (memory + the terminal would refuse a
 * bitmap this big anyway). */
#define SIX_MAX_PIXELS (8u * 1024u * 1024u)

/* ------------------------------------------------------------------------- */
/* DCS payload accumulation                                                  */
/* ------------------------------------------------------------------------- */

static void sixbuf_reset(Window *w)
{
	w->sixlen = 0;
	w->six_overflow = false;
}

static void sixbuf_append(Window *w, const char *bytes, size_t n)
{
	if (w->six_overflow || n == 0) {
		return;
	}
	if (w->sixlen + n > SIX_BUF_CAP_MAX) {
		w->six_overflow = true;
		return;
	}
	if (w->sixlen + n > w->sixcap) {
		size_t cap = w->sixcap ? w->sixcap : 4096;
		while (cap < w->sixlen + n) {
			cap *= 2;
		}
		char *nb = realloc(w->sixbuf, cap);
		if (!nb) {
			w->six_overflow = true;
			return;
		}
		w->sixbuf = nb;
		w->sixcap = cap;
	}
	memcpy(w->sixbuf + w->sixlen, bytes, n);
	w->sixlen += n;
}

/* ------------------------------------------------------------------------- */
/* Decode                                                                    */
/* ------------------------------------------------------------------------- */

/* Decode the accumulated sixel payload into a freshly-malloc'd packed RGBA
 * buffer (R,G,B,A bytes in memory, as ncvisual_from_rgba expects). Returns NULL
 * on failure; on success sets the out dimensions and the caller frees it. */
static uint32_t *sixel_to_rgba(const unsigned char *bytes, int len, int *out_w,
			       int *out_h)
{
	unsigned char *indexed = NULL, *pal = NULL;
	int iw = 0, ih = 0, ncolors = 0;
	SIXELSTATUS st = sixel_decode_raw((unsigned char *)bytes, len, &indexed,
					  &iw, &ih, &pal, &ncolors, NULL);
	if (SIXEL_FAILED(st) || !indexed || !pal || iw <= 0 || ih <= 0 ||
	    ncolors <= 0) {
		free(indexed);
		free(pal);
		return NULL;
	}
	if ((unsigned)iw * (unsigned)ih > SIX_MAX_PIXELS) {
		free(indexed);
		free(pal);
		return NULL;
	}

	uint32_t *rgba = malloc((size_t)iw * (size_t)ih * 4);
	if (!rgba) {
		free(indexed);
		free(pal);
		return NULL;
	}
	/* sixel_decode_raw yields palette-indexed pixels (1 byte each) and an RGB
	 * (3 bytes/entry) palette; expand to opaque RGBA. */
	for (size_t i = 0; i < (size_t)iw * (size_t)ih; i++) {
		unsigned idx = indexed[i];
		if ((int)idx >= ncolors) {
			idx = 0;
		}
		const unsigned char *c = pal + (size_t)idx * 3;
		unsigned char *o = (unsigned char *)&rgba[i];
		o[0] = c[0];
		o[1] = c[1];
		o[2] = c[2];
		o[3] = 0xff;
	}
	free(indexed);
	free(pal);
	*out_w = iw;
	*out_h = ih;
	return rgba;
}

/* ------------------------------------------------------------------------- */
/* Image list                                                                */
/* ------------------------------------------------------------------------- */

/* Swap-remove image i, destroying its plane. */
/* Tear down an image's notcurses resources: the (optional) visible plane and the
 * retained visual. */
static void image_free(vp_image *img)
{
	if (img->plane) {
		ncplane_destroy(img->plane);
		img->plane = NULL;
	}
	if (img->visual) {
		ncvisual_destroy(img->visual);
		img->visual = NULL;
	}
}

static void image_drop(Window *w, int i)
{
	image_free(&w->images[i]);
	w->images[i] = w->images[--w->nimages];
	if (w->wm) {
		w->wm->needs_render = true;
	}
}

void sixel_images_clear(Window *w)
{
	for (int i = 0; i < w->nimages; i++) {
		image_free(&w->images[i]);
	}
	if (w->nimages && w->wm) {
		w->wm->needs_render = true;
	}
	w->nimages = 0;
}

/* The inner app overwrote a region of the live screen (a clear, an alt-screen
 * switch, a TUI redraw). Any image whose on-screen cells intersect the damaged
 * rectangle is now stale - the text underneath is meant to show through - so
 * drop it, mirroring how a real sixel terminal discards pixels under rewritten
 * cells. Damage from scrolling arrives via moverect (a full repaint), not here,
 * so scrolled images are preserved and merely repositioned. */
void sixel_damage(Window *w, int row0, int row1, int col0, int col1)
{
	for (int i = 0; i < w->nimages;) {
		vp_image *img = &w->images[i];
		int lr0 = (int)(img->abs_row - w->scroll_base);
		int lr1 = lr0 + img->cell_h;
		bool row_hit = lr0 < row1 && row0 < lr1;
		bool col_hit =
			img->col < col1 && col0 < img->col + img->cell_w;
		if (row_hit && col_hit) {
			image_drop(w, i); /* compacts the list; don't advance i */
			continue;
		}
		i++;
	}
}

void sixel_planes_drop(Window *w)
{
	bool any = false;
	for (int i = 0; i < w->nimages; i++) {
		if (w->images[i].plane) {
			ncplane_destroy(w->images[i].plane);
			w->images[i].plane = NULL;
			any = true;
		}
	}
	if (any && w->wm) {
		w->wm->needs_render = true;
	}
}

/* A scene change (a window moved, raised, restored, …) can corrupt *other*
 * windows' bitmaps: dragging or raising a window over an image occludes its
 * sprixel, and notcurses' damage diff doesn't reliably re-emit it afterwards.
 * These two halves bracket such a change: drop every window's planes first
 * (destroying each at its current location invalidates the right cells), then
 * after the change re-blit them all, repainting the content cells the bitmaps
 * had annihilated. Splitting drop from re-blit lets the move path drop *before*
 * it slides the frame, so the moving window's own old footprint is invalidated
 * too (its planes ride the frame, so dropping after the move would target the
 * new footprint and strand the old one). */
void sixel_drop_all(WM *wm)
{
	for (int i = 0; i < wm->nwins; i++) {
		Window *w = wm->wins[i];
		if (!w->minimized && w->nimages > 0) {
			sixel_planes_drop(w);
		}
	}
}

void sixel_reblit_all(WM *wm)
{
	for (int i = 0; i < wm->nwins; i++) {
		Window *w = wm->wins[i];
		if (!w->minimized && w->nimages > 0) {
			w->dirty = true;
			w->dmg_all = true;
			sixel_reposition(w);
		}
	}
}

void sixel_window_free(Window *w)
{
	sixel_images_clear(w);
	free(w->images);
	w->images = NULL;
	w->images_cap = 0;
	free(w->sixbuf);
	w->sixbuf = NULL;
	w->sixlen = w->sixcap = 0;
	w->six_overflow = false;
}

/* ------------------------------------------------------------------------- */
/* Emit (decode + anchor)                                                    */
/* ------------------------------------------------------------------------- */

static void sixel_emit(Window *w)
{
	WM *wm = w->wm;
	if (!wm || !wm->pixel_ok || !w->content || w->six_overflow ||
	    w->sixlen == 0) {
		return;
	}

	int iw = 0, ih = 0;
	uint32_t *rgba = sixel_to_rgba((const unsigned char *)w->sixbuf,
				       (int)w->sixlen, &iw, &ih);
	if (!rgba) {
		return;
	}

	/* Anchor at the current cursor cell on the live screen (the DCS does not
	 * move the cursor, so currow/curcol still point where the image starts). */
	int currow = w->currow < 0 ? 0 : w->currow;
	int curcol = w->curcol < 0 ? 0 : w->curcol;

	/* Keep the decoded pixels in a visual so the image plane can be blitted and
	 * destroyed repeatedly as it scrolls in and out of view, without ever
	 * having to re-decode or move a bitmap off-screen. */
	struct ncvisual *v = ncvisual_from_rgba(rgba, ih, iw * 4, iw);
	free(rgba);
	if (!v) {
		return;
	}

	/* Footprint in cells, for the cursor advance and visibility tests. */
	unsigned celldimy = 0, celldimx = 0;
	ncplane_pixel_geom(w->content, NULL, NULL, &celldimy, &celldimx, NULL,
			   NULL);
	int cell_h = celldimy ? (ih + (int)celldimy - 1) / (int)celldimy : ih;
	int cell_w = celldimx ? (iw + (int)celldimx - 1) / (int)celldimx : iw;

	if (w->nimages == w->images_cap) {
		int cap = w->images_cap ? w->images_cap * 2 : 4;
		vp_image *na = realloc(w->images, (size_t)cap * sizeof(*na));
		if (!na) {
			ncvisual_destroy(v);
			return;
		}
		w->images = na;
		w->images_cap = cap;
	}
	vp_image *img = &w->images[w->nimages++];
	img->visual = v;
	img->plane = NULL; /* blitted lazily by sixel_reposition when visible */
	img->abs_row = w->scroll_base + currow;
	img->col = curcol;
	img->cell_h = cell_h;
	img->cell_w = cell_w;

	wm->needs_render = true;
	vp_log("sixel id=%d %dx%dpx (%dx%d cells) row=%d col=%d\n", w->id, iw,
	       ih, cell_w, cell_h, currow, curcol);

	/* Advance the cursor below the image so text flows underneath. The actual
	 * line feeds are deferred to vt_feed: feeding them here would re-enter the
	 * parser from inside its own DCS callback. */
	w->six_pending_lf += cell_h;
}

/* ------------------------------------------------------------------------- */
/* Public entry points                                                       */
/* ------------------------------------------------------------------------- */

void sixel_accumulate(Window *w, const char *command, size_t commandlen,
		      const char *str, size_t len, bool initial, bool final)
{
	/* No pixel support: consume and drop, never buffer. */
	if (!w->wm || !w->wm->pixel_ok) {
		return;
	}
	if (initial) {
		sixbuf_reset(w);
		/* Reconstruct what libsixel's decoder parses: the DCS introducer
		 * (ESC P) it scans for, the command bytes (Pa;Pb;Ph params and the
		 * 'q' that enters sixel mode), then the raster-data fragments. The
		 * ESC P prefix is required; the ST terminator is not, so we omit it. */
		sixbuf_append(w, "\x1bP", 2);
		sixbuf_append(w, command, commandlen);
	}
	sixbuf_append(w, str, len);
	if (final) {
		sixel_emit(w);
		sixbuf_reset(w);
	}
}

void sixel_answer_xtsmgraphics(Window *w, const long *args, int argcount)
{
	int pi = (int)
		args[0]; /* item: 1 = colour registers, 2 = graphics geometry */
	char rsp[64];
	int n = 0;

	if (pi == 1) {
		/* Number of sixel colour registers. libsixel decodes a palette of up
		 * to 256 colours, so report that (status 0 = success). */
		n = snprintf(rsp, sizeof rsp, "\x1b[?1;0;256S");
	} else if (pi == 2) {
		/* Maximum sixel image geometry in pixels: the largest bitmap the
		 * content plane can show, falling back to its current pixel size. */
		unsigned maxy = 0, maxx = 0;
		ncplane_pixel_geom(w->content, NULL, NULL, NULL, NULL, &maxy,
				   &maxx);
		if (maxx == 0 || maxy == 0) {
			ncplane_pixel_geom(w->content, &maxy, &maxx, NULL, NULL,
					   NULL, NULL);
		}
		if (maxx == 0 || maxy == 0) {
			return;
		}
		n = snprintf(rsp, sizeof rsp, "\x1b[?2;0;%u;%uS", maxx, maxy);
	} else {
		return; /* ReGIS (3) and unknown items: no reply */
	}
	if (n > 0 && n < (int)sizeof rsp) {
		vt_reply(w, rsp, (size_t)n);
	}
	(void)argcount;
}

void sixel_reposition(Window *w)
{
	if (w->nimages == 0) {
		return;
	}
	/* Absolute index shown at visible row 0, and the oldest abs index still
	 * retained in history. */
	int64_t top_abs = w->scroll_base - w->sb_offset;
	int64_t oldest = w->scroll_base - w->sb_count;

	/* Absolute screen position of the content plane, so we can keep images off
	 * the last physical row: a pixel bitmap rendered on the bottom row makes
	 * many terminals scroll the whole screen up (dragging all of viewpoint with
	 * it). scr_rows - 1 is the taskbar row, which covers the window anyway. */
	int ay = 0, ax = 0;
	ncplane_abs_yx(w->content, &ay, &ax);
	int scr_rows = w->wm ? (int)w->wm->scr_rows : w->rows;
	int scr_cols = w->wm ? (int)w->wm->scr_cols : w->cols;

	for (int i = 0; i < w->nimages;) {
		vp_image *img = &w->images[i];
		if (img->abs_row + img->cell_h <= oldest) {
			image_drop(w, i); /* scrolled out of retained history */
			continue;
		}
		int r = (int)(img->abs_row - top_abs);
		int top_screen = ay + r;
		int bot_screen = ay + r + img->cell_h; /* exclusive */
		int left_screen = ax + img->col;
		int right_screen = ax + img->col + img->cell_w; /* exclusive */
		/* Show only when the bitmap fits wholly inside the content cells and
		 * inside the screen (off the last physical row). Planes aren't
		 * scissored to the content plane, so a partial would spill over the
		 * frame/taskbar; and a pixel bitmap placed off-screen makes some
		 * terminals scroll the whole display. The column bounds matter as much
		 * as the rows: img->col is relative to the content plane, so dragging
		 * the window past the left screen edge pushes the bitmap to a negative
		 * absolute x (off-screen) even though its content-relative column never
		 * changes. So rather than move a hidden bitmap off-screen, we destroy
		 * its plane and re-blit from the retained visual when it comes back
		 * fully into view. */
		bool visible = r >= 0 && r + img->cell_h <= w->rows &&
			       img->col >= 0 && img->col + img->cell_w <= w->cols &&
			       top_screen >= 0 && bot_screen < scr_rows &&
			       left_screen >= 0 && right_screen <= scr_cols;
		if (visible) {
			if (img->plane) {
				ncplane_move_yx(img->plane, r, img->col);
			} else if (img->visual) {
				struct ncvisual_options vopts = {
					.n = w->content,
					.y = r,
					.x = img->col,
					.scaling = NCSCALE_NONE,
					.blitter = NCBLIT_PIXEL,
					.flags = NCVISUAL_OPTION_CHILDPLANE,
				};
				img->plane = ncvisual_blit(w->wm->nc,
							   img->visual, &vopts);
				/* A newly created plane lands at the top of the
				 * global z-order, not within its window's family
				 * (binding governs geometry, not stacking). Left
				 * there, a re-blitted image would float above
				 * windows stacked over it. Anchor it just above
				 * its own content plane so higher windows occlude
				 * it and move_family_top keeps it with the frame. */
				if (img->plane) {
					ncplane_move_above(img->plane,
							   w->content);
				}
				w->wm->needs_render = true;
			}
		} else if (img->plane) {
			ncplane_destroy(img->plane);
			img->plane = NULL;
			w->wm->needs_render = true;
		}
		i++;
	}
}
