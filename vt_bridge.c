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

static void dmg_full(Window *w)
{
	w->dmg_all = true;
}

static int cb_damage(VTermRect rect, void *user)
{
	Window *w = user;
	dmg_add_rows(w, rect.start_row, rect.end_row);
	/* A full-screen erase discards any sixel on the live screen; smaller damage
	 * is deliberately not treated as an erase, or a cursor landing on an
	 * image's edge after a scroll would drop it before it ever showed. */
	if (rect.start_row <= 0 && rect.end_row >= w->rows &&
	    rect.start_col <= 0 && rect.end_col >= w->cols) {
		sixel_damage(w, rect.start_row, rect.end_row, rect.start_col,
			     rect.end_col);
	}
	window_damage_content(w, false);
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
	window_damage_content(w, false);
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
	case VTERM_PROP_ALTSCREEN:
		w->alt_screen = val->boolean != 0;
		/* The alt screen never feeds the history ring, so a held offset
		 * would splice primary-screen history above the app's rows.
		 * Snap back to the live screen for the duration. */
		if (w->sb_offset != 0) {
			w->sb_offset = 0;
			/* drop the title bar's scrolled-back indicator */
			window_damage_frame(w);
		}
		/* Images are anchored in shared scrollback coordinates and can't
		 * tell the two buffers apart, so a primary-screen sixel would
		 * bleed through the alt screen. Drop them on any switch. */
		sixel_images_clear(w);
		/* libvterm damages the whole screen across a switch either way (the
		 * entry erase, and an explicit damagescreen on exit), so the grid
		 * repaint is already covered. */
		break;
	/* OSC window titles are intentionally ignored: the title is derived
	 * from the PTY's foreground program / shell cwd (window_refresh_title). */
	default:
		break;
	}
	return 1;
}

static int cb_bell(void *user)
{
	(void)user;
	return 1;
}

static int cb_resize(int rows, int cols, void *user)
{
	/* We drive resizes ourselves (vt_resize); just note the dirty state. */
	(void)rows;
	(void)cols;
	Window *w = user;
	dmg_full(w);
	window_damage_content(w, false);
	return 1;
}

/* ----- scrollback ring -----
 * The ring holds up to w->sb_max lines, oldest at sb_head. Index i
 * (0 = oldest .. sb_count-1 = newest) lives at slot (sb_head + i) % sb_cap. */

static sb_line *sb_get(Window *w, int i)
{
	return &w->sb[(w->sb_head + i) % w->sb_cap];
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
	Window *w = user;
	/* A line left the top of the live screen: advance the absolute coordinate
	 * sixel images are anchored in, whether or not we retain the line. */
	w->scroll_base++;
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
	window_damage_content(w, false);
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
	w->scroll_base--;
	if (w->sb_offset > w->sb_count) {
		w->sb_offset = w->sb_count;
	}
	dmg_full(w);
	window_damage_content(w, false);
	return 1;
}

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
	sixel_images_clear(w);
	dmg_full(w);
	window_damage_content(w, false);
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

/* libvterm hands unrecognised DCS sequences here. We only care about sixel:
 * DCS <Pa;Pb;Ph> q <data> ST, whose command bytes are digits/';' before a final
 * 'q'. Other 'q'-terminated DCS (e.g. XTGETTCAP's "+q") must not be mistaken
 * for sixel, or it decodes garbage and hangs the app's capability probe. */
static int cb_dcs(const char *command, size_t commandlen,
		  VTermStringFragment frag, void *user)
{
	Window *w = user;
	bool is_sixel = commandlen >= 1 && command[commandlen - 1] == 'q';
	for (size_t i = 0; is_sixel && i + 1 < commandlen; i++) {
		if (!((command[i] >= '0' && command[i] <= '9') ||
		      command[i] == ';')) {
			is_sixel = false;
		}
	}
	if (!is_sixel) {
		return 0;
	}
	sixel_accumulate(w, command, commandlen, frag.str, frag.len,
			 frag.initial, frag.final);
	return 1;
}

/* libvterm hands unrecognised CSI here. We only act on XTSMGRAPHICS
 * (CSI ? Pi ; Pa ; Pv S), which apps use to probe sixel colour registers and
 * the maximum image geometry; answer it only when we can display pixels.
 * Everything else keeps libvterm's default (dropped). */
static int cb_csi(const char *leader, const long args[], int argcount,
		  const char *intermed, char command, void *user)
{
	Window *w = user;
	if (command == 'S' && leader && leader[0] == '?' &&
	    (!intermed || !intermed[0]) && argcount >= 2) {
		if (w->wm && comp_graphics_ok(w->wm->comp)) {
			sixel_answer_xtsmgraphics(w, args, argcount);
		}
		return 1;
	}
	return 0;
}

/* Answer the dynamic-color queries OSC 10/11/12 (fg/bg/cursor) with our
 * window's default colors in xterm's rgb:RRRR/GGGG/BBBB form - apps like lsix
 * use these to blend into the terminal background. A color-set request
 * (payload other than "?") is consumed but ignored. */
static int cb_osc(int command, VTermStringFragment frag, void *user)
{
	Window *w = user;
	if (command != 10 && command != 11 && command != 12) {
		return 0;
	}
	/* Only the single-fragment "?" query interests us. */
	if (!frag.initial || !frag.final || frag.len < 1 ||
	    frag.str[0] != '?') {
		return 1;
	}

	VTermState *st = vterm_obtain_state(w->vt);
	VTermColor fg, bg;
	vterm_state_get_default_colors(st, &fg, &bg);
	VTermColor c = (command == 10) ? fg : bg; /* cursor (12): reuse fg */
	if (!VTERM_COLOR_IS_RGB(&c)) {
		vterm_state_convert_color_to_rgb(st, &c);
	}

	/* 8-bit channels widened to 16-bit by replication (0xff -> 0xffff). */
	char rsp[48];
	int n = snprintf(rsp, sizeof rsp, "\x1b]%d;rgb:%04x/%04x/%04x\x1b\\",
			 command, c.rgb.red * 0x101, c.rgb.green * 0x101,
			 c.rgb.blue * 0x101);
	if (n > 0 && n < (int)sizeof rsp) {
		vt_reply(w, rsp, (size_t)n);
	}
	return 1;
}

static const VTermStateFallbacks state_fallbacks = {
	.csi = cb_csi,
	.osc = cb_osc,
	.dcs = cb_dcs,
};

/* Write a terminal->child reply (DA responses, cursor reports, the sixel probe
 * answers below) to the PTY master, tolerating short writes; drops the tail
 * rather than block on EAGAIN. */
void vt_reply(Window *w, const char *bytes, size_t len)
{
	if (!session_send_input(w, bytes, len) && w->pty >= 0) {
		size_t off = 0;
		while (off < len) {
			ssize_t k = write(w->pty, bytes + off, len - off);
			if (k > 0) {
				off += (size_t)k;
			} else if (k < 0 && (errno == EINTR)) {
				continue;
			} else {
				break;
			}
		}
	}
}

static void cb_output(const char *s, size_t len, void *user)
{
	Window *w = user;

	/* Advertise sixel when the host can show pixels. libvterm's DA1 reply
	 * (ESC[?1;2c) is VT100-class, where splicing in the sixel attribute ';4'
	 * would be malformed and hang strict DA1 parsers; replace it wholesale
	 * with a VT220-class DA1 that lists sixel: ESC[?62;1;2;4c. */
	static const char da1[] = "\x1b[?1;2c";
	static const char da1_sixel[] = "\x1b[?62;1;2;4c";
	const size_t dn = sizeof(da1) - 1;
	bool advertise = w->wm && comp_graphics_ok(w->wm->comp);

	size_t start = 0;
	for (size_t i = 0; advertise && i + dn <= len;) {
		if (memcmp(s + i, da1, dn) == 0) {
			vt_reply(w, s + start, i - start);
			vt_reply(w, da1_sixel, sizeof(da1_sixel) - 1);
			i += dn;
			start = i;
		} else {
			i++;
		}
	}
	vt_reply(w, s + start, len - start);
}

void vt_init(Window *w)
{
	w->vt = vterm_new(w->rows, w->cols);
	vterm_set_utf8(w->vt, 1);

	w->vts = vterm_obtain_screen(w->vt);
	vterm_screen_set_callbacks(w->vts, &screen_cbs, w);
	vterm_screen_set_unrecognised_fallbacks(w->vts, &state_fallbacks, w);
	/* Allocate the alternate buffer. libvterm leaves it off by default, and
	 * without it DECSET 1049/1047/47 is silently refused (its settermprop
	 * bails when the buffer is absent) - so htop and every other full-screen
	 * app draws straight onto the primary buffer, and the 1049l on exit has
	 * nothing to restore, leaving the app's frame stranded on the shell's
	 * screen. Must precede the reset that establishes the active buffer. */
	vterm_screen_enable_altscreen(w->vts, 1);
	vterm_screen_reset(w->vts, 1);

	vterm_output_set_callback(w->vt, cb_output, w);

	w->cursor_visible = true;
	dmg_full(w);
	window_damage_content(w, false);
}

void vt_feed(Window *w, const char *bytes, size_t n)
{
	/* vterm_input_write drives the screen callbacks synchronously, so the
     * precise damage region is accumulated for us during this call; we only
     * flag dirty so the render pass knows to sweep. */
	vterm_input_write(w->vt, bytes, n);
	/* A sixel emitted during that parse defers its cursor advance to here so we
	 * don't re-enter the parser from inside its own DCS callback. */
	while (w->six_pending_lf > 0) {
		vterm_input_write(w->vt, "\r\n", 2);
		w->six_pending_lf--;
	}
	window_damage_content(w, false);
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
	/* Reflow moves content in ways we can't track for fixed pixel placement,
     * and the cell-pixel geometry may differ; drop the images. */
	sixel_images_clear(w);
	vterm_set_size(w->vt, rows, cols);
	comp_layer_resize(w->content, rows, cols);
	dmg_full(w);
	window_damage_content(w, false);
}

void vt_scroll(Window *w, int delta)
{
	/* History belongs to the primary screen; nothing scrolls off the alt one,
	 * so scrolling there would only drag unrelated shell output into view
	 * behind the running app. Inert until it switches back. */
	if (w->alt_screen) {
		return;
	}
	int off = w->sb_offset + delta;
	if (off < 0)
		off = 0;
	if (off > w->sb_count)
		off = w->sb_count;
	if (off != w->sb_offset) {
		w->sb_offset = off;
		dmg_full(w);
		window_damage_content(w, false);
		/* refresh the title-bar scrollback indicator */
		window_damage_frame(w);
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
		window_damage_content(w, false);
		return;
	}

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
	window_damage_content(w, false);
}

void vt_free(Window *w)
{
	sixel_window_free(w);
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
	/* notcurses carries modifiers two ways that aren't kept in sync (legacy
	 * alt/shift/ctrl bools vs. the kitty/CSI-u modifiers bitmask); consult
	 * both or a forwarded Ctrl+c degrades to a bare 'c' for the app. */
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
		window_damage_content(w, false);
		window_damage_frame(w);
	}

	VTermModifier mod = vterm_mods(ni);
	uint32_t id = ni->id;

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

	if (id >= NCKEY_F01 && id <= NCKEY_F60) {
		int n = (int)(id - NCKEY_F01) + 1;
		vt_key_special(w, VTERM_KEY_FUNCTION(n), mod);
		return;
	}

	if (nckey_synthesized_p(id)) {
		return;
	}

	/* notcurses uppercases ASCII letters when Ctrl/Shift is held, so Ctrl+c
	 * reaches us as 'C' - but libvterm only folds lowercase a-z to a C0 byte,
	 * emitting a literal CSI-u sequence otherwise. Undo it for Ctrl so Ctrl+c
	 * yields 0x03 (Shift-only stays uppercase: that's the real character). */
	if ((mod & VTERM_MOD_CTRL) && id >= 'A' && id <= 'Z') {
		id += 'a' - 'A';
	}

	/* A real Unicode codepoint. libvterm handles modifier + cursor-mode
     * encoding for us. */
	if (id != 0) {
		vt_key_unichar(w, id, mod);
	}
}

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
	struct ncplane *n = comp_layer_plane(w->content);
	if (!n) {
		return;
	}

	int off = w->sb_offset;
	if (off < 0)
		off = 0;
	if (off > w->sb_count)
		off = w->sb_count;

	/* Repaint only the rows libvterm reported damaged; any scrollback view or
	 * a full-repaint condition (dmg_all) sweeps the whole grid instead. */
	int r0 = 0, r1 = w->rows;
	if (off == 0 && !w->dmg_all && w->dmg_valid) {
		r0 = w->dmg_r0 < 0 ? 0 : w->dmg_r0;
		r1 = w->dmg_r1 > w->rows ? w->rows : w->dmg_r1;
	}

	cellcache cc = { 0 };

	/* Visible row r maps to combined index (sb_count - off) + r: indices below
	 * sb_count are retained history, the rest live screen rows. */
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
	w->dmg_all = false;
	w->dmg_valid = false;
}
