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

/* input.c - input routing: keyboard (WM chords vs. forward-to-app) and mouse
 * (focus, title-bar drag-move, border-resize, content forwarding, taskbar).
 * notcurses and GPM (bare console) both feed one source-agnostic mouse_event.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#define MOVE_STEP 2
#define RESIZE_STEP 1

/* Effective modifier mask of a key event (defined below, used by the keymap
 * helpers above it). */
static unsigned eff_mods(const ncinput *ni);

static void toggle_mode(WM *wm)
{
	wm->mode = (wm->mode == MODE_INTERPRET) ? MODE_PASSTHROUGH :
						  MODE_INTERPRET;
	taskbar_damage(wm);
	for (int i = 0; i < wm->nwins; i++) {
		window_damage_frame(
			wm->wins[i]); /* per-window mode indicator */
	}
	vp_log("mode=%s\n",
	       wm->mode == MODE_PASSTHROUGH ? "PASSTHRU" : "INTERPRET");
}

/* The built-in keymap, in one editable table - these are the *defaults*; the
 * config file (bind/unbind lines) edits the live copy in WM.config. 'mods' is
 * matched exactly against the SHIFT/ALT/CTRL bits (lock bits are ignored). */
#define A NCKEY_MOD_ALT
#define AS (NCKEY_MOD_ALT | NCKEY_MOD_SHIFT)
#define S NCKEY_MOD_SHIFT

static const keychord g_default_keymap[] = {
	{ NCKEY_TAB, A, ACT_FOCUS_NEXT },
	{ NCKEY_TAB, AS, ACT_FOCUS_PREV },
	{ NCKEY_F04, A, ACT_CLOSE },
	{ 'n', A, ACT_NEW },
	{ 'm', A, ACT_MIN },
	{ 'x', A, ACT_MAXTOGGLE },
	{ NCKEY_LEFT, A, ACT_MOVE_L },
	{ NCKEY_RIGHT, A, ACT_MOVE_R },
	{ NCKEY_UP, A, ACT_MOVE_U },
	{ NCKEY_DOWN, A, ACT_MOVE_D },
	{ NCKEY_LEFT, AS, ACT_RESIZE_L },
	{ NCKEY_RIGHT, AS, ACT_RESIZE_R },
	{ NCKEY_UP, AS, ACT_RESIZE_U },
	{ NCKEY_DOWN, AS, ACT_RESIZE_D },
	{ NCKEY_PGUP, S, ACT_SCROLL_UP },
	{ NCKEY_PGDOWN, S, ACT_SCROLL_DOWN },
	{ '1', A, ACT_SLOT_1 },
	{ '2', A, ACT_SLOT_2 },
	{ '3', A, ACT_SLOT_3 },
	{ '4', A, ACT_SLOT_4 },
	{ '5', A, ACT_SLOT_5 },
	{ '6', A, ACT_SLOT_6 },
	{ '7', A, ACT_SLOT_7 },
	{ '8', A, ACT_SLOT_8 },
	{ '9', A, ACT_SLOT_9 },
	{ ',', A, ACT_TASKBAR_L },
	{ '.', A, ACT_TASKBAR_R },
};

#undef A
#undef AS
#undef S

/* Action names accepted in `bind` lines, paired with a human label (shown in
 * the in-app settings editor) and the enum. Row order here is the order the
 * settings editor lists actions in. */
static const struct {
	const char *name;
	const char *label;
	vp_action act;
} g_action_names[] = {
	{ "focus_next", "Focus next window", ACT_FOCUS_NEXT },
	{ "focus_prev", "Focus previous window", ACT_FOCUS_PREV },
	{ "new", "New window", ACT_NEW },
	{ "close", "Close window", ACT_CLOSE },
	{ "minimize", "Minimize window", ACT_MIN },
	{ "maximize", "Maximize / restore", ACT_MAXTOGGLE },
	{ "move_left", "Move window left", ACT_MOVE_L },
	{ "move_right", "Move window right", ACT_MOVE_R },
	{ "move_up", "Move window up", ACT_MOVE_U },
	{ "move_down", "Move window down", ACT_MOVE_D },
	{ "resize_left", "Resize window left", ACT_RESIZE_L },
	{ "resize_right", "Resize window right", ACT_RESIZE_R },
	{ "resize_up", "Resize window up", ACT_RESIZE_U },
	{ "resize_down", "Resize window down", ACT_RESIZE_D },
	{ "scroll_up", "Scroll back (history)", ACT_SCROLL_UP },
	{ "scroll_down", "Scroll forward", ACT_SCROLL_DOWN },
	{ "slot1", "Focus taskbar slot 1", ACT_SLOT_1 },
	{ "slot2", "Focus taskbar slot 2", ACT_SLOT_2 },
	{ "slot3", "Focus taskbar slot 3", ACT_SLOT_3 },
	{ "slot4", "Focus taskbar slot 4", ACT_SLOT_4 },
	{ "slot5", "Focus taskbar slot 5", ACT_SLOT_5 },
	{ "slot6", "Focus taskbar slot 6", ACT_SLOT_6 },
	{ "slot7", "Focus taskbar slot 7", ACT_SLOT_7 },
	{ "slot8", "Focus taskbar slot 8", ACT_SLOT_8 },
	{ "slot9", "Focus taskbar slot 9", ACT_SLOT_9 },
	{ "taskbar_left", "Scroll taskbar left", ACT_TASKBAR_L },
	{ "taskbar_right", "Scroll taskbar right", ACT_TASKBAR_R },
};

#define ACTION_COUNT ((int)(sizeof(g_action_names) / sizeof(g_action_names[0])))

/* Named non-printable keys accepted in chord strings. Single printable
 * characters (e.g. "n", "x", "/") and function keys ("f1".."f60") are handled
 * separately in key_from_name(). */
static const struct {
	const char *name;
	uint32_t id;
} g_key_names[] = {
	{ "tab", NCKEY_TAB },
	{ "enter", NCKEY_ENTER },
	{ "return", NCKEY_ENTER },
	{ "esc", NCKEY_ESC },
	{ "escape", NCKEY_ESC },
	{ "space", ' ' },
	{ "backspace", NCKEY_BACKSPACE },
	{ "delete", NCKEY_DEL },
	{ "del", NCKEY_DEL },
	{ "insert", NCKEY_INS },
	{ "ins", NCKEY_INS },
	{ "home", NCKEY_HOME },
	{ "end", NCKEY_END },
	{ "pgup", NCKEY_PGUP },
	{ "pageup", NCKEY_PGUP },
	{ "pgdn", NCKEY_PGDOWN },
	{ "pagedown", NCKEY_PGDOWN },
	{ "up", NCKEY_UP },
	{ "down", NCKEY_DOWN },
	{ "left", NCKEY_LEFT },
	{ "right", NCKEY_RIGHT },
};

static bool ci_eq(const char *a, const char *b)
{
	for (; *a && *b; a++, b++) {
		if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) {
			return false;
		}
	}
	return *a == *b;
}

static bool parse_action(const char *name, vp_action *out)
{
	for (size_t i = 0;
	     i < sizeof(g_action_names) / sizeof(g_action_names[0]); i++) {
		if (ci_eq(name, g_action_names[i].name)) {
			*out = g_action_names[i].act;
			return true;
		}
	}
	return false;
}

static bool key_from_name(const char *name, uint32_t *id)
{
	if (name[0] == '\0') {
		return false;
	}
	/* single printable character: use its codepoint (lowercased if a letter, so
     * the SHIFT modifier alone expresses the upper case). */
	if (name[1] == '\0') {
		unsigned char c = (unsigned char)name[0];
		*id = isalpha(c) ? (uint32_t)tolower(c) : (uint32_t)c;
		return true;
	}
	if ((name[0] == 'f' || name[0] == 'F') &&
	    isdigit((unsigned char)name[1])) {
		char *end = NULL;
		long n = strtol(name + 1, &end, 10);
		if (end && *end == '\0' && n >= 1 && n <= 60) {
			*id = (uint32_t)(NCKEY_F01 + (n - 1));
			return true;
		}
	}
	for (size_t i = 0; i < sizeof(g_key_names) / sizeof(g_key_names[0]);
	     i++) {
		if (ci_eq(name, g_key_names[i].name)) {
			*id = g_key_names[i].id;
			return true;
		}
	}
	return false;
}

/* Parse "alt+shift+left" into an id + modifier mask. The last '+'-separated
 * token is the key; the earlier ones are modifiers. */
static bool parse_chord(const char *str, uint32_t *id_out, unsigned *mods_out)
{
	char buf[64];
	size_t len = strlen(str);
	if (len >= sizeof(buf)) {
		return false;
	}
	memcpy(buf, str, len + 1); /* length already bounded above */

	unsigned mods = 0;
	char *tok = buf;
	for (;;) {
		char *plus = strchr(tok, '+');
		if (!plus) {
			break; /* tok is the final segment: the key itself */
		}
		*plus = '\0';
		if (ci_eq(tok, "alt") || ci_eq(tok, "meta")) {
			mods |= NCKEY_MOD_ALT;
		} else if (ci_eq(tok, "shift")) {
			mods |= NCKEY_MOD_SHIFT;
		} else if (ci_eq(tok, "ctrl") || ci_eq(tok, "control") ||
			   ci_eq(tok, "ctl")) {
			mods |= NCKEY_MOD_CTRL;
		} else {
			return false; /* unknown modifier */
		}
		tok = plus + 1;
	}

	uint32_t id;
	if (!key_from_name(tok, &id)) {
		return false;
	}
	*id_out = id;
	*mods_out = mods;
	return true;
}

static void keymap_put(VpConfig *cfg, uint32_t id, unsigned mods, vp_action act)
{
	for (int i = 0; i < cfg->nkeys; i++) {
		if (cfg->keymap[i].id == id && cfg->keymap[i].mods == mods) {
			cfg->keymap[i].act = act;
			return;
		}
	}
	if (cfg->nkeys == cfg->keycap) {
		int ncap = cfg->keycap ? cfg->keycap * 2 : 16;
		keychord *n = realloc(cfg->keymap, (size_t)ncap * sizeof(*n));
		if (!n) {
			return;
		}
		cfg->keymap = n;
		cfg->keycap = ncap;
	}
	cfg->keymap[cfg->nkeys++] = (keychord){ id, mods, act };
}

void keymap_load_defaults(VpConfig *cfg)
{
	int n = (int)(sizeof(g_default_keymap) / sizeof(g_default_keymap[0]));
	cfg->keymap = malloc((size_t)n * sizeof(keychord));
	if (cfg->keymap) {
		memcpy(cfg->keymap, g_default_keymap,
		       (size_t)n * sizeof(keychord));
		cfg->nkeys = n;
		cfg->keycap = n;
	} else {
		cfg->nkeys = 0;
		cfg->keycap = 0;
	}
	cfg->toggle_key = VP_TOGGLE_KEY;
}

bool keymap_bind(VpConfig *cfg, const char *chord, const char *action)
{
	uint32_t id;
	unsigned mods;
	vp_action act;
	if (!parse_chord(chord, &id, &mods)) {
		vp_log("config: bad key chord '%s'\n", chord);
		return false;
	}
	if (!parse_action(action, &act)) {
		vp_log("config: unknown action '%s'\n", action);
		return false;
	}
	keymap_put(cfg, id, mods, act);
	return true;
}

bool keymap_unbind(VpConfig *cfg, const char *chord)
{
	/* "unbind = all" clears the whole table - used by the in-app save so the
     * written file is a complete snapshot rather than an overlay on defaults. */
	if (ci_eq(chord, "all") || strcmp(chord, "*") == 0) {
		cfg->nkeys = 0;
		return true;
	}

	uint32_t id;
	unsigned mods;
	if (!parse_chord(chord, &id, &mods)) {
		vp_log("config: bad key chord '%s'\n", chord);
		return false;
	}
	for (int i = 0; i < cfg->nkeys; i++) {
		if (cfg->keymap[i].id == id && cfg->keymap[i].mods == mods) {
			for (int j = i; j < cfg->nkeys - 1; j++) {
				cfg->keymap[j] = cfg->keymap[j + 1];
			}
			cfg->nkeys--;
			return true;
		}
	}
	return true; /* nothing bound to that chord - not an error */
}

bool keymap_set_toggle(VpConfig *cfg, const char *chord)
{
	uint32_t id;
	unsigned mods;
	if (!parse_chord(chord, &id, &mods)) {
		vp_log("config: bad toggle chord '%s'\n", chord);
		return false;
	}
	cfg->toggle_key = id; /* modifiers are ignored for the toggle */
	return true;
}

int keymap_action_count(void)
{
	return ACTION_COUNT;
}

bool keymap_action_info(int row, vp_action *act, const char **label)
{
	if (row < 0 || row >= ACTION_COUNT) {
		return false;
	}
	if (act)
		*act = g_action_names[row].act;
	if (label)
		*label = g_action_names[row].label;
	return true;
}

const char *keymap_action_name(vp_action act)
{
	for (int i = 0; i < ACTION_COUNT; i++) {
		if (g_action_names[i].act == act) {
			return g_action_names[i].name;
		}
	}
	return "?";
}

void keymap_format_chord(uint32_t id, unsigned mods, char *buf, size_t n)
{
	if (n == 0) {
		return;
	}
	buf[0] = '\0';
	size_t len = 0;
#define APPEND(s)                                                 \
	do {                                                      \
		int _w = snprintf(buf + len, n - len, "%s", (s)); \
		if (_w > 0) {                                     \
			len += (size_t)_w;                        \
			if (len >= n)                             \
				len = n - 1;                      \
		}                                                 \
	} while (0)

	if (mods & NCKEY_MOD_CTRL)
		APPEND("ctrl+");
	if (mods & NCKEY_MOD_ALT)
		APPEND("alt+");
	if (mods & NCKEY_MOD_SHIFT)
		APPEND("shift+");

	if (id >= NCKEY_F01 && id <= NCKEY_F60) {
		char fb[8];
		snprintf(fb, sizeof(fb), "f%u", (unsigned)(id - NCKEY_F01 + 1));
		APPEND(fb);
	} else {
		const char *named = NULL;
		for (size_t i = 0;
		     i < sizeof(g_key_names) / sizeof(g_key_names[0]); i++) {
			if (g_key_names[i].id == id) {
				named = g_key_names[i].name;
				break;
			}
		}
		if (named) {
			APPEND(named);
		} else if (id >= 0x21 && id < 0x7f) { /* printable ASCII */
			char cb[2] = { (char)id, '\0' };
			APPEND(cb);
		} else {
			char hb[16];
			snprintf(hb, sizeof(hb), "0x%x", id);
			APPEND(hb);
		}
	}
#undef APPEND
}

bool keymap_chord_for_action(const VpConfig *cfg, vp_action act, char *buf,
			     size_t n)
{
	for (int i = 0; i < cfg->nkeys; i++) {
		if (cfg->keymap[i].act == act) {
			keymap_format_chord(cfg->keymap[i].id,
					    cfg->keymap[i].mods, buf, n);
			return true;
		}
	}
	if (n) {
		snprintf(buf, n, "%s", "(unbound)");
	}
	return false;
}

/* Translate a live keypress into a chord, rejecting bare modifier/lock keys and
 * mouse events. */
bool keymap_chord_from_input(const ncinput *ni, uint32_t *id_out,
			     unsigned *mods_out)
{
	uint32_t id = ni->id;
	if (id == 0 || nckey_mouse_p(id)) {
		return false;
	}
	switch (id) {
	case NCKEY_LSHIFT:
	case NCKEY_RSHIFT:
	case NCKEY_LCTRL:
	case NCKEY_RCTRL:
	case NCKEY_LALT:
	case NCKEY_RALT:
	case NCKEY_LSUPER:
	case NCKEY_RSUPER:
	case NCKEY_LHYPER:
	case NCKEY_RHYPER:
	case NCKEY_LMETA:
	case NCKEY_RMETA:
	case NCKEY_CAPS_LOCK:
	case NCKEY_NUM_LOCK:
	case NCKEY_SCROLL_LOCK:
		return false;
	default:
		break;
	}
	if (id < 0x80 && isalpha((int)id)) {
		id = (uint32_t)tolower((int)id);
	}
	*id_out = id;
	*mods_out = eff_mods(ni);
	return true;
}

/* Make `act` reachable solely via (id,mods): drop any other chords currently
 * bound to it, then set this chord (overriding whatever it pointed at before). */
void keymap_rebind_action(VpConfig *cfg, vp_action act, uint32_t id,
			  unsigned mods)
{
	for (int i = 0; i < cfg->nkeys;) {
		if (cfg->keymap[i].act == act) {
			for (int j = i; j < cfg->nkeys - 1; j++) {
				cfg->keymap[j] = cfg->keymap[j + 1];
			}
			cfg->nkeys--;
		} else {
			i++;
		}
	}
	keymap_put(cfg, id, mods, act);
}

void keymap_unbind_action(VpConfig *cfg, vp_action act)
{
	for (int i = 0; i < cfg->nkeys;) {
		if (cfg->keymap[i].act == act) {
			for (int j = i; j < cfg->nkeys - 1; j++) {
				cfg->keymap[j] = cfg->keymap[j + 1];
			}
			cfg->nkeys--;
		} else {
			i++;
		}
	}
}

/* Effective modifier mask. notcurses carries modifiers two ways that aren't
 * kept in sync (legacy ni->alt/shift/ctrl bools vs. the kitty/CSI-u
 * ni->modifiers bitmask) - consult both or chords vanish on some terminals. */
static unsigned eff_mods(const ncinput *ni)
{
	unsigned mods = ni->modifiers;
	if (ni->alt)
		mods |= NCKEY_MOD_ALT;
	if (ni->shift)
		mods |= NCKEY_MOD_SHIFT;
	if (ni->ctrl)
		mods |= NCKEY_MOD_CTRL;
	return mods & (NCKEY_MOD_ALT | NCKEY_MOD_SHIFT | NCKEY_MOD_CTRL);
}

static void focus_slot(WM *wm, int slot)
{
	if (slot < 0 || slot >= wm->nwins) {
		return;
	}
	Window *win = wm->wins[slot];
	if (win->minimized) {
		wm_restore(wm, win);
	} else {
		wm_focus_window(wm, win);
	}
}

static void do_action(WM *wm, vp_action act)
{
	switch (act) {
	case ACT_FOCUS_NEXT:
		wm_focus_next(wm, +1);
		break;
	case ACT_FOCUS_PREV:
		wm_focus_next(wm, -1);
		break;
	case ACT_CLOSE:
		wm_close_focused(wm);
		break;
	case ACT_NEW:
		wm_spawn_window(wm);
		break;
	case ACT_MIN:
		wm_minimize(wm, wm_focused(wm));
		break;
	case ACT_MAXTOGGLE:
		wm_toggle_maximize(wm, wm_focused(wm));
		break;
	case ACT_MOVE_L:
		wm_move_focused(wm, -MOVE_STEP, 0);
		break;
	case ACT_MOVE_R:
		wm_move_focused(wm, +MOVE_STEP, 0);
		break;
	case ACT_MOVE_U:
		wm_move_focused(wm, 0, -MOVE_STEP);
		break;
	case ACT_MOVE_D:
		wm_move_focused(wm, 0, +MOVE_STEP);
		break;
	case ACT_RESIZE_L:
		wm_resize_focused(wm, -RESIZE_STEP, 0);
		break;
	case ACT_RESIZE_R:
		wm_resize_focused(wm, +RESIZE_STEP, 0);
		break;
	case ACT_RESIZE_U:
		wm_resize_focused(wm, 0, -RESIZE_STEP);
		break;
	case ACT_RESIZE_D:
		wm_resize_focused(wm, 0, +RESIZE_STEP);
		break;
	case ACT_SCROLL_UP:
	case ACT_SCROLL_DOWN: {
		Window *win = wm_focused(wm);
		if (win) {
			int page = win->rows > 1 ? win->rows - 1 : 1;
			vt_scroll(win, act == ACT_SCROLL_UP ? +page : -page);
		}
		break;
	}
	case ACT_SLOT_1:
	case ACT_SLOT_2:
	case ACT_SLOT_3:
	case ACT_SLOT_4:
	case ACT_SLOT_5:
	case ACT_SLOT_6:
	case ACT_SLOT_7:
	case ACT_SLOT_8:
	case ACT_SLOT_9:
		focus_slot(wm, (int)(act - ACT_SLOT_1));
		break;
	case ACT_TASKBAR_L:
		taskbar_scroll_by(wm, -1);
		break;
	case ACT_TASKBAR_R:
		taskbar_scroll_by(wm, +1);
		break;
	}
}

bool input_handle_key(WM *wm, const ncinput *ni)
{
	if (wm->settings.open) {
		settings_handle_key(wm, ni);
		return true;
	}

	if (ni->id == wm->config.toggle_key) {
		toggle_mode(wm);
		return true;
	}

	if (wm->mode != MODE_INTERPRET) {
		return false;
	}

	unsigned mods = eff_mods(ni);

	/* Fold back to lowercase, matching keymap_chord_from_input: notcurses
	 * uppercases ASCII letters when Ctrl/Shift is held, but the keymap stores
	 * them lowercased so SHIFT alone expresses the upper case. */
	uint32_t id = ni->id;
	if (id < 0x80 && isalpha((int)id)) {
		id = (uint32_t)tolower((int)id);
	}

	const VpConfig *cfg = &wm->config;
	for (int i = 0; i < cfg->nkeys; i++) {
		if (cfg->keymap[i].id == id && cfg->keymap[i].mods == mods) {
			do_action(wm, cfg->keymap[i].act);
			return true;
		}
	}

	return false;
}

static Window *find_by_id(WM *wm, int id)
{
	for (int i = 0; i < wm->nwins; i++) {
		if (wm->wins[i]->id == id) {
			return wm->wins[i];
		}
	}
	return NULL;
}

/* Source-agnostic mouse model. notcurses funnels every backend (terminal mouse
 * protocols and GPM on the console alike) into ncinput, which input_route_mouse
 * turns into mouse_event() - so the WM mouse logic lives in exactly one place. */

typedef enum {
	MEV_PRESS,
	MEV_RELEASE,
	MEV_MOTION,
	MEV_SCROLL_UP,
	MEV_SCROLL_DOWN,
	MEV_SCROLL_LEFT,
	MEV_SCROLL_RIGHT,
} mev_type;

static VTermModifier to_vmod(unsigned mods)
{
	VTermModifier m = VTERM_MOD_NONE;
	if (mods & NCKEY_MOD_SHIFT)
		m |= VTERM_MOD_SHIFT;
	if (mods & NCKEY_MOD_ALT)
		m |= VTERM_MOD_ALT;
	if (mods & NCKEY_MOD_CTRL)
		m |= VTERM_MOD_CTRL;
	return m;
}

/* ----- title-bar hit testing (must mirror frame_paint's layout) ----- */

typedef enum { HIT_MOVE, HIT_MIN, HIT_MAX, HIT_CLOSE } titlehit;

static titlehit titlebar_hit(const Window *win, int relx)
{
	int w = win->w;
	int btnx = w - 1 - 9; /* "[_][▢][x]" is 9 columns wide */
	if (btnx > 4) {
		if (relx >= btnx && relx < btnx + 3)
			return HIT_MIN;
		if (relx >= btnx + 3 && relx < btnx + 6)
			return HIT_MAX;
		if (relx >= btnx + 6 && relx < btnx + 9)
			return HIT_CLOSE;
	}
	return HIT_MOVE;
}

/* Which resize edges (if any) the interior border cell (rely,relx) sits on.
 * rely==0 is the title bar and is handled before this is consulted. */
static int border_edge(const Window *win, int rely, int relx)
{
	int edge = 0;
	if (relx == 0)
		edge |= RZ_LEFT;
	if (relx == win->w - 1)
		edge |= RZ_RIGHT;
	if (rely == win->h - 1)
		edge |= RZ_BOTTOM;
	return edge;
}

static void snap_geom(WM *wm, vp_snapzone z, int *gx, int *gy, int *gw, int *gh)
{
	int W = (int)wm->scr_cols;
	int H = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
	int hw = W / 2, hh = H / 2;
	switch (z) {
	case SNAP_LEFT:
		*gx = 0;
		*gy = 0;
		*gw = hw;
		*gh = H;
		break;
	case SNAP_RIGHT:
		*gx = hw;
		*gy = 0;
		*gw = W - hw;
		*gh = H;
		break;
	case SNAP_TL:
		*gx = 0;
		*gy = 0;
		*gw = hw;
		*gh = hh;
		break;
	case SNAP_TR:
		*gx = hw;
		*gy = 0;
		*gw = W - hw;
		*gh = hh;
		break;
	case SNAP_BL:
		*gx = 0;
		*gy = hh;
		*gw = hw;
		*gh = H - hh;
		break;
	case SNAP_BR:
		*gx = hw;
		*gy = hh;
		*gw = W - hw;
		*gh = H - hh;
		break;
	case SNAP_MAX:
		*gx = 0;
		*gy = 0;
		*gw = W;
		*gh = H;
		break;
	default:
		*gx = 0;
		*gy = 0;
		*gw = W;
		*gh = H;
		break;
	}
}

static vp_snapzone snap_zone_at(WM *wm, int y, int x)
{
	int W = (int)wm->scr_cols;
	int H = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
	bool L = x <= VP_SNAP_EDGE;
	bool R = x >= W - 1 - VP_SNAP_EDGE;
	bool top = y < H / 4;
	bool bot = y > H - H / 4;
	if (L)
		return top ? SNAP_TL : bot ? SNAP_BL : SNAP_LEFT;
	if (R)
		return top ? SNAP_TR : bot ? SNAP_BR : SNAP_RIGHT;
	if (y <= VP_SNAP_EDGE)
		return SNAP_MAX; /* top edge (not a corner) maximizes */
	return SNAP_NONE;
}

static void snap_hide(WM *wm)
{
	if (wm->snap_layer) {
		comp_layer_show(wm->snap_layer, false);
	}
	wm->snap_preview = SNAP_NONE;
}

/* The preview outline is a hollow rectangle, so it must not be treated as
 * covering what it is drawn over - a bitmap under the outline stays visible. */
static void snap_paint(struct ncplane *p, bool full, void *user)
{
	(void)full;
	WM *wm = user;
	vp_rect r = comp_layer_rect(wm->snap_layer);
	int gh = r.h, gw = r.w;

	ncplane_erase(p);
	vp_setfg(p, wm->theme.snap_outline);
	ncplane_set_bg_alpha(p, NCALPHA_TRANSPARENT);
	ncplane_putegc_yx(p, 0, 0, "╔", NULL);
	ncplane_putegc_yx(p, 0, gw - 1, "╗", NULL);
	ncplane_putegc_yx(p, gh - 1, 0, "╚", NULL);
	ncplane_putegc_yx(p, gh - 1, gw - 1, "╝", NULL);
	for (int c = 1; c < gw - 1; c++) {
		ncplane_putegc_yx(p, 0, c, "═", NULL);
		ncplane_putegc_yx(p, gh - 1, c, "═", NULL);
	}
	for (int r2 = 1; r2 < gh - 1; r2++) {
		ncplane_putegc_yx(p, r2, 0, "║", NULL);
		ncplane_putegc_yx(p, r2, gw - 1, "║", NULL);
	}
}

static void snap_show(WM *wm, vp_snapzone z)
{
	if (z == SNAP_NONE) {
		snap_hide(wm);
		return;
	}
	int gx, gy, gw, gh;
	snap_geom(wm, z, &gx, &gy, &gw, &gh);
	if (gw < 2 || gh < 2) {
		return;
	}

	if (!wm->snap_layer) {
		vp_rect r = { gy, gx, gh, gw };
		wm->snap_layer = comp_layer_new(wm->comp, VP_BAND_OVERLAY, r,
						snap_paint, wm);
		if (!wm->snap_layer) {
			return;
		}
		uint64_t base =
			0; /* transparent interior so windows show through */
		ncchannels_set_fg_alpha(&base, NCALPHA_TRANSPARENT);
		ncchannels_set_bg_alpha(&base, NCALPHA_TRANSPARENT);
		ncplane_set_base(comp_layer_plane(wm->snap_layer), "", 0, base);
		comp_layer_set_opaque(wm->snap_layer, false);
	} else {
		comp_layer_resize(wm->snap_layer, gh, gw);
		comp_layer_move(wm->snap_layer, gy, gx);
		comp_layer_show(wm->snap_layer, true);
		comp_layer_damage(wm->snap_layer);
	}
	comp_layer_raise(
		wm->snap_layer); /* above the taskbar, within the band */
	wm->snap_preview = z;
}

static void content_forward(Window *win, mev_type t, int btn, int y, int x,
			    unsigned mods)
{
	if (!win) {
		return;
	}
	int crow = y - (win->y + VP_BORDER);
	int ccol = x - (win->x + VP_BORDER);
	if (crow < 0 || ccol < 0 || crow >= win->rows || ccol >= win->cols) {
		return;
	}
	VTermModifier vm = to_vmod(mods);
	/* libvterm only emits a report if the app enabled mouse tracking, so it's
     * always safe to forward; quiet apps simply ignore it. */
	vt_mouse_move(win, crow, ccol, vm);
	switch (t) {
	case MEV_PRESS:
		vt_mouse_button(win, btn, true, vm);
		break;
	case MEV_RELEASE:
		vt_mouse_button(win, btn, false, vm);
		break;
	case MEV_SCROLL_UP:
		vt_mouse_button(win, 4, true, vm);
		break;
	case MEV_SCROLL_DOWN:
		vt_mouse_button(win, 5, true, vm);
		break;
	case MEV_SCROLL_LEFT:
		vt_mouse_button(win, 6, true, vm);
		break;
	case MEV_SCROLL_RIGHT:
		vt_mouse_button(win, 7, true, vm);
		break;
	case MEV_MOTION:
		break; /* the move above is the event */
	}
}

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static void update_drag(WM *wm, int y, int x)
{
	/* Desktop-icon drag: just slide the tile under the pointer, clamped on the
     * screen. Handled before the window lookup below, which doesn't apply. */
	if (wm->drag == DRAG_ICON) {
		vp_layer *icon = wm->drag_icon;
		if (!icon) {
			wm->drag = DRAG_NONE;
			return;
		}
		vp_rect ir = comp_layer_rect(icon);
		int ny = y - wm->drag_off_y;
		int nx = x - wm->drag_off_x;
		int maxy = (int)wm->scr_rows - (wm->taskbar ? 1 : 0) - ir.h;
		int maxx = (int)wm->scr_cols - ir.w;
		if (ny > maxy)
			ny = maxy;
		if (nx > maxx)
			nx = maxx;
		if (ny < 0)
			ny = 0;
		if (nx < 0)
			nx = 0;
		comp_layer_move(icon, ny, nx);
		return;
	}

	Window *win = find_by_id(wm, wm->drag_win);
	if (!win) {
		wm->drag = DRAG_NONE;
		return;
	}

	if (wm->drag == DRAG_MOVE) {
		/* Dragging a maximized window's title bar un-maximizes it and rescales
		 * the grab offset proportionally. Requires real displacement, so a
		 * stationary click (incl. a double-click) leaves it maximized. */
		if (win->maximized && (x - wm->drag_off_x != win->x ||
				       y - wm->drag_off_y != win->y)) {
			wm->drag_off_x = (win->w > 1) ? wm->drag_off_x *
								(win->sw - 1) /
								(win->w - 1) :
							0;
			win->maximized = false;
			window_set_geometry(win, win->x, win->y, win->sw,
					    win->sh);
		}
		int nx = x - wm->drag_off_x;
		int ny = y - wm->drag_off_y;
		if (ny < 0)
			ny = 0;
		if (ny > (int)wm->scr_rows - 1)
			ny = (int)wm->scr_rows - 1;
		if (nx < -(win->w - 2))
			nx = -(win->w - 2);
		if (nx > (int)wm->scr_cols - 2)
			nx = (int)wm->scr_cols - 2;
		window_set_geometry(win, nx, ny, win->w, win->h);
		snap_show(wm, snap_zone_at(wm, y, x));
		comp_layer_raise(win->frame);
		return;
	}

	if (wm->drag == DRAG_RESIZE) {
		int nx = win->x, ny = win->y, nw = win->w, nh = win->h;
		if (wm->resize_edge & RZ_RIGHT)
			nw = x - win->x + 1;
		if (wm->resize_edge & RZ_BOTTOM)
			nh = y - win->y + 1;
		if (wm->resize_edge & RZ_LEFT) {
			int right = wm->drag_ax;
			nx = x;
			nw = right - x + 1;
			if (nw < VP_MIN_W) {
				nw = VP_MIN_W;
				nx = right - nw + 1;
			}
			if (nx < 0) {
				nx = 0;
				nw = right + 1;
			}
		}
		if (wm->resize_edge & RZ_TOP) {
			int bottom = wm->drag_ay;
			ny = y;
			nh = bottom - y + 1;
			if (nh < VP_MIN_H) {
				nh = VP_MIN_H;
				ny = bottom - nh + 1;
			}
			if (ny < 0) {
				ny = 0;
				nh = bottom + 1;
			}
		}
		window_set_geometry(win, nx, ny, nw, nh);
		return;
	}

	if (wm->drag == DRAG_CONTENT) {
		content_forward(win, MEV_MOTION, 1, y, x, 0);
		return;
	}
}

static void mouse_press(WM *wm, int btn, int y, int x, unsigned mods)
{
	if (btn != 1) {
		content_forward(wm_window_at(wm, y, x), MEV_PRESS, btn, y, x,
				mods);
		return;
	}
	if (taskbar_click(wm, y, x)) {
		return;
	}
	Window *win = wm_window_at(wm, y, x);
	if (!win) {
		/* Empty desktop: a launcher icon may have been grabbed. Begin a drag;
		 * a release that never moved it is a plain click (see mouse_release). */
		vp_layer *icon = NULL;
		if (settings_icon_hit(wm, y, x)) {
			icon = wm->settings.icon;
		} else if (exit_icon_hit(wm, y, x)) {
			icon = wm->exit_icon;
		} else if (die_icon_hit(wm, y, x)) {
			icon = wm->die_icon;
		}
		if (icon) {
			vp_rect r = comp_layer_abs(icon);
			wm->drag = DRAG_ICON;
			wm->drag_icon = icon;
			wm->drag_icon_y0 = r.y;
			wm->drag_icon_x0 = r.x;
			wm->drag_off_y = y - r.y;
			wm->drag_off_x = x - r.x;
		}
		return;
	}
	wm_focus_window(wm, win);

	int rely = y - win->y;
	int relx = x - win->x;

	if (rely == 0) {
		/* The corner cells (┌ ┐) resize diagonally; the rest is buttons/move. */
		int corner = 0;
		if (relx == 0)
			corner = RZ_TOP | RZ_LEFT;
		else if (relx == win->w - 1)
			corner = RZ_TOP | RZ_RIGHT;
		if (corner) {
			if (win->maximized) {
				win->maximized = false;
			}
			wm->drag = DRAG_RESIZE;
			wm->drag_win = win->id;
			wm->resize_edge = corner;
			wm->drag_ax = win->x + win->w -
				      1; /* anchor right for left-edge drags */
			wm->drag_ay = win->y + win->h -
				      1; /* anchor bottom for top-edge drags */
			return;
		}
		switch (titlebar_hit(win, relx)) {
		case HIT_MIN:
			wm_minimize(wm, win);
			return;
		case HIT_MAX:
			wm_toggle_maximize(wm, win);
			return;
		case HIT_CLOSE:
			session_close(win);
			win->dead = true;
			vp_log("close id=%d (button)\n", win->id);
			return;
		case HIT_MOVE:
			break;
		}
		/* A second click here within VP_DBLCLICK_MS toggles maximize. */
		uint64_t t = now_ns();
		if (wm->last_titleclick_win == win->id &&
		    t - wm->last_titleclick_ns <=
			    (uint64_t)VP_DBLCLICK_MS * 1000000ull) {
			wm->last_titleclick_win = 0;
			wm_toggle_maximize(wm, win);
			return;
		}
		wm->last_titleclick_ns = t;
		wm->last_titleclick_win = win->id;
		/* Stay maximized through the press so a stationary second click can
		 * restore via wm_toggle_maximize; update_drag drops the flag once the
		 * pointer actually moves. */
		wm->drag = DRAG_MOVE;
		wm->drag_win = win->id;
		wm->drag_off_x = relx;
		wm->drag_off_y = rely;
		return;
	}

	int edge = border_edge(win, rely, relx);
	if (edge) {
		if (win->maximized) {
			win->maximized = false;
		}
		wm->drag = DRAG_RESIZE;
		wm->drag_win = win->id;
		wm->resize_edge = edge;
		wm->drag_ax = win->x + win->w -
			      1; /* anchor right for left-edge drags */
		return;
	}

	wm->drag = DRAG_CONTENT;
	wm->drag_win = win->id;
	content_forward(win, MEV_PRESS, btn, y, x, mods);
}

static void mouse_release(WM *wm, int btn, int y, int x, unsigned mods)
{
	/* Desktop-icon drop: persist the new position if it moved, otherwise treat
	 * the press+release as a plain click and run the icon's action. */
	if (wm->drag == DRAG_ICON) {
		vp_layer *icon = wm->drag_icon;
		update_drag(wm, y, x);
		int ay = wm->drag_icon_y0, ax = wm->drag_icon_x0;
		if (icon) {
			vp_rect r = comp_layer_abs(icon);
			ay = r.y;
			ax = r.x;
		}
		bool moved = (ay != wm->drag_icon_y0 || ax != wm->drag_icon_x0);
		if (moved) {
			if (icon == wm->settings.icon) {
				wm->config.settings_icon_y = ay;
				wm->config.settings_icon_x = ax;
			} else if (icon == wm->exit_icon) {
				wm->config.exit_icon_y = ay;
				wm->config.exit_icon_x = ax;
			} else if (icon == wm->die_icon) {
				wm->config.die_icon_y = ay;
				wm->config.die_icon_x = ax;
			}
			config_save(&wm->config);
			vp_log("icon: moved to y=%d x=%d (saved)\n", ay, ax);
		} else if (icon == wm->settings.icon) {
			settings_open(wm);
		} else if (icon == wm->exit_icon) {
			wm->should_quit = true;
			wm->should_kill_session = false;
			vp_log("exit: persist requested via desktop icon\n");
		} else if (icon == wm->die_icon) {
			wm->should_quit = true;
			wm->should_kill_session = true;
			vp_log("exit: die requested via desktop icon\n");
		}
		wm->drag = DRAG_NONE;
		wm->drag_icon = NULL;
		return;
	}

	/* Apply the final move/resize from the release position, so terminals that
	 * only report press+release (e.g. Konsole) still snap to the drop point. */
	if (wm->drag == DRAG_MOVE || wm->drag == DRAG_RESIZE) {
		update_drag(wm, y, x);
	}

	if (wm->drag == DRAG_MOVE && wm->snap_preview != SNAP_NONE) {
		Window *win = find_by_id(wm, wm->drag_win);
		if (win && wm->snap_preview == SNAP_MAX) {
			/* Dropping at the top edge maximizes. Skip if already maximized:
			 * a plain click on a maximized title bar also arms SNAP_MAX and
			 * must not toggle it back off. */
			if (!win->maximized) {
				wm_toggle_maximize(wm, win);
			}
		} else if (win) {
			int gx, gy, gw, gh;
			snap_geom(wm, wm->snap_preview, &gx, &gy, &gw, &gh);
			win->maximized = false;
			window_set_geometry(win, gx, gy, gw, gh);
		}
	} else if (wm->drag == DRAG_MOVE) {
		/* Plain (non-snap) drop: re-clamp on-screen (the live drag allows
		 * overhang; the drop pulls it back), matching the keyboard move path. */
		Window *win = find_by_id(wm, wm->drag_win);
		if (win) {
			wm_clamp_onscreen(wm, win);
		}
	} else if (wm->drag == DRAG_CONTENT) {
		content_forward(find_by_id(wm, wm->drag_win), MEV_RELEASE, btn,
				y, x, mods);
	}
	snap_hide(wm);
	wm->drag = DRAG_NONE;
	wm->resize_edge = 0;
}

static void mouse_motion(WM *wm, int y, int x, unsigned mods)
{
	if (wm->drag != DRAG_NONE) {
		update_drag(wm, y, x);
		return;
	}
	content_forward(wm_window_at(wm, y, x), MEV_MOTION, 1, y, x, mods);
}

static void mouse_event(WM *wm, mev_type t, int btn, int y, int x,
			unsigned mods)
{
	wm_set_mouse_pos(wm, y, x);

	if (wm->settings.open) {
		if (t == MEV_PRESS) {
			settings_click(wm, btn, y, x);
		} else if (t == MEV_SCROLL_UP) {
			settings_scroll(wm, -1);
		} else if (t == MEV_SCROLL_DOWN) {
			settings_scroll(wm, +1);
		}
		return;
	}

	switch (t) {
	case MEV_PRESS:
		mouse_press(wm, btn, y, x, mods);
		break;
	case MEV_RELEASE:
		mouse_release(wm, btn, y, x, mods);
		break;
	case MEV_MOTION:
		mouse_motion(wm, y, x, mods);
		break;
	case MEV_SCROLL_UP:
	case MEV_SCROLL_DOWN: {
		/* The wheel scrolls our scrollback, unless the hovered app grabbed the
         * mouse (e.g. less, vim) - then the notches belong to it. */
		Window *win = wm_window_at(wm, y, x);
		if (win && !win->app_mouse) {
			int step = wm->config.scroll_step;
			vt_scroll(win, t == MEV_SCROLL_UP ? +step : -step);
		} else {
			content_forward(win, t, btn, y, x, mods);
		}
		break;
	}
	case MEV_SCROLL_LEFT:
	case MEV_SCROLL_RIGHT:
		/* Horizontal wheel over the taskbar row scrolls its window slots;
         * anywhere else it's forwarded to the hovered app. */
		if (wm->taskbar && y == (int)wm->scr_rows - 1) {
			taskbar_scroll_by(wm, t == MEV_SCROLL_LEFT ? -1 : +1);
		} else {
			content_forward(wm_window_at(wm, y, x), t, btn, y, x,
					mods);
		}
		break;
	}
}

void input_route_mouse(WM *wm, const ncinput *ni)
{
	int y = ni->y, x = ni->x;
	unsigned mods = ni->modifiers;

	switch (ni->id) {
	case NCKEY_BUTTON1:
	case NCKEY_BUTTON2:
	case NCKEY_BUTTON3: {
		int btn = (int)(ni->id - NCKEY_BUTTON1) + 1;
		if (ni->evtype == NCTYPE_RELEASE) {
			mouse_event(wm, MEV_RELEASE, btn, y, x, mods);
		} else if (wm->drag != DRAG_NONE ||
			   ni->evtype == NCTYPE_REPEAT) {
			/* notcurses reports a held-button drag as a fresh PRESS, so
			 * treat any non-release event as motion once dragging. */
			mouse_event(wm, MEV_MOTION, btn, y, x, mods);
		} else {
			mouse_event(wm, MEV_PRESS, btn, y, x, mods);
		}
		break;
	}
	case NCKEY_BUTTON4:
		mouse_event(wm, MEV_SCROLL_UP, 4, y, x, mods);
		break;
	case NCKEY_BUTTON5:
		mouse_event(wm, MEV_SCROLL_DOWN, 5, y, x, mods);
		break;
	case NCKEY_BUTTON6:
		mouse_event(wm, MEV_SCROLL_LEFT, 6, y, x, mods);
		break;
	case NCKEY_BUTTON7:
		mouse_event(wm, MEV_SCROLL_RIGHT, 7, y, x, mods);
		break;
	case NCKEY_MOTION:
		mouse_event(wm, MEV_MOTION, 0, y, x, mods);
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------------- */
/* GPM - the bare Linux console mouse, used only when notcurses' own mouse    */
/* decoding is off (both would collide over GPM's shared connection state).  */
/* We request the full event mask so hover motion (GPM_MOVE) is delivered.   */
/* ------------------------------------------------------------------------- */

#include <gpm.h>

void gpm_setup(WM *wm)
{
	wm->gpm_active = false;
	wm->gpm_fd = -1;

	Gpm_Connect conn;
	conn.eventMask =
		(unsigned short)~0; /* all event types, incl. GPM_MOVE */
	conn.defaultMask = 0; /* don't let anything pass to the VT */
	conn.minMod = 0;
	conn.maxMod = (unsigned short)~0;
	conn.pid = 0;
	conn.vc = 0; /* current virtual console */

	int fd = Gpm_Open(&conn, 0);
	if (fd >= 0) {
		wm->gpm_active = true;
		wm->gpm_fd = fd;
		vp_log("gpm: active fd=%d\n", fd);
	} else {
		vp_log("gpm: unavailable (%d); no console mouse\n", fd);
	}
}

void gpm_pump(WM *wm)
{
	if (!wm->gpm_active) {
		return;
	}
	Gpm_Event ev;
	int r = Gpm_GetEvent(&ev);
	if (r <= 0) {
		/* EOF/error: the GPM server went away - drop back to no console mouse. */
		gpm_teardown(wm);
		return;
	}

	int y = ev.y - 1; /* GPM is 1-based */
	int x = ev.x - 1;
	if (y < 0)
		y = 0;
	if (x < 0)
		x = 0;

	unsigned mods =
		0; /* GPM modifier bits aren't mapped; chords stay keyboard */
	int btn = (ev.buttons & GPM_B_RIGHT)  ? 3 :
		  (ev.buttons & GPM_B_MIDDLE) ? 2 :
						1;

	if (ev.wdy > 0) {
		mouse_event(wm, MEV_SCROLL_UP, 4, y, x, mods);
	} else if (ev.wdy < 0) {
		mouse_event(wm, MEV_SCROLL_DOWN, 5, y, x, mods);
	} else if (ev.wdx < 0) {
		mouse_event(wm, MEV_SCROLL_LEFT, 6, y, x, mods);
	} else if (ev.wdx > 0) {
		mouse_event(wm, MEV_SCROLL_RIGHT, 7, y, x, mods);
	} else if (ev.type & GPM_DOWN) {
		mouse_event(wm, MEV_PRESS, btn, y, x, mods);
	} else if (ev.type & GPM_UP) {
		mouse_event(wm, MEV_RELEASE, btn, y, x, mods);
	} else if (ev.type & (GPM_MOVE | GPM_DRAG)) {
		mouse_event(wm, MEV_MOTION, btn, y, x, mods);
	}
}

void gpm_teardown(WM *wm)
{
	if (wm->gpm_active) {
		Gpm_Close();
		wm->gpm_active = false;
		wm->gpm_fd = -1;
	}
}
