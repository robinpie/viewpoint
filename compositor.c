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

/* compositor.c - scene graph, stacking, pixel graphics and presentation.
 *
 * A flat array of layers, each carrying the band/order/sequence that fixes
 * its place in the stack. Any scene-changing mutation bumps a revision
 * counter; comp_frame diffs it against what the stack/graphics last matched
 * and does only the work implied. See compositor.h for the contract.
 */
#define _GNU_SOURCE
#include "compositor.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>

/* Row a hidden layer's plane is parked on. notcurses has no per-plane
 * visibility, so "hidden" is implemented by moving the plane far below the
 * screen - but only ever in here, and never as something a caller can observe:
 * comp_layer_rect/abs keep reporting the layer's logical geometry. */
#define COMP_PARK_Y 100000

struct vp_layer {
	vp_comp *comp;
	struct ncplane *plane;
	vp_layer *parent; /* NULL for a top-level layer */

	vp_band band; /* meaningful on a top-level layer */
	int order; /* stacking key within the band */
	uint32_t seq; /* creation sequence: stable tiebreak */

	vp_rect rect; /* absolute, or parent-relative for a sublayer */
	bool visible;
	bool opaque;
	bool damaged;
	bool damaged_full;

	bool is_root; /* the standard plane: never moved, never destroyed */

	vp_paint_fn paint;
	void *user;

	int sidx; /* index in comp->sorted, refreshed by comp_sort */
};

struct vp_graphic {
	vp_comp *comp;
	vp_layer *owner;
	struct ncvisual *visual; /* retained so the bitmap can be re-blitted */
	struct ncplane *plane; /* live sprixel, or NULL while not shown */

	int y, x; /* owner-relative cell anchor */
	int h, w; /* footprint in cells */

	/* What is actually on screen, for diffing against what should be. Compared
	 * in owner-local coords, since the bitmap rides its owner's plane for
	 * free; re-emitting one that merely rode along wastes work and can leave
	 * smears trailing behind a dragged window. */
	bool shown;
	int shown_y, shown_x; /* owner-local anchor of the live bitmap */
	vp_rect shown_abs; /* where it currently sits on screen */
	uint32_t shown_order; /* stacking revision it was blitted under */
};

struct vp_comp {
	struct notcurses *nc;
	struct ncplane *std;
	int rows, cols;
	bool pixel_ok;

	vp_layer **layers;
	int nlayers, layers_cap;
	vp_layer **sorted; /* layers in ascending z; rebuilt on demand */
	int nsorted;
	uint32_t sorted_rev;
	vp_layer **applied; /* the order last pushed to notcurses */
	int napplied;

	vp_graphic **gfx;
	int ngfx, gfx_cap;
	bool gfx_dirty;

	/* One counter behind every "does the screen still match the model?"
	 * question. Bumped by geometry, visibility, stacking and layer/graphic
	 * lifetime changes; compared against the revisions the sorted array, the
	 * plane stack and the graphics were last reconciled at. */
	uint32_t rev;
	uint32_t stack_rev; /* rev the plane stack was last reconciled at */
	uint32_t gfx_rev; /* rev the graphics were last reconciled at */
	/* Bumped only when a reconcile actually *changed* the plane order.
	 * Moving a window changes the scene every frame of a drag but almost
	 * never changes who is in front of whom, and only the latter forces a
	 * bitmap to be re-emitted. */
	uint32_t order_rev;
	uint32_t next_seq;
	int band_order[VP_BAND_COUNT];

	bool cursor_on; /* requested */
	int cursor_y, cursor_x;
	bool applied_on; /* last handed to notcurses */
	int applied_y, applied_x;

	bool force; /* present next frame regardless */
	vp_layer *root;
};

/* ------------------------------------------------------------------------- */
/* Small geometry helpers                                                    */
/* ------------------------------------------------------------------------- */

static bool rect_overlap(vp_rect a, vp_rect b)
{
	return a.y < b.y + b.h && b.y < a.y + a.h && a.x < b.x + b.w &&
	       b.x < a.x + a.w;
}

/* Does `a` wholly contain `b`? */
static bool rect_covers(vp_rect a, vp_rect b)
{
	return b.y >= a.y && b.x >= a.x && b.y + b.h <= a.y + a.h &&
	       b.x + b.w <= a.x + a.w;
}

static bool rect_contains(vp_rect r, int y, int x)
{
	return y >= r.y && y < r.y + r.h && x >= r.x && x < r.x + r.w;
}

static vp_layer *layer_root_of(vp_layer *l)
{
	while (l->parent) {
		l = l->parent;
	}
	return l;
}

static int layer_depth(const vp_layer *l)
{
	int d = 0;
	for (const vp_layer *p = l->parent; p; p = p->parent) {
		d++;
	}
	return d;
}

/* A layer is only really on screen if it and every ancestor is shown. */
static bool layer_shown(const vp_layer *l)
{
	for (const vp_layer *p = l; p; p = p->parent) {
		if (!p->visible) {
			return false;
		}
	}
	return true;
}

vp_rect comp_layer_abs(const vp_layer *l)
{
	vp_rect r = l->rect;
	for (const vp_layer *p = l->parent; p; p = p->parent) {
		r.y += p->rect.y;
		r.x += p->rect.x;
	}
	return r;
}

/* ------------------------------------------------------------------------- */
/* Revision bookkeeping                                                      */
/* ------------------------------------------------------------------------- */

static void comp_touch(vp_comp *c)
{
	c->rev++;
}

void comp_invalidate(vp_comp *c)
{
	c->force = true;
}

/* ------------------------------------------------------------------------- */
/* Sorting                                                                   */
/* ------------------------------------------------------------------------- */

/* Ascending z. Top-level layers order by (band, order, sequence); a sublayer
 * inherits its root's key and then sorts after it by depth, so a family always
 * occupies one contiguous run of the stack. */
static int cmp_layers(const void *pa, const void *pb)
{
	vp_layer *a = *(vp_layer *const *)pa;
	vp_layer *b = *(vp_layer *const *)pb;
	vp_layer *ra = layer_root_of(a);
	vp_layer *rb = layer_root_of(b);

	if (ra != rb) {
		if (ra->band != rb->band) {
			return ra->band < rb->band ? -1 : 1;
		}
		if (ra->order != rb->order) {
			return ra->order < rb->order ? -1 : 1;
		}
		return ra->seq < rb->seq ? -1 : 1;
	}
	int da = layer_depth(a), db = layer_depth(b);
	if (da != db) {
		return da < db ? -1 : 1;
	}
	if (a->seq != b->seq) {
		return a->seq < b->seq ? -1 : 1;
	}
	return 0;
}

static void comp_sort(vp_comp *c)
{
	if (c->sorted_rev == c->rev && c->nsorted == c->nlayers) {
		return;
	}
	if (c->nlayers > 0) {
		memcpy(c->sorted, c->layers,
		       (size_t)c->nlayers * sizeof(*c->sorted));
		qsort(c->sorted, (size_t)c->nlayers, sizeof(*c->sorted),
		      cmp_layers);
	}
	c->nsorted = c->nlayers;
	for (int i = 0; i < c->nsorted; i++) {
		c->sorted[i]->sidx = i;
	}
	c->sorted_rev = c->rev;
}

/* ------------------------------------------------------------------------- */
/* Layer registry                                                            */
/* ------------------------------------------------------------------------- */

static bool comp_track(vp_comp *c, vp_layer *l)
{
	if (c->nlayers == c->layers_cap) {
		int cap = c->layers_cap ? c->layers_cap * 2 : 16;
		vp_layer **nl = realloc(c->layers, (size_t)cap * sizeof(*nl));
		if (!nl) {
			return false;
		}
		c->layers = nl;
		vp_layer **ns = realloc(c->sorted, (size_t)cap * sizeof(*ns));
		if (!ns) {
			return false;
		}
		c->sorted = ns;
		vp_layer **na = realloc(c->applied, (size_t)cap * sizeof(*na));
		if (!na) {
			return false;
		}
		c->applied = na;
		c->layers_cap = cap;
	}
	c->layers[c->nlayers++] = l;
	comp_touch(c);
	return true;
}

static void comp_untrack(vp_comp *c, vp_layer *l)
{
	for (int i = 0; i < c->nlayers; i++) {
		if (c->layers[i] == l) {
			for (int j = i; j < c->nlayers - 1; j++) {
				c->layers[j] = c->layers[j + 1];
			}
			c->nlayers--;
			comp_touch(c);
			return;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

vp_comp *comp_create(struct notcurses *nc)
{
	vp_comp *c = calloc(1, sizeof(*c));
	if (!c) {
		return NULL;
	}
	c->nc = nc;
	c->std = notcurses_stdplane(nc);
	unsigned r = 0, cl = 0;
	notcurses_stddim_yx(nc, &r, &cl);
	c->rows = (int)r;
	c->cols = (int)cl;
	c->pixel_ok = notcurses_check_pixel_support(nc) != NCPIXEL_NONE;

	vp_layer *root = calloc(1, sizeof(*root));
	if (!root) {
		free(c);
		return NULL;
	}
	root->comp = c;
	root->plane = c->std;
	root->band = VP_BAND_BACKDROP;
	root->order = INT_MIN;
	root->seq = c->next_seq++;
	root->rect = (vp_rect){ 0, 0, c->rows, c->cols };
	root->visible = true;
	root->opaque = true;
	root->is_root = true;
	if (!comp_track(c, root)) {
		free(root);
		free(c->layers);
		free(c->sorted);
		free(c->applied);
		free(c);
		return NULL;
	}
	c->root = root;
	c->force = true;
	return c;
}

void comp_destroy(vp_comp *c)
{
	if (!c) {
		return;
	}
	for (int i = 0; i < c->ngfx; i++) {
		if (c->gfx[i]->plane) {
			ncplane_destroy(c->gfx[i]->plane);
		}
		if (c->gfx[i]->visual) {
			ncvisual_destroy(c->gfx[i]->visual);
		}
		free(c->gfx[i]);
	}
	free(c->gfx);
	for (int i = 0; i < c->nlayers; i++) {
		vp_layer *l = c->layers[i];
		if (!l->is_root && l->plane) {
			ncplane_destroy(l->plane);
		}
		free(l);
	}
	free(c->layers);
	free(c->sorted);
	free(c->applied);
	free(c);
}

void comp_resize(vp_comp *c)
{
	unsigned r = 0, cl = 0;
	notcurses_stddim_yx(c->nc, &r, &cl);
	c->rows = (int)r;
	c->cols = (int)cl;
	c->root->rect = (vp_rect){ 0, 0, c->rows, c->cols };
	comp_touch(c);
	comp_invalidate(c);
}

struct notcurses *comp_nc(const vp_comp *c)
{
	return c->nc;
}

int comp_rows(const vp_comp *c)
{
	return c->rows;
}

int comp_cols(const vp_comp *c)
{
	return c->cols;
}

bool comp_graphics_ok(const vp_comp *c)
{
	return c->pixel_ok;
}

vp_layer *comp_root_layer(vp_comp *c)
{
	return c->root;
}

void comp_set_root_painter(vp_comp *c, vp_paint_fn paint, void *user)
{
	c->root->paint = paint;
	c->root->user = user;
	comp_layer_damage_full(c->root);
}

/* ------------------------------------------------------------------------- */
/* Layers                                                                    */
/* ------------------------------------------------------------------------- */

static bool gfx_visible(vp_comp *c, vp_graphic *g, vp_rect *out);
static void damage_under(vp_comp *c, vp_rect r);

static bool layer_within(const vp_layer *l, const vp_layer *ancestor)
{
	for (; l; l = l->parent) {
		if (l == ancestor) {
			return true;
		}
	}
	return false;
}

/* Take down any bitmap under `l` that the pending change is about to make
 * undisplayable. Called with the model already updated but the planes still
 * where the terminal drew them - the only moment the pixels can be erased;
 * doing this after parking/hiding the plane would strand the picture. */
static void gfx_withdraw_hidden(vp_comp *c, vp_layer *l)
{
	if (c->ngfx == 0) {
		return; /* the common case: moving the pointer, a window, a tile */
	}
	for (int i = 0; i < c->ngfx; i++) {
		vp_graphic *g = c->gfx[i];
		vp_rect r;
		if (!g->plane || !layer_within(g->owner, l)) {
			continue;
		}
		if (gfx_visible(c, g, &r)) {
			continue; /* it can simply ride the change */
		}
		ncplane_destroy(g->plane);
		g->plane = NULL;
		g->shown = false;
		damage_under(c, g->shown_abs);
		c->gfx_dirty = true;
	}
}

static vp_layer *layer_alloc(vp_comp *c, vp_layer *parent, struct ncplane *p,
			     vp_rect rect, vp_paint_fn paint, void *user)
{
	vp_layer *l = calloc(1, sizeof(*l));
	if (!l) {
		ncplane_destroy(p);
		return NULL;
	}
	l->comp = c;
	l->plane = p;
	l->parent = parent;
	l->rect = rect;
	l->visible = true;
	l->opaque = true;
	l->damaged = true;
	l->damaged_full = true;
	l->paint = paint;
	l->user = user;
	l->seq = c->next_seq++;
	if (!parent) {
		l->band = VP_BAND_BACKDROP; /* overwritten by the caller */
	}
	if (!comp_track(c, l)) {
		ncplane_destroy(p);
		free(l);
		return NULL;
	}
	return l;
}

vp_layer *comp_layer_new(vp_comp *c, vp_band band, vp_rect r, vp_paint_fn paint,
			 void *user)
{
	if (r.h < 1 || r.w < 1) {
		return NULL;
	}
	ncplane_options o = { 0 };
	o.y = r.y;
	o.x = r.x;
	o.rows = (unsigned)r.h;
	o.cols = (unsigned)r.w;
	struct ncplane *p = ncplane_create(c->std, &o);
	if (!p) {
		return NULL;
	}
	vp_layer *l = layer_alloc(c, NULL, p, r, paint, user);
	if (!l) {
		return NULL;
	}
	l->band = band;
	l->order = ++c->band_order[band];
	ncplane_set_userptr(p, l);
	return l;
}

vp_layer *comp_layer_adopt(vp_comp *c, vp_band band, struct ncplane *plane,
			   void *user)
{
	if (!plane) {
		return NULL;
	}
	unsigned h = 0, w = 0;
	ncplane_dim_yx(plane, &h, &w);
	vp_rect r = { ncplane_y(plane), ncplane_x(plane), (int)h, (int)w };
	vp_layer *l = layer_alloc(c, NULL, plane, r, NULL, user);
	if (!l) {
		return NULL;
	}
	l->band = band;
	l->order = ++c->band_order[band];
	l->damaged = false; /* its pixels came with it */
	l->damaged_full = false;
	ncplane_set_userptr(plane, l);
	return l;
}

vp_layer *comp_sublayer_new(vp_layer *parent, vp_rect local, vp_paint_fn paint,
			    void *user)
{
	if (!parent || local.h < 1 || local.w < 1) {
		return NULL;
	}
	ncplane_options o = { 0 };
	o.y = local.y;
	o.x = local.x;
	o.rows = (unsigned)local.h;
	o.cols = (unsigned)local.w;
	struct ncplane *p = ncplane_create(parent->plane, &o);
	if (!p) {
		return NULL;
	}
	vp_layer *l = layer_alloc(parent->comp, parent, p, local, paint, user);
	if (l) {
		ncplane_set_userptr(p, l);
	}
	return l;
}

vp_layer *comp_sublayer_adopt(vp_layer *parent, struct ncplane *plane,
			      void *user)
{
	if (!parent || !plane) {
		return NULL;
	}
	unsigned h = 0, w = 0;
	ncplane_dim_yx(plane, &h, &w);
	vp_rect r = { ncplane_y(plane), ncplane_x(plane), (int)h, (int)w };
	vp_layer *l = layer_alloc(parent->comp, parent, plane, r, NULL, user);
	if (l) {
		l->damaged = false;
		l->damaged_full = false;
		ncplane_set_userptr(plane, l);
	}
	return l;
}

void comp_layer_destroy(vp_layer *l)
{
	if (!l || l->is_root) {
		return;
	}
	vp_comp *c = l->comp;

	/* Sublayers and graphics are owned by the layer: take them with it. The
	 * scan restarts after each child because destroying one compacts the
	 * registry (and may take grandchildren with it). */
	for (bool again = true; again;) {
		again = false;
		for (int i = 0; i < c->nlayers; i++) {
			if (c->layers[i]->parent == l) {
				comp_layer_destroy(c->layers[i]);
				again = true;
				break;
			}
		}
	}
	comp_graphics_clear(l);

	comp_untrack(c, l);
	if (l->plane) {
		ncplane_destroy(l->plane);
	}
	free(l);
	comp_invalidate(c);
}

struct ncplane *comp_layer_plane(const vp_layer *l)
{
	return l ? l->plane : NULL;
}

void *comp_layer_user(const vp_layer *l)
{
	return l ? l->user : NULL;
}

vp_rect comp_layer_rect(const vp_layer *l)
{
	return l->rect;
}

void comp_layer_move(vp_layer *l, int y, int x)
{
	if (l->is_root || (l->rect.y == y && l->rect.x == x)) {
		return;
	}
	l->rect.y = y;
	l->rect.x = x;
	gfx_withdraw_hidden(l->comp, l); /* model moved, planes have not */
	if (l->visible) {
		ncplane_move_yx(l->plane, y, x);
	}
	comp_touch(l->comp);
}

void comp_layer_resize(vp_layer *l, int h, int w)
{
	if (l->is_root || h < 1 || w < 1 ||
	    (l->rect.h == h && l->rect.w == w)) {
		return;
	}
	l->rect.h = h;
	l->rect.w = w;
	gfx_withdraw_hidden(l->comp,
			    l); /* a shrink can push a bitmap outside */
	ncplane_resize_simple(l->plane, (unsigned)h, (unsigned)w);
	comp_layer_damage_full(l);
	comp_touch(l->comp);
}

void comp_layer_show(vp_layer *l, bool visible)
{
	if (l->is_root || l->visible == visible) {
		return;
	}
	l->visible = visible;
	/* Every bitmap under a layer being hidden has to go before the plane
	 * does, while the terminal can still be told to erase it. */
	if (!visible) {
		gfx_withdraw_hidden(l->comp, l);
	}
	/* Parking is the mechanism, not the model: the layer's rect is
	 * untouched, so everything that asks where it is still gets the right
	 * answer, and showing it again just puts the plane back. */
	ncplane_move_yx(l->plane, visible ? l->rect.y : COMP_PARK_Y, l->rect.x);
	if (visible) {
		comp_layer_damage_full(l);
	}
	comp_touch(l->comp);
}

bool comp_layer_visible(const vp_layer *l)
{
	return l->visible;
}

void comp_layer_set_opaque(vp_layer *l, bool opaque)
{
	if (l->opaque != opaque) {
		l->opaque = opaque;
		comp_touch(l->comp);
	}
}

void comp_layer_damage(vp_layer *l)
{
	if (l) {
		l->damaged = true;
	}
}

void comp_layer_damage_full(vp_layer *l)
{
	if (l) {
		l->damaged = true;
		l->damaged_full = true;
	}
}

void comp_layer_raise(vp_layer *l)
{
	vp_layer *r = layer_root_of(l);
	int top = r->comp->band_order[r->band];
	if (r->order == top) {
		return; /* already frontmost in its band */
	}
	r->order = ++r->comp->band_order[r->band];
	comp_touch(r->comp);
}

int comp_layer_order(const vp_layer *l)
{
	return layer_root_of((vp_layer *)l)->order;
}

vp_layer *comp_layer_at(vp_comp *c, vp_band band, int y, int x)
{
	comp_sort(c);
	for (int i = c->nsorted - 1; i >= 0; i--) {
		vp_layer *l = c->sorted[i];
		vp_layer *r = layer_root_of(l);
		if (r->band != band || r->is_root) {
			continue;
		}
		if (!layer_shown(l)) {
			continue;
		}
		if (rect_contains(comp_layer_abs(l), y, x)) {
			return r;
		}
	}
	return NULL;
}

/* ------------------------------------------------------------------------- */
/* Pixel graphics                                                            */
/* ------------------------------------------------------------------------- */

static void gfx_untrack(vp_comp *c, vp_graphic *g)
{
	for (int i = 0; i < c->ngfx; i++) {
		if (c->gfx[i] == g) {
			c->gfx[i] = c->gfx[--c->ngfx];
			return;
		}
	}
}

/* A bitmap that just appeared or disappeared leaves the cells it covered
 * needing a repaint. Damage from the top down and stop at the first opaque
 * layer that wholly covers the rect - repainting below it is wasted work. */
static void damage_under(vp_comp *c, vp_rect r)
{
	comp_sort(c);
	for (int i = c->nsorted - 1; i >= 0; i--) {
		vp_layer *l = c->sorted[i];
		if (!layer_shown(l)) {
			continue;
		}
		vp_rect lr = comp_layer_abs(l);
		if (!rect_overlap(lr, r)) {
			continue;
		}
		comp_layer_damage_full(l);
		if (l->opaque && rect_covers(lr, r)) {
			break;
		}
	}
}

vp_graphic *comp_graphic_add(vp_layer *owner, struct ncvisual *v, int local_y,
			     int local_x, int cell_h, int cell_w)
{
	if (!owner || !v) {
		return NULL;
	}
	vp_comp *c = owner->comp;
	if (!c->pixel_ok) {
		ncvisual_destroy(v);
		return NULL;
	}
	if (c->ngfx == c->gfx_cap) {
		int cap = c->gfx_cap ? c->gfx_cap * 2 : 8;
		vp_graphic **ng = realloc(c->gfx, (size_t)cap * sizeof(*ng));
		if (!ng) {
			ncvisual_destroy(v);
			return NULL;
		}
		c->gfx = ng;
		c->gfx_cap = cap;
	}
	vp_graphic *g = calloc(1, sizeof(*g));
	if (!g) {
		ncvisual_destroy(v);
		return NULL;
	}
	g->comp = c;
	g->owner = owner;
	g->visual = v;
	g->y = local_y;
	g->x = local_x;
	g->h = cell_h;
	g->w = cell_w;
	c->gfx[c->ngfx++] = g;
	c->gfx_dirty = true;
	return g;
}

void comp_graphic_move(vp_graphic *g, int local_y, int local_x)
{
	if (!g || (g->y == local_y && g->x == local_x)) {
		return;
	}
	g->y = local_y;
	g->x = local_x;
	g->comp->gfx_dirty = true;
}

void comp_graphic_remove(vp_graphic *g)
{
	if (!g) {
		return;
	}
	vp_comp *c = g->comp;
	if (g->plane) {
		ncplane_destroy(g->plane);
		g->plane = NULL;
		damage_under(c, g->shown_abs);
		comp_invalidate(c);
	}
	if (g->visual) {
		ncvisual_destroy(g->visual);
	}
	gfx_untrack(c, g);
	free(g);
	c->gfx_dirty = true;
}

void comp_graphics_clear(vp_layer *owner)
{
	if (!owner) {
		return;
	}
	vp_comp *c = owner->comp;
	for (int i = c->ngfx - 1; i >= 0; i--) {
		if (i < c->ngfx && c->gfx[i]->owner == owner) {
			comp_graphic_remove(c->gfx[i]);
		}
	}
}

/* Is any opaque layer stacked above `owner` covering part of `r`? A pixel
 * bitmap cannot be blended with cells drawn over it, so an occluded bitmap must
 * not be on screen at all. */
static bool gfx_occluded(vp_comp *c, vp_layer *owner, vp_rect r)
{
	for (int i = owner->sidx + 1; i < c->nsorted; i++) {
		vp_layer *m = c->sorted[i];
		if (!m->opaque || m->is_root || !layer_shown(m)) {
			continue;
		}
		if (rect_overlap(comp_layer_abs(m), r)) {
			return true;
		}
	}
	return false;
}

/* Decide whether a bitmap should currently be on screen, and where. */
static bool gfx_visible(vp_comp *c, vp_graphic *g, vp_rect *out)
{
	vp_layer *o = g->owner;
	if (!c->pixel_ok || !layer_shown(o)) {
		return false;
	}
	/* Planes are not scissored to their parent, so a bitmap that does not
	 * fit wholly inside its owner would spill over the window frame. */
	if (g->y < 0 || g->x < 0 || g->y + g->h > o->rect.h ||
	    g->x + g->w > o->rect.w) {
		return false;
	}
	vp_rect oa = comp_layer_abs(o);
	vp_rect r = { oa.y + g->y, oa.x + g->x, g->h, g->w };
	/* Wholly on screen, and clear of the last physical row: a bitmap that
	 * reaches it makes many terminals scroll the whole display. */
	if (r.y < 0 || r.x < 0 || r.x + r.w > c->cols || r.y + r.h >= c->rows) {
		return false;
	}
	if (gfx_occluded(c, o, r)) {
		return false;
	}
	*out = r;
	return true;
}

/* Bring the set of live bitmaps in line with what should be visible. Only the
 * graphics whose visibility, position or stacking actually moved are touched -
 * the whole point of computing occlusion instead of tearing every bitmap in
 * every window down on any scene change. */
static bool comp_graphics_sync(vp_comp *c)
{
	if (c->ngfx == 0) {
		return false;
	}
	comp_sort(c);

	bool changed = false;
	for (int i = 0; i < c->ngfx; i++) {
		vp_graphic *g = c->gfx[i];
		vp_rect r = { 0, 0, 0, 0 };
		bool want = gfx_visible(c, g, &r);

		/* Nothing to do when it is up, still wanted, still anchored at
		 * the same spot in its owner, and still at the same depth: the
		 * plane is a child of the owner's, so it has already followed it
		 * wherever it went. Only the *absolute* footprint needs
		 * recording, for the damage bookkeeping. A change of depth does
		 * force a redraw - notcurses' sprixel bookkeeping is
		 * order-sensitive. */
		if (want && g->shown && g->y == g->shown_y &&
		    g->x == g->shown_x && g->shown_order == c->order_rev) {
			g->shown_abs = r;
			continue;
		}
		if (!want && !g->shown) {
			continue;
		}

		if (g->plane) {
			ncplane_destroy(g->plane);
			g->plane = NULL;
			g->shown = false;
			damage_under(c, g->shown_abs);
			changed = true;
		}

		if (want) {
			struct ncvisual_options o = {
				.n = g->owner->plane,
				.y = g->y,
				.x = g->x,
				.scaling = NCSCALE_NONE,
				.blitter = NCBLIT_PIXEL,
				.flags = NCVISUAL_OPTION_CHILDPLANE,
			};
			g->plane = ncvisual_blit(c->nc, g->visual, &o);
			if (g->plane) {
				/* Binding governs geometry, not stacking: a
				 * freshly blitted plane lands on top of the
				 * whole display until it is placed with its
				 * owner. */
				ncplane_move_above(g->plane, g->owner->plane);
				g->shown = true;
				g->shown_y = g->y;
				g->shown_x = g->x;
				g->shown_abs = r;
				g->shown_order = c->order_rev;
				damage_under(c, r);
				changed = true;
			}
		}
	}
	return changed;
}

/* ------------------------------------------------------------------------- */
/* Stacking                                                                  */
/* ------------------------------------------------------------------------- */

/* Rewrite notcurses' plane stack to match the scene, bottom up, so the result
 * depends only on the model. Returns true if the order actually moved - most
 * scene changes (a window sliding) leave it unchanged, sparing a re-splice
 * and every visible bitmap a teardown it doesn't need. */
static bool comp_restack(vp_comp *c)
{
	comp_sort(c);

	bool same = (c->napplied == c->nsorted);
	for (int i = 0; same && i < c->nsorted; i++) {
		same = (c->applied[i] == c->sorted[i]);
	}
	if (same) {
		c->stack_rev = c->rev;
		return false;
	}

	ncplane_move_bottom(c->std);
	struct ncplane *prev = c->std;

	for (int i = 0; i < c->nsorted; i++) {
		vp_layer *l = c->sorted[i];
		if (l->is_root) {
			continue; /* the floor, already placed */
		}
		ncplane_move_above(l->plane, prev);
		prev = l->plane;
		/* A layer's live bitmaps ride directly above it. */
		for (int j = 0; j < c->ngfx; j++) {
			if (c->gfx[j]->owner == l && c->gfx[j]->plane) {
				ncplane_move_above(c->gfx[j]->plane, prev);
				prev = c->gfx[j]->plane;
			}
		}
	}

	memcpy(c->applied, c->sorted, (size_t)c->nsorted * sizeof(*c->applied));
	c->napplied = c->nsorted;
	c->stack_rev = c->rev;
	c->order_rev++;
	return true;
}

/* ------------------------------------------------------------------------- */
/* Cursor and presentation                                                   */
/* ------------------------------------------------------------------------- */

void comp_set_cursor(vp_comp *c, bool on, int y, int x)
{
	c->cursor_on = on;
	c->cursor_y = y;
	c->cursor_x = x;
}

/* Repaint every damaged layer. Returns true if anything was painted. Painters
 * may mark further layers dirty (a panel that resizes, a grid that re-anchors
 * its images), so callers run this until it settles. */
static bool comp_paint(vp_comp *c)
{
	bool drew = false;
	for (int i = 0; i < c->nlayers; i++) {
		vp_layer *l = c->layers[i];
		if (!l->damaged) {
			continue;
		}
		bool full = l->damaged_full;
		l->damaged = false;
		l->damaged_full = false;
		if (l->paint) {
			l->paint(l->plane, full, l->user);
		}
		drew = true;
	}
	return drew;
}

/* Painting, stacking and graphics feed each other (a painter can re-anchor a
 * bitmap; placing one damages the layers whose cells it ate), so settle them
 * before presenting. The bound is a backstop against a self-dirtying painter. */
#define COMP_MAX_PASSES 4

bool comp_frame(vp_comp *c)
{
	bool drew = c->force;
	c->force = false;

	for (int pass = 0; pass < COMP_MAX_PASSES; pass++) {
		bool work = comp_paint(c);

		if (c->rev != c->stack_rev && comp_restack(c)) {
			work = true;
		}
		/* Graphics follow the scene, so they are re-evaluated whenever
		 * it moved as well as when a bitmap was added, dropped or
		 * re-anchored. */
		if (c->gfx_dirty || c->rev != c->gfx_rev) {
			c->gfx_dirty = false;
			c->gfx_rev = c->rev;
			if (comp_graphics_sync(c)) {
				work = true;
			}
		}
		if (!work) {
			break;
		}
		drew = true;
	}

	if (c->cursor_on != c->applied_on ||
	    (c->cursor_on &&
	     (c->cursor_y != c->applied_y || c->cursor_x != c->applied_x))) {
		drew = true;
	}

	if (!drew) {
		return false; /* nothing changed: an idle frame is free */
	}

	if (c->cursor_on) {
		notcurses_cursor_enable(c->nc, c->cursor_y, c->cursor_x);
	} else {
		notcurses_cursor_disable(c->nc);
	}
	c->applied_on = c->cursor_on;
	c->applied_y = c->cursor_y;
	c->applied_x = c->cursor_x;

	notcurses_render(c->nc);
	return true;
}
