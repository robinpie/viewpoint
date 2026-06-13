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

/* vt_bridge.c - the libvterm <-> notcurses ncplane bridge.
 *
 * Each window owns a VTerm instance fed by its PTY master's bytes. On any
 * screen change we (phase-1 strategy) mark the whole window dirty and re-sweep
 * the entire grid into the content plane at render time.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------------- */
/* Damage accumulation                                                       */
/* ------------------------------------------------------------------------- */

/* Mark the live-screen rows [r0, r1) as needing a repaint at the next sweep. */
static void dmg_add_rows(Window *w, int r0, int r1)
{
	if (w->dmg_all) {
		return; /* already repainting everything */
	}
	if (!w->dmg_valid) {
		w->dmg_r0 = r0;
		w->dmg_r1 = r1;
		w->dmg_valid = true;
	} else {
		if (r0 < w->dmg_r0)
			w->dmg_r0 = r0;
		if (r1 > w->dmg_r1)
			w->dmg_r1 = r1;
	}
}

/* Force a full-window repaint at the next sweep. */
static void dmg_full(Window *w)
{
	w->dmg_all = true;
}

/* ------------------------------------------------------------------------- */
/* libvterm callbacks                                                        */
/* ------------------------------------------------------------------------- */

static int cb_damage(VTermRect rect, void *user)
{
	Window *w = user;
	dmg_add_rows(w, rect.start_row, rect.end_row);
	w->dirty = true;
	return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
	(void)dest;
	(void)src;
	/* A scrolled band is cheaper to treat as a full repaint than to track the
     * moved region precisely; the precise-damage win is for small in-place edits. */
	Window *w = user;
	dmg_full(w);
	w->dirty = true;
	return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
	(void)oldpos;
	Window *w = user;
	w->currow = pos.row;
	w->curcol = pos.col;
	w->cursor_visible = visible != 0;
	return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
	Window *w = user;
	switch (prop) {
	case VTERM_PROP_CURSORVISIBLE:
		w->cursor_visible = val->boolean != 0;
		break;
	case VTERM_PROP_MOUSE:
		/* When the inner app turns on mouse reporting, the wheel belongs to it;
         * otherwise the wheel drives our scrollback (see input.c). */
		w->app_mouse = val->number != VTERM_PROP_MOUSE_NONE;
		break;
	/* OSC window titles are intentionally ignored: viewpoint derives the title
     * itself from the PTY's foreground program / shell cwd (window_refresh_title). */
	default:
		break;
	}
	return 1;
}

static int cb_bell(void *user)
{
	(void)user;
	/* Could flash the frame; ignored for now. */
	return 1;
}

static int cb_resize(int rows, int cols, void *user)
{
	/* We drive resizes ourselves (vt_resize); just note the dirty state. */
	(void)rows;
	(void)cols;
	Window *w = user;
	dmg_full(w);
	w->dirty = true;
	return 1;
}

/* ----- scrollback ring -----
 * The ring holds up to w->sb_max lines, oldest at sb_head. Index i
 * (0 = oldest .. sb_count-1 = newest) lives at slot (sb_head + i) % sb_cap. */

static sb_line *sb_get(Window *w, int i)
{
	return &w->sb[(w->sb_head + i) % w->sb_cap];
}

/* libvterm hands us a line that just scrolled off the top; retain a copy. */
static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	Window *w = user;
	if (w->sb_max <= 0 || cols <= 0) {
		return 0;
	}
	if (!w->sb) {
		w->sb = calloc((size_t)w->sb_max, sizeof(*w->sb));
		if (!w->sb) {
			return 0;
		}
		w->sb_cap = w->sb_max;
	}

	VTermScreenCell *copy = malloc((size_t)cols * sizeof(*copy));
	if (!copy) {
		return 0;
	}
	memcpy(copy, cells, (size_t)cols * sizeof(*copy));

	int slot;
	if (w->sb_count < w->sb_cap) {
		slot = (w->sb_head + w->sb_count) % w->sb_cap;
		w->sb_count++;
	} else {
		/* Ring full: evict the oldest line. */
		free(w->sb[w->sb_head].cells);
		slot = w->sb_head;
		w->sb_head = (w->sb_head + 1) % w->sb_cap;
	}
	w->sb[slot].cells = copy;
	w->sb[slot].cols = cols;

	/* Keep a scrolled-up view pinned to the same lines as new output streams in
     * below it (until it saturates at the top of the history). */
	if (w->sb_offset > 0 && w->sb_offset < w->sb_count) {
		w->sb_offset++;
	}
	dmg_full(w);
	w->dirty = true;
	return 1;
}

/* libvterm wants the most recently scrolled-off line back (e.g. the screen grew
 * or scrolled down past the top). Hand back our newest retained line. */
static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
	Window *w = user;
	if (w->sb_count == 0) {
		return 0;
	}
	sb_line *ln = sb_get(w, w->sb_count - 1);
	int n = ln->cols < cols ? ln->cols : cols;
	int i = 0;
	for (; i < n; i++) {
		cells[i] = ln->cells[i];
	}
	/* The stored line was narrower than the screen now is: pad with blanks. */
	for (; i < cols; i++) {
		cells[i] = ln->cells[ln->cols > 0 ? ln->cols - 1 : 0];
		cells[i].chars[0] = 0;
		cells[i].width = 1;
	}
	free(ln->cells);
	ln->cells = NULL;
	w->sb_count--;
	if (w->sb_offset > w->sb_count) {
		w->sb_offset = w->sb_count;
	}
	dmg_full(w);
	w->dirty = true;
	return 1;
}

/* The inner app cleared the scrollback (e.g. CSI 3 J). Drop our copy too. */
static int cb_sb_clear(void *user)
{
	Window *w = user;
	for (int i = 0; i < w->sb_count; i++) {
		free(sb_get(w, i)->cells);
		sb_get(w, i)->cells = NULL;
	}
	w->sb_count = 0;
	w->sb_head = 0;
	w->sb_offset = 0;
	dmg_full(w);
	w->dirty = true;
	return 1;
}

static const VTermScreenCallbacks screen_cbs = {
	.damage = cb_damage,
	.moverect = cb_moverect,
	.movecursor = cb_movecursor,
	.settermprop = cb_settermprop,
	.bell = cb_bell,
	.resize = cb_resize,
	.sb_pushline = cb_sb_pushline,
	.sb_popline = cb_sb_popline,
	.sb_clear = cb_sb_clear,
};

/* Output callback: bytes the emulator wants to send back to the child. */
static void cb_output(const char *s, size_t len, void *user)
{
	Window *w = user;
	size_t off = 0;
	while (off < len) {
		ssize_t k = write(w->pty, s + off, len - off);
		if (k > 0) {
			off += (size_t)k;
		} else if (k < 0 && (errno == EINTR)) {
			continue;
		} else {
			/* EAGAIN or hard error: drop the rest rather than block. */
			break;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void vt_init(Window *w)
{
	w->vt = vterm_new(w->rows, w->cols);
	vterm_set_utf8(w->vt, 1);

	w->vts = vterm_obtain_screen(w->vt);
	vterm_screen_set_callbacks(w->vts, &screen_cbs, w);
	vterm_screen_reset(w->vts, 1);

	vterm_output_set_callback(w->vt, cb_output, w);

	w->cursor_visible = true;
	dmg_full(w);
	w->dirty = true;
}

void vt_feed(Window *w, const char *bytes, size_t n)
{
	/* vterm_input_write drives the screen callbacks synchronously, so the
     * precise damage region is accumulated for us during this call; we only
     * flag dirty so the render pass knows to sweep. */
	vterm_input_write(w->vt, bytes, n);
	w->dirty = true;
}

void vt_resize(Window *w, int rows, int cols)
{
	if (rows < 1)
		rows = 1;
	if (cols < 1)
		cols = 1;
	w->rows = rows;
	w->cols = cols;
	/* Snap back to the live screen: the reflow that vterm_set_size triggers
     * makes a held scrollback offset meaningless. */
	w->sb_offset = 0;
	vterm_set_size(w->vt, rows, cols);
	if (w->content) {
		ncplane_resize_simple(w->content, (unsigned)rows,
				      (unsigned)cols);
	}
	dmg_full(w);
	w->dirty = true;
}

void vt_scroll(Window *w, int delta)
{
	int off = w->sb_offset + delta;
	if (off < 0)
		off = 0;
	if (off > w->sb_count)
		off = w->sb_count;
	if (off != w->sb_offset) {
		w->sb_offset = off;
		dmg_full(w);
		w->dirty = true;
		w->frame_dirty =
			true; /* refresh the title-bar scrollback indicator */
	}
}

void vt_set_scrollback_max(Window *w, int max)
{
	if (max < 0) {
		max = 0;
	}
	w->sb_max = max;

	/* Ring not materialized yet (no lines retained): the new cap simply governs
     * the first push. */
	if (!w->sb || max == w->sb_cap) {
		return;
	}

	if (max == 0) {
		for (int i = 0; i < w->sb_count; i++) {
			free(sb_get(w, i)->cells);
		}
		free(w->sb);
		w->sb = NULL;
		w->sb_cap = w->sb_count = w->sb_head = w->sb_offset = 0;
		dmg_full(w);
		w->dirty = true;
		return;
	}

	/* Repack into a fresh array of the new size, keeping the newest min(count,
     * max) lines and freeing the oldest ones that no longer fit. */
	sb_line *na = calloc((size_t)max, sizeof(*na));
	if (!na) {
		return; /* keep the existing ring on allocation failure */
	}
	int keep = w->sb_count < max ? w->sb_count : max;
	int drop = w->sb_count - keep;
	for (int i = 0; i < drop; i++) {
		free(sb_get(w, i)->cells);
	}
	for (int i = 0; i < keep; i++) {
		na[i] = *sb_get(w, drop + i);
	}
	free(w->sb);
	w->sb = na;
	w->sb_cap = max;
	w->sb_head = 0;
	w->sb_count = keep;
	if (w->sb_offset > w->sb_count) {
		w->sb_offset = w->sb_count;
	}
	dmg_full(w);
	w->dirty = true;
}

void vt_free(Window *w)
{
	if (w->vt) {
		vterm_free(w->vt);
		w->vt = NULL;
		w->vts = NULL;
	}
	if (w->sb) {
		for (int i = 0; i < w->sb_count; i++) {
			free(sb_get(w, i)->cells);
		}
		free(w->sb);
		w->sb = NULL;
		w->sb_cap = w->sb_count = w->sb_head = w->sb_offset = 0;
	}
}

/* ------------------------------------------------------------------------- */
/* Keyboard / mouse forwarding                                               */
/* ------------------------------------------------------------------------- */

void vt_key_unichar(Window *w, uint32_t c, VTermModifier mod)
{
	vterm_keyboard_unichar(w->vt, c, mod);
}

void vt_key_special(Window *w, VTermKey key, VTermModifier mod)
{
	vterm_keyboard_key(w->vt, key, mod);
}

void vt_mouse_move(Window *w, int row, int col, VTermModifier mod)
{
	vterm_mouse_move(w->vt, row, col, mod);
}

void vt_mouse_button(Window *w, int button, bool pressed, VTermModifier mod)
{
	vterm_mouse_button(w->vt, button, pressed, mod);
}

static VTermModifier vterm_mods(const ncinput *ni)
{
	VTermModifier mod = VTERM_MOD_NONE;
	/* notcurses carries modifiers two ways and does not keep them in sync (see
     * the "FIXME for abi4" on ncinput): the deprecated alt/shift/ctrl bools (set
     * by the legacy path - e.g. Konsole's ESC-prefixed Alt arrives as alt=1) and
     * the ni->modifiers bitmask (set by the kitty/CSI-u path - e.g. Ctrl+c
     * arrives as id='c' with NCKEY_MOD_CTRL and the bool left clear). Consult
     * both, or e.g. a forwarded Ctrl+c degrades to a bare 'c' for the app. */
	if (ni->shift || (ni->modifiers & NCKEY_MOD_SHIFT))
		mod |= VTERM_MOD_SHIFT;
	if (ni->alt || (ni->modifiers & NCKEY_MOD_ALT))
		mod |= VTERM_MOD_ALT;
	if (ni->ctrl || (ni->modifiers & NCKEY_MOD_CTRL))
		mod |= VTERM_MOD_CTRL;
	return mod;
}

void vt_send_key(Window *w, const ncinput *ni)
{
	/* Typing snaps the view back to the live screen, like a real terminal. */
	if (w->sb_offset != 0) {
		w->sb_offset = 0;
		w->dirty = true;
		w->frame_dirty = true;
	}

	VTermModifier mod = vterm_mods(ni);
	uint32_t id = ni->id;

	/* Map the synthesized/control keys libvterm has dedicated encodings for. */
	switch (id) {
	case NCKEY_ENTER:
		vt_key_special(w, VTERM_KEY_ENTER, mod);
		return;
	case NCKEY_TAB:
		vt_key_special(w, VTERM_KEY_TAB, mod);
		return;
	case NCKEY_BACKSPACE:
		vt_key_special(w, VTERM_KEY_BACKSPACE, mod);
		return;
	case NCKEY_ESC:
		vt_key_special(w, VTERM_KEY_ESCAPE, mod);
		return;
	case NCKEY_UP:
		vt_key_special(w, VTERM_KEY_UP, mod);
		return;
	case NCKEY_DOWN:
		vt_key_special(w, VTERM_KEY_DOWN, mod);
		return;
	case NCKEY_LEFT:
		vt_key_special(w, VTERM_KEY_LEFT, mod);
		return;
	case NCKEY_RIGHT:
		vt_key_special(w, VTERM_KEY_RIGHT, mod);
		return;
	case NCKEY_INS:
		vt_key_special(w, VTERM_KEY_INS, mod);
		return;
	case NCKEY_DEL:
		vt_key_special(w, VTERM_KEY_DEL, mod);
		return;
	case NCKEY_HOME:
		vt_key_special(w, VTERM_KEY_HOME, mod);
		return;
	case NCKEY_END:
		vt_key_special(w, VTERM_KEY_END, mod);
		return;
	case NCKEY_PGUP:
		vt_key_special(w, VTERM_KEY_PAGEUP, mod);
		return;
	case NCKEY_PGDOWN:
		vt_key_special(w, VTERM_KEY_PAGEDOWN, mod);
		return;
	default:
		break;
	}

	/* Function keys F1..F60 map to VTERM_KEY_FUNCTION(n). */
	if (id >= NCKEY_F01 && id <= NCKEY_F60) {
		int n = (int)(id - NCKEY_F01) + 1;
		vt_key_special(w, VTERM_KEY_FUNCTION(n), mod);
		return;
	}

	/* Ignore the modifier-only and other synthesized events we don't forward. */
	if (nckey_synthesized_p(id)) {
		return;
	}

	/* notcurses uppercases ASCII letters whenever Ctrl or Shift is held, for
     * cross-backend consistency (see load_ncinput() in notcurses) - so Ctrl+c
     * reaches us as 'C'. But libvterm only folds *lowercase* a-z down to a C0
     * control byte (c & 0x1f); handed an uppercase letter with Ctrl it instead
     * emits a CSI-u sequence ("\e[67;5u"), which inner apps that never enabled
     * that protocol print literally. Undo the uppercasing for the Ctrl case so
     * Ctrl+c yields 0x03. (Shift-only stays uppercase: that 'A' is the real
     * character to send, and libvterm drops Shift for printable chars anyway.) */
	if ((mod & VTERM_MOD_CTRL) && id >= 'A' && id <= 'Z') {
		id += 'a' - 'A';
	}

	/* A real Unicode codepoint. libvterm handles modifier + cursor-mode
     * encoding for us. */
	if (id != 0) {
		vt_key_unichar(w, id, mod);
	}
}

/* ------------------------------------------------------------------------- */
/* Rendering: sweep the grid into the content plane                          */
/* ------------------------------------------------------------------------- */

/* Append a unicode codepoint to a UTF-8 buffer; returns bytes written. */
static size_t utf8_encode(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	} else if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	} else if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	} else {
		out[0] = (char)(0xF0 | (cp >> 18));
		out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
		out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[3] = (char)(0x80 | (cp & 0x3F));
		return 4;
	}
}

/* The plane's fg/bg/styles last applied during a sweep. Runs of identically
 * styled cells (the common case - whole lines share attributes) then skip the
 * redundant setter calls and indexed-to-rgb conversions. Reset per vt_render so
 * the first painted cell always establishes the state. */
typedef struct {
	bool init;
	bool fg_def, bg_def;
	uint8_t fg_r, fg_g, fg_b, bg_r, bg_g, bg_b;
	unsigned styles;
} cellcache;

static void apply_fg(struct ncplane *n, const VTermScreen *vts, cellcache *cc,
		     VTermColor c)
{
	if (VTERM_COLOR_IS_DEFAULT_FG(&c)) {
		if (!cc->init || !cc->fg_def) {
			ncplane_set_fg_default(n);
			cc->fg_def = true;
		}
		return;
	}
	if (VTERM_COLOR_IS_INDEXED(&c)) {
		vterm_screen_convert_color_to_rgb(vts, &c);
	}
	if (!cc->init || cc->fg_def || cc->fg_r != c.rgb.red ||
	    cc->fg_g != c.rgb.green || cc->fg_b != c.rgb.blue) {
		ncplane_set_fg_rgb8(n, c.rgb.red, c.rgb.green, c.rgb.blue);
		cc->fg_def = false;
		cc->fg_r = c.rgb.red;
		cc->fg_g = c.rgb.green;
		cc->fg_b = c.rgb.blue;
	}
}

static void apply_bg(struct ncplane *n, const VTermScreen *vts, cellcache *cc,
		     VTermColor c)
{
	if (VTERM_COLOR_IS_DEFAULT_BG(&c)) {
		if (!cc->init || !cc->bg_def) {
			ncplane_set_bg_default(n);
			cc->bg_def = true;
		}
		return;
	}
	if (VTERM_COLOR_IS_INDEXED(&c)) {
		vterm_screen_convert_color_to_rgb(vts, &c);
	}
	if (!cc->init || cc->bg_def || cc->bg_r != c.rgb.red ||
	    cc->bg_g != c.rgb.green || cc->bg_b != c.rgb.blue) {
		ncplane_set_bg_rgb8(n, c.rgb.red, c.rgb.green, c.rgb.blue);
		cc->bg_def = false;
		cc->bg_r = c.rgb.red;
		cc->bg_g = c.rgb.green;
		cc->bg_b = c.rgb.blue;
	}
}

static void apply_styles(struct ncplane *n, cellcache *cc, unsigned styles)
{
	if (!cc->init || cc->styles != styles) {
		ncplane_set_styles(n, styles);
		cc->styles = styles;
	}
}

/* Paint a default-colored blank (used to pad history lines narrower than the
 * window). Goes through the cache like a real cell so state stays in sync. */
static void paint_blank(struct ncplane *n, cellcache *cc, int row, int col)
{
	if (!cc->init || !cc->fg_def) {
		ncplane_set_fg_default(n);
		cc->fg_def = true;
	}
	if (!cc->init || !cc->bg_def) {
		ncplane_set_bg_default(n);
		cc->bg_def = true;
	}
	apply_styles(n, cc, NCSTYLE_NONE);
	cc->init = true;
	ncplane_putegc_yx(n, row, col, " ", NULL);
}

/* Paint one cell onto the content plane at (row,col). The trailing column of a
 * wide glyph (width 0) must be skipped by the caller before this is reached. */
static void paint_cell(struct ncplane *n, const VTermScreen *vts, cellcache *cc,
		       int row, int col, const VTermScreenCell *cell)
{
	VTermColor fg = cell->fg;
	VTermColor bg = cell->bg;
	if (cell->attrs.reverse) {
		VTermColor t = fg;
		fg = bg;
		bg = t;
	}
	apply_fg(n, vts, cc, fg);
	apply_bg(n, vts, cc, bg);

	unsigned styles = NCSTYLE_NONE;
	if (cell->attrs.bold)
		styles |= NCSTYLE_BOLD;
	if (cell->attrs.underline)
		styles |= NCSTYLE_UNDERLINE;
	if (cell->attrs.italic)
		styles |= NCSTYLE_ITALIC;
	if (cell->attrs.strike)
		styles |= NCSTYLE_STRUCK;
	apply_styles(n, cc, styles);
	cc->init = true;

	/* Build the EGC: base glyph plus any combining chars. */
	char egc[VTERM_MAX_CHARS_PER_CELL * 4 + 1];
	size_t off = 0;
	if (cell->chars[0] == 0) {
		egc[off++] = ' ';
	} else {
		for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell->chars[i];
		     i++) {
			off += utf8_encode(cell->chars[i], egc + off);
		}
	}
	egc[off] = '\0';

	ncplane_putegc_yx(n, row, col, egc, NULL);
}

void vt_render(Window *w)
{
	struct ncplane *n = w->content;
	if (!n) {
		return;
	}

	int off = w->sb_offset;
	if (off < 0)
		off = 0;
	if (off > w->sb_count)
		off = w->sb_count;

	/* Repaint only the rows libvterm reported as damaged. A precise row range
     * applies only to the live screen (off == 0); any scrollback view, plus
     * scrolls/resizes (dmg_all) and the dirty-without-damage fallback, sweeps
     * the whole grid. */
	int r0 = 0, r1 = w->rows;
	if (off == 0 && !w->dmg_all && w->dmg_valid) {
		r0 = w->dmg_r0 < 0 ? 0 : w->dmg_r0;
		r1 = w->dmg_r1 > w->rows ? w->rows : w->dmg_r1;
	}

	cellcache cc = { 0 };

	/* The visible region is a window into [scrollback ... live screen]: visible
     * row r maps to combined index (sb_count - off) + r, where indices below
     * sb_count are retained history and the rest are live screen rows. With
     * off == 0 this is exactly the live screen. */
	for (int row = r0; row < r1; row++) {
		int ci = w->sb_count - off + row;
		if (ci < w->sb_count) {
			const sb_line *ln = sb_get(w, ci);
			for (int col = 0; col < w->cols; col++) {
				if (col < ln->cols) {
					const VTermScreenCell *cell =
						&ln->cells[col];
					if (cell->width == 0) {
						continue;
					}
					paint_cell(n, w->vts, &cc, row, col,
						   cell);
					if (cell->width == 2) {
						col++; /* skip the trailing half */
					}
				} else {
					/* History line narrower than the window: pad with a blank. */
					paint_blank(n, &cc, row, col);
				}
			}
		} else {
			int srow = ci - w->sb_count;
			for (int col = 0; col < w->cols; col++) {
				VTermPos pos = { .row = srow, .col = col };
				VTermScreenCell cell;
				if (vterm_screen_get_cell(w->vts, pos, &cell) ==
				    0) {
					continue;
				}
				if (cell.width == 0) {
					continue;
				}
				paint_cell(n, w->vts, &cc, row, col, &cell);
				if (cell.width == 2) {
					col++; /* skip the trailing half */
				}
			}
		}
	}
	w->dirty = false;
	w->dmg_all = false;
	w->dmg_valid = false;
}
