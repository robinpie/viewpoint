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
 * notcurses owns the screen, so raw sixel bytes can't pass through. The
 * libvterm DCS fallback (vt_bridge.c) hands us the payload; we decode it with
 * libsixel and hand the pixels to the compositor as a graphic on the window's
 * content layer. What's left here is what the compositor doesn't know: each
 * image is anchored to an absolute scrollback row (Window.scroll_base) so it
 * scrolls with its text, and sixel_sync resolves anchors to view-relative rows.
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

/* Swap-remove image i, releasing its pixels. */
static void image_drop(Window *w, int i)
{
	comp_graphic_remove(w->images[i].gfx);
	w->images[i] = w->images[--w->nimages];
}

void sixel_images_clear(Window *w)
{
	for (int i = 0; i < w->nimages; i++) {
		comp_graphic_remove(w->images[i].gfx);
	}
	w->nimages = 0;
}

/* The inner app overwrote a region of the live screen; drop any image whose
 * on-screen cells intersect it, mirroring a real sixel terminal. Scroll damage
 * arrives via moverect instead, so scrolled images are merely repositioned. */
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
			image_drop(w,
				   i); /* compacts the list; don't advance i */
			continue;
		}
		i++;
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
	if (!wm || !comp_graphics_ok(wm->comp) || !w->content ||
	    w->six_overflow || w->sixlen == 0) {
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

	struct ncvisual *v = ncvisual_from_rgba(rgba, ih, iw * 4, iw);
	free(rgba);
	if (!v) {
		return;
	}

	/* Footprint in cells, for the cursor advance and the anchor arithmetic. */
	unsigned celldimy = 0, celldimx = 0;
	ncplane_pixel_geom(comp_layer_plane(w->content), NULL, NULL, &celldimy,
			   &celldimx, NULL, NULL);
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

	/* Hand the pixels to the compositor, anchored where the cursor is now.
	 * It retains them, so the bitmap can come and go as the view scrolls
	 * without ever being re-decoded. */
	vp_graphic *g =
		comp_graphic_add(w->content, v, currow, curcol, cell_h, cell_w);
	if (!g) {
		return; /* comp_graphic_add consumed the visual */
	}
	vp_image *img = &w->images[w->nimages++];
	img->gfx = g;
	img->abs_row = w->scroll_base + currow;
	img->col = curcol;
	img->cell_h = cell_h;
	img->cell_w = cell_w;

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
	if (!w->wm || !comp_graphics_ok(w->wm->comp)) {
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
		struct ncplane *cp = comp_layer_plane(w->content);
		ncplane_pixel_geom(cp, NULL, NULL, NULL, NULL, &maxy, &maxx);
		if (maxx == 0 || maxy == 0) {
			ncplane_pixel_geom(cp, &maxy, &maxx, NULL, NULL, NULL,
					   NULL);
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

/* Resolve every image's absolute scrollback anchor against the window's
 * current view, and evict the ones that scrolled out of retained history.
 * Whether a re-anchored bitmap is actually visible is the compositor's call. */
void sixel_sync(Window *w)
{
	if (w->nimages == 0) {
		return;
	}
	/* Absolute index shown at visible row 0, and the oldest abs index still
	 * retained in history. */
	int64_t top_abs = w->scroll_base - w->sb_offset;
	int64_t oldest = w->scroll_base - w->sb_count;

	for (int i = 0; i < w->nimages;) {
		vp_image *img = &w->images[i];
		if (img->abs_row + img->cell_h <= oldest) {
			image_drop(w, i); /* scrolled out of retained history */
			continue;
		}
		comp_graphic_move(img->gfx, (int)(img->abs_row - top_abs),
				  img->col);
		i++;
	}
}
