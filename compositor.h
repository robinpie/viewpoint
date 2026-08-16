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

/* compositor.h - the scene graph and display pipeline.
 *
 * Everything on screen is a *layer*: a rect with a backing ncplane, a z-order
 * band, a visibility flag, and a paint callback. The compositor is the sole
 * owner of stacking, pixel-graphics placement, the text cursor and present().
 * Stacking is declarative (band + order, reconciled by comp_frame) rather than
 * imperative raise-calls; layer geometry is read from vp_layer, never from
 * notcurses, so a hidden (off-screen-parked) layer can't corrupt later math.
 * Pixel graphics are declared, not placed - the compositor blits them only
 * when actually visible. A no-op frame presents nothing.
 */
#ifndef VP_COMPOSITOR_H
#define VP_COMPOSITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <notcurses/notcurses.h>

typedef struct vp_comp vp_comp;
typedef struct vp_layer vp_layer;
typedef struct vp_graphic vp_graphic;

/* A cell rectangle. Absolute (screen) coordinates for a top-level layer,
 * parent-relative for a sublayer. */
typedef struct vp_rect {
	int y, x, h, w;
} vp_rect;

/* Z-order bands, bottom to top. Within a band, layers stack by `order` (see
 * comp_layer_raise) then creation sequence; raising never crosses bands. */
typedef enum {
	VP_BAND_BACKDROP = 0,
	VP_BAND_DESKTOP, /* launcher icons */
	VP_BAND_WINDOW, /* window frames (+ their content sublayers) */
	VP_BAND_OVERLAY, /* taskbar, snap-preview outline */
	VP_BAND_MODAL, /* settings panel */
	VP_BAND_POINTER,
	VP_BAND_COUNT
} vp_band;

/* Repaint a layer's plane. `full` is set after an overlapping pixel bitmap was
 * torn down (the terminal annihilated the cells beneath it); painters that
 * track their own damage must honor it, painters that always redraw can ignore it. */
typedef void (*vp_paint_fn)(struct ncplane *plane, bool full, void *user);

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

/* Wrap a live notcurses context. The standard plane becomes the root surface
 * (floor of the stack, never moved), painted via comp_root_layer(). */
vp_comp *comp_create(struct notcurses *nc);

/* Destroy every layer and graphic the compositor owns (not the notcurses
 * context, and not the standard plane). */
void comp_destroy(vp_comp *c);

/* Re-read the screen dimensions after a terminal resize (callers reposition
 * their own layers separately). */
void comp_resize(vp_comp *c);

struct notcurses *comp_nc(const vp_comp *c);
int comp_rows(const vp_comp *c);
int comp_cols(const vp_comp *c);

/* True if the host terminal can render pixel bitmaps at all. When false,
 * comp_graphic_add is a no-op and no graphic is ever blitted. */
bool comp_graphics_ok(const vp_comp *c);

/* The standard plane's layer: the desktop surface every other layer sits on.
 * Damage it to have its painter re-run. */
vp_layer *comp_root_layer(vp_comp *c);

/* Install the painter for the root surface (the desktop background). */
void comp_set_root_painter(vp_comp *c, vp_paint_fn paint, void *user);

/* ------------------------------------------------------------------------- */
/* Layers                                                                    */
/* ------------------------------------------------------------------------- */

/* Create a layer with a freshly allocated plane at absolute rect `r`, placed at
 * the top of `band`. `paint` may be NULL for a surface drawn by its owner (it
 * must then damage the layer itself). Returns NULL on failure. */
vp_layer *comp_layer_new(vp_comp *c, vp_band band, vp_rect r, vp_paint_fn paint,
			 void *user);

/* Create a layer adopting an already-existing plane (e.g. from ncvisual_blit).
 * Takes ownership: the plane is destroyed with the layer. */
vp_layer *comp_layer_adopt(vp_comp *c, vp_band band, struct ncplane *plane,
			   void *user);

/* Create a sublayer bound to `parent`'s plane (rect in parent-relative cells),
 * always stacked immediately above it and carried along when it moves/hides.
 * This is how a window's terminal grid rides its frame. */
vp_layer *comp_sublayer_new(vp_layer *parent, vp_rect local, vp_paint_fn paint,
			    void *user);
vp_layer *comp_sublayer_adopt(vp_layer *parent, struct ncplane *plane,
			      void *user);

/* Destroy a layer, its plane, its sublayers and any graphics it owns. */
void comp_layer_destroy(vp_layer *l);

struct ncplane *comp_layer_plane(const vp_layer *l);
void *comp_layer_user(const vp_layer *l);

/* Geometry: comp_layer_move takes absolute coords for a top-level layer,
 * parent-relative for a sublayer. comp_layer_rect/_abs stay correct even
 * while the layer is hidden, unlike reading the plane back from notcurses. */
void comp_layer_move(vp_layer *l, int y, int x);
void comp_layer_resize(vp_layer *l, int h, int w);
vp_rect comp_layer_rect(const vp_layer *l);
vp_rect comp_layer_abs(const vp_layer *l);

/* Show or hide a layer (and its sublayers). Hiding is a first-class state:
 * geometry is kept, hit testing/occlusion skip it, and its pixel graphics are
 * torn down rather than dragged off-screen. */
void comp_layer_show(vp_layer *l, bool visible);
bool comp_layer_visible(const vp_layer *l);

/* Whether the layer's rect can be assumed to fully cover what is beneath it.
 * Only used to decide whether it occludes a pixel bitmap, which cannot be
 * blended with cells above it. Layers default to opaque; the snap outline and
 * other see-through overlays must clear it. */
void comp_layer_set_opaque(vp_layer *l, bool opaque);

/* Mark a layer as needing a repaint on the next frame. The _full variant also
 * asks its painter for a from-scratch redraw (see vp_paint_fn). */
void comp_layer_damage(vp_layer *l);
void comp_layer_damage_full(vp_layer *l);

/* Raise a layer to the top of its own band. Bands are never crossed, so this
 * cannot put a window over the taskbar. */
void comp_layer_raise(vp_layer *l);

/* Stacking key within a band; higher is nearer the front. Lets callers ask
 * "which of these layers is topmost" without walking notcurses' stack. */
int comp_layer_order(const vp_layer *l);

/* Topmost visible layer of `band` whose absolute rect covers (y,x), or NULL.
 * Sublayer hits resolve to their top-level ancestor. */
vp_layer *comp_layer_at(vp_comp *c, vp_band band, int y, int x);

/* ------------------------------------------------------------------------- */
/* Pixel graphics                                                            */
/* ------------------------------------------------------------------------- */

/* Attach a decoded bitmap to `owner` at an owner-relative cell anchor, sized
 * `cell_h` x `cell_w`. Takes ownership of `v`. The bitmap is declared, not
 * placed: the compositor blits it only when genuinely visible. Returns NULL
 * (destroying `v`) if the terminal has no pixel support or on OOM. */
vp_graphic *comp_graphic_add(vp_layer *owner, struct ncvisual *v, int local_y,
			     int local_x, int cell_h, int cell_w);

/* Re-anchor a graphic within its owner (a scrollback view that moved). */
void comp_graphic_move(vp_graphic *g, int local_y, int local_x);

void comp_graphic_remove(vp_graphic *g);
void comp_graphics_clear(vp_layer *owner);

/* ------------------------------------------------------------------------- */
/* Cursor and presentation                                                   */
/* ------------------------------------------------------------------------- */

/* Desired hardware text cursor state for the next frame. Applied (and diffed
 * against what is on screen) by comp_frame. */
void comp_set_cursor(vp_comp *c, bool on, int y, int x);

/* Force the next comp_frame to present even if nothing looks dirty. For the
 * cases where the terminal itself lost state (a resize, a host-level refresh). */
void comp_invalidate(vp_comp *c);

/* Run one frame: repaint damaged layers, reconcile the plane stack to the
 * scene, re-place pixel graphics, apply the cursor, present. Returns true if it
 * actually presented - a frame in which nothing changed does no work. */
bool comp_frame(vp_comp *c);

#endif /* VP_COMPOSITOR_H */
