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

/* viewpoint.h - shared declarations for the viewpoint terminal multiplexer.
 *
 * A single-process, single-threaded, poll(2)-driven WM that presents floating
 * "windows", each running a shell/app in its own PTY + libvterm instance, drawn
 * onto a notcurses ncplane stack.
 */
#ifndef VIEWPOINT_H
#define VIEWPOINT_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>
#include <notcurses/notcurses.h>
#include <vterm.h>

/* ------------------------------------------------------------------------- */
/* Tunables                                                                  */
/* ------------------------------------------------------------------------- */

/* The always-on mode toggle key. Works in BOTH interpret and passthrough
 * modes and is never forwarded to an app. Single #define so it's trivial to
 * change. */
#define VP_TOGGLE_KEY NCKEY_F12

/* Frame geometry: 1-cell border all around, top row doubles as title bar.
 * Content interior is therefore (w-2) x (h-2). */
#define VP_BORDER 1
#define VP_MIN_W 8
#define VP_MIN_H 4

#define VP_TITLE_MAX 128

/* Per-window scrollback defaults: how many lines that scroll off the top are
 * retained, and how many lines a single mouse-wheel notch scrolls the history
 * view. Both are overridable via the config file / in-app Terminal settings;
 * these are just the starting values, and the bounds the editor clamps to. */
#define VP_SCROLLBACK_MAX 2000
#define VP_SCROLL_STEP 3
#define VP_SCROLLBACK_LIMIT 100000 /* upper bound on retained lines */
#define VP_SCROLL_STEP_MAX 50 /* upper bound on the wheel notch size */

/* Off-screen parking row for hidden planes (minimized windows, idle snap
 * preview). Far below any real screen. */
#define VP_HIDDEN_Y 100000

/* How close (in cells) the pointer must come to a screen edge during a move
 * drag to arm an edge/corner snap. */
#define VP_SNAP_EDGE 2

/* Max gap (milliseconds) between two title-bar clicks for them to count as a
 * double-click (which toggles maximize). */
#define VP_DBLCLICK_MS 400

/* Minimum gap (milliseconds) between title re-derivations for one window.
 * window_refresh_title polls /proc; this keeps a window streaming output from
 * re-polling on every render pass. */
#define VP_TITLE_POLL_MS 250

/* ------------------------------------------------------------------------- */
/* Global mode                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
	MODE_INTERPRET = 0, /* WM chords handled, others forwarded */
	MODE_PASSTHROUGH, /* everything (except toggle) forwarded */
} vp_mode;

/* Gated debug log (active only when $VP_DEBUG names a writable file). */
void vp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------------- */
/* Theme                                                                     */
/* ------------------------------------------------------------------------- */

/* A packed 0xRRGGBB color. vp_setfg/vp_setbg unpack and apply it to a plane. */
typedef uint32_t vp_rgb;

/* Desktop background style. SOLID fills with a single base glyph; PATTERN tiles
 * a (possibly multi-cell) glyph string across the desktop; IMAGE blits a decoded
 * image file as a pixel bitmap (needs WM.pixel_ok, else it falls back to SOLID). */
typedef enum {
	BG_SOLID = 0,
	BG_PATTERN,
	BG_IMAGE,
} vp_bg_mode;

/* How a background IMAGE is mapped onto the desktop. */
typedef enum {
	FIT_STRETCH = 0, /* stretch to fill (aspect may distort) */
	FIT_SCALE, /* scale to fit, keep aspect (letterboxed) */
	FIT_CENTER, /* native size, centered (crop/border) */
	FIT_TILE, /* native size, tiled across the desktop */
} vp_bg_fit;

/* A full color palette + desktop-background spec. The active theme is a value
 * copy on the WM (WM.theme): a chosen preset, then any per-color/background
 * overrides from the config laid on top. Every chrome color reads from here, so
 * there are no hardcoded RGBs left in the drawing code. See theme.c. */
typedef struct VpTheme {
	const char *name; /* stable id used in the config ("theme = <name>") */

	/* Window frame chrome. */
	vp_rgb win_focus_fg, win_focus_bg;
	vp_rgb win_unfocus_fg, win_unfocus_bg;

	/* Taskbar. */
	vp_rgb bar_fg, bar_bg; /* base cell */
	vp_rgb bar_focus_fg, bar_focus_bg; /* focused window slot */
	vp_rgb bar_min_fg, bar_min_bg; /* minimized window slot */
	vp_rgb bar_slot_fg, bar_slot_bg; /* normal window slot */
	vp_rgb bar_arrow_bg; /* scroll-arrow background */
	vp_rgb bar_mode_fg; /* mode-pill text */
	vp_rgb bar_pass_bg; /* passthrough pill */
	vp_rgb bar_interp_bg; /* interpret pill */

	/* Settings panel. */
	vp_rgb panel_fg, panel_bg; /* body */
	vp_rgb panel_sel_fg, panel_sel_bg; /* selected row / tile */
	vp_rgb panel_cap_fg, panel_cap_bg; /* row capturing a chord */
	vp_rgb panel_accent; /* borders, tile accents, chord text */
	vp_rgb panel_status; /* status line */
	vp_rgb panel_hint; /* hint line / dim text */

	/* Desktop launcher icons. */
	vp_rgb icon_fg, icon_bg, icon_border, icon_glyph; /* Settings tile */
	vp_rgb exit_fg, exit_bg, exit_border, exit_glyph; /* Exit tile */

	/* Software mouse cursor (console) + snap-preview outline. */
	vp_rgb cursor_fg, cursor_bg;
	vp_rgb snap_outline;

	/* Desktop background. */
	vp_rgb bg_fg, bg_bg; /* glyph colors (SOLID/PATTERN, and IMAGE letterbox) */
	vp_bg_mode bg_mode;
	vp_bg_fit bg_fit;
	const char *bg_glyph; /* base glyph (SOLID) / repeating cell(s) (PATTERN) */
} VpTheme;

/* Apply a packed color to a plane's pen. */
void vp_setfg(struct ncplane *p, vp_rgb c);
void vp_setbg(struct ncplane *p, vp_rgb c);

/* Built-in presets. *_builtin returns NULL for an unknown name. */
const VpTheme *vp_theme_builtin(const char *name);
const VpTheme *vp_theme_default(void);
int vp_theme_count(void);
const VpTheme *vp_theme_by_index(int i);

/* Per-color override table: a stable mapping of element names (as used in the
 * config's "color = <name> <hex>" lines and the Appearance editor) to the
 * VpTheme color field they set. */
int vp_theme_field_count(void);
const char *vp_theme_field_name(int idx); /* NULL if out of range */
int vp_theme_field_index(const char *name); /* -1 if unknown */
vp_rgb vp_theme_field_get(const VpTheme *t, int idx);
void vp_theme_field_set(VpTheme *t, int idx, vp_rgb c);

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* WM chord actions - the targets a key binding can be bound to. The string
 * names accepted in the config file live alongside the dispatcher in input.c. */
typedef enum {
	ACT_FOCUS_NEXT,
	ACT_FOCUS_PREV,
	ACT_CLOSE,
	ACT_NEW,
	ACT_MIN,
	ACT_MAXTOGGLE,
	ACT_MOVE_L,
	ACT_MOVE_R,
	ACT_MOVE_U,
	ACT_MOVE_D,
	ACT_RESIZE_L,
	ACT_RESIZE_R,
	ACT_RESIZE_U,
	ACT_RESIZE_D,
	/* Scroll the focused window's scrollback view back / forward through its
     * retained history. */
	ACT_SCROLL_UP,
	ACT_SCROLL_DOWN,
	/* Focus/restore the window in taskbar slot N. Kept contiguous (slot N is
     * ACT_SLOT_1 + (N-1)) so the dispatcher can recover the slot index. */
	ACT_SLOT_1,
	ACT_SLOT_2,
	ACT_SLOT_3,
	ACT_SLOT_4,
	ACT_SLOT_5,
	ACT_SLOT_6,
	ACT_SLOT_7,
	ACT_SLOT_8,
	ACT_SLOT_9,
	/* Scroll the taskbar's window slots when more windows are open than fit. */
	ACT_TASKBAR_L,
	ACT_TASKBAR_R,
} vp_action;

/* A single key chord → action binding. 'mods' is matched exactly against the
 * SHIFT/ALT/CTRL bits (lock bits are ignored). */
typedef struct {
	uint32_t id;
	unsigned mods;
	vp_action act;
} keychord;

/* Parsed user configuration, read from $XDG_CONFIG_HOME/viewpoint/viewpoint.conf
 * (see config.c). Currently this is just the customizable keymap: it starts as
 * a copy of the built-in defaults, which `bind`/`unbind` lines then edit. */
typedef struct VpConfig {
	bool loaded; /* a config file was found and parsed */

	keychord *keymap; /* malloc'd chord table (defaults + user overrides) */
	int nkeys;
	int keycap;

	uint32_t toggle_key; /* the always-on INTERPRET↔PASSTHROUGH key */

	/* Terminal behavior. scrollback_max is the per-window history cap (lines);
     * scroll_step is how many lines one mouse-wheel notch scrolls. Both seed
     * from the VP_* defaults and are editable in-app (Terminal settings). */
	int scrollback_max;
	int scroll_step;

	/* Desktop launcher icon positions (top-left cell), as stored in the config.
     * -1 means "unset": fall back to the built-in placement (Settings: top-left;
     * Exit: auto-anchored to the bottom-right). Set once the user drags an icon,
     * and written back to the in-app config section by config_save(). */
	int settings_icon_y, settings_icon_x;
	int exit_icon_y, exit_icon_x;
	int die_icon_y, die_icon_x;

	/* Theming. theme_name is the chosen preset (NULL = the default preset).
	 * The background fields override the preset's desktop background when set:
	 * bg_mode is a vp_bg_mode or -1 ("use the theme's"); bg_fit is a vp_bg_fit;
	 * bg_glyph / bg_image_path are heap strings or NULL. Per-color overrides are
	 * laid on top of the preset (see VpColorOverride). */
	char *theme_name;
	int bg_mode; /* vp_bg_mode, or -1 = inherit from the preset */
	int bg_fit; /* vp_bg_fit (only meaningful for BG_IMAGE) */
	char *bg_glyph; /* SOLID/PATTERN glyph override, or NULL */
	char *bg_image_path; /* BG_IMAGE source file, or NULL */

	/* When true, switching the theme preset in-app keeps the per-color and
	 * background overrides layered on top; when false (default), a theme switch
	 * discards them so the new preset applies in full. */
	bool keep_customizations;

	struct VpColorOverride *color_overrides; /* malloc'd list, or NULL */
	int n_color_overrides, color_cap;

	/* Verbatim text of the file's hand-written "manual" section, captured at
     * load. config_save() preserves it untouched and only rewrites the
     * app-managed section below it. NULL if there was none. */
	char *manual_text;
} VpConfig;

/* One per-color override: a VpTheme color-field index (see vp_theme_field_*) and
 * the color to force it to, on top of whatever preset is active. */
typedef struct VpColorOverride {
	int field_idx;
	vp_rgb color;
} VpColorOverride;

/* Scalar (single-value) settings the in-app panels can change and the manual
 * config section may therefore shadow. To make a new in-app setting honor the
 * manual-shadow warning, add a value here and one comparison line in config.c's
 * setting_differs(); config_manual_shadows_setting() then covers it. */
typedef enum {
	SETTING_TOGGLE_KEY,
	SETTING_SCROLLBACK,
	SETTING_SCROLL_STEP,
	SETTING_THEME,
	SETTING_BACKGROUND,
	SETTING_COLORS,
	SETTING_KEEP_CUSTOM,
} vp_setting;

/* ------------------------------------------------------------------------- */
/* Window                                                                    */
/* ------------------------------------------------------------------------- */

/* One retained scrollback line: a heap copy of the cells that scrolled off the
 * top of a window, plus how many of them were valid (the window's width at the
 * time, which may differ from its current width). */
typedef struct sb_line {
	VTermScreenCell *cells;
	int cols;
} sb_line;

/* A decoded sixel image, composited as a notcurses pixel bitmap on its own
 * ncplane (a child of the owning window's content plane). Anchored to an
 * absolute scrollback row so it scrolls and persists with its text; the plane's
 * pixels are blitted once and only its position is updated as the view moves. */
typedef struct vp_image {
	struct ncvisual *visual; /* decoded pixels, retained so the plane can be
				  * re-blitted whenever the image is on-screen */
	struct ncplane
		*plane; /* NCBLIT_PIXEL child of Window.content; exists only
				* while the image is visible (NULL when hidden) */
	int64_t abs_row; /* absolute scrollback anchor of the top-left cell */
	int col; /* anchor column (content-relative) */
	int cell_h, cell_w; /* footprint in cells */
} vp_image;

typedef struct Window {
	int id; /* stable, monotonic per session */
	pid_t child; /* child shell pid */
	int pty; /* PTY master fd (non-blocking) */

	struct WM *
		wm; /* owning WM (callbacks carry only Window*; sixel needs nc) */

	VTerm *vt;
	VTermScreen *vts;

	struct ncplane *frame; /* decoration plane (border + title bar) */
	struct ncplane *content; /* inner terminal grid, child of frame */

	/* geometry of the *frame* in screen cells */
	int x, y, w, h;
	/* content grid dims = h-2, w-2 (kept in sync) */
	int rows, cols;

	bool minimized;
	bool maximized;
	/* saved frame geometry for un-maximize */
	int sx, sy, sw, sh;

	bool dirty; /* content needs a re-sweep into the content plane */
	bool frame_dirty; /* frame chrome needs a redraw (geometry/title/focus) */
	bool dead; /* child exited; destroy after the current loop pass */

	/* Accumulated screen-damage since the last vt_render, so the sweep can repaint
     * only the rows that actually changed instead of the whole grid. dmg_all
     * forces a full repaint (scrolls/resizes/scrollback view changes); otherwise
     * dmg_valid means [dmg_r0, dmg_r1) is the damaged live-screen row range. */
	bool dmg_all;
	bool dmg_valid;
	int dmg_r0, dmg_r1;

	/* inner cursor position (content-relative) and visibility, tracked from
     * the vterm movecursor / settermprop callbacks */
	int currow, curcol;
	bool cursor_visible;

	/* Scrollback ring: lines that scrolled off the top, oldest..newest. Lazily
     * allocated on the first push. sb_offset is how many lines the view is
     * scrolled up into history (0 = the live screen). See vt_bridge.c. */
	sb_line *sb;
	int sb_cap, sb_count, sb_head;
	int sb_offset;
	int sb_max; /* logical history cap for this window (from config) */
	bool app_mouse; /* the inner app enabled mouse reporting (VTERM_PROP_MOUSE) */

	/* Sixel graphics. scroll_base is the absolute index of the top live-screen
	 * row (++ on sb_pushline, -- on sb_popline): the coordinate space images are
	 * anchored in. sixbuf accumulates a sixel DCS payload across libvterm
	 * fragments; images is the list of live image planes. See sixel.c. */
	int64_t scroll_base;
	char *sixbuf;
	size_t sixlen, sixcap;
	bool six_overflow; /* payload exceeded the cap; discard until the final frag */
	int six_pending_lf; /* line feeds owed after an image, flushed by vt_feed
			     * (feeding them inside the DCS callback would re-enter
			     * the parser) */
	vp_image *images;
	int nimages, images_cap;

	/* CLOCK_MONOTONIC (ns) before which window_refresh_title skips its /proc
     * poll - throttles title updates to VP_TITLE_POLL_MS. */
	uint64_t title_poll_ns;

	char title[VP_TITLE_MAX];
} Window;

/* ------------------------------------------------------------------------- */
/* Window manager state                                                      */
/* ------------------------------------------------------------------------- */

typedef enum {
	DRAG_NONE = 0,
	DRAG_MOVE,
	DRAG_RESIZE,
	DRAG_CONTENT, /* button held over a window's content; route to the app */
	DRAG_ICON, /* a desktop launcher icon (Settings / Exit) is being dragged */
} vp_dragkind;

/* Resize-edge bitmask. The top edge is the title bar (drag = move), but its
 * two corner cells (┌ ┐) still resize diagonally. */
#define RZ_LEFT 1
#define RZ_RIGHT 2
#define RZ_BOTTOM 4
#define RZ_TOP 8

typedef enum {
	SNAP_NONE = 0,
	SNAP_LEFT,
	SNAP_RIGHT,
	SNAP_TL,
	SNAP_TR,
	SNAP_BL,
	SNAP_BR,
	SNAP_MAX,
} vp_snapzone;

/* Which screen the modal settings panel is showing: the Control-Panel grid of
 * tiles (the landing page) or the keybinding editor reached from it. */
typedef enum {
	SETTINGS_VIEW_GRID = 0, /* Control-Panel tile grid (landing page) */
	SETTINGS_VIEW_KEYBINDINGS,
	SETTINGS_VIEW_TERMINAL, /* scrollback size / scroll step */
	SETTINGS_VIEW_APPEARANCE, /* theme, desktop background, color overrides */
} settings_view;

/* In-app settings. A modal panel opened from a desktop launcher icon; while open
 * it captures all input. It lands on a Control-Panel grid (settings_view) whose
 * tiles open sub-views; the keybinding editor is one such tile. See settings.c. */
typedef struct Settings {
	bool open;
	settings_view view; /* current screen (grid vs. a sub-view) */
	int grid_sel; /* selected tile in the Control-Panel grid */
	bool capturing; /* waiting for a keypress to assign to row `sel` */
	int sel; /* selected row: 0..ACTION_COUNT-1, then the toggle */
	int scroll; /* first visible row in the scrolling list */
	bool dirty; /* panel needs a redraw */
	struct ncplane *icon; /* desktop launcher (low z, above the background) */
	struct ncplane *panel; /* the modal panel (created on open) */
	char status[128]; /* transient status/hint line */

	/* In-app text entry (Appearance view): while `editing`, keystrokes build up
	 * `input`. edit_kind selects what a commit sets - the background image path
	 * or a color override (edit_color_idx names the VpTheme field). */
	bool editing;
	char input[256];
	int input_len;
	int edit_kind; /* 0 = image path, 1 = color hex */
	int edit_color_idx; /* VpTheme color-field index when edit_kind == 1 */
} Settings;

typedef struct WM {
	struct notcurses *nc;
	struct ncplane *std; /* standard plane (background / desktop) */

	bool pixel_ok; /* terminal supports pixel bitmaps (sixel/kitty) for graphics */

	Window **wins;
	int nwins;
	int cap;
	int focused; /* index into wins, or -1 */
	int next_id;

	vp_mode mode;

	unsigned scr_rows, scr_cols; /* screen dims in cells */

	/* taskbar */
	struct ncplane *taskbar;
	bool taskbar_dirty;
	int taskbar_scroll; /* index of first window shown when slots overflow */

	/* Desktop exit launcher icons, bottom-right (low z, above the background).
	 * Persist detaches the UI; Die also terminates the session daemon. */
	struct ncplane *exit_icon;
	struct ncplane *die_icon;

	/* Bare Linux console vs. a GUI terminal emulator. On the console we own a
     * GPM connection directly and draw a software pointer; in a GUI terminal
     * notcurses decodes the mouse and the emulator draws the hardware cursor.
     * The two mouse sources are mutually exclusive - enabling both makes the
     * two libgpm clients in-process fight over GPM's shared global state. */
	bool console;

	/* software mouse pointer - drawn only on the console, where the full-screen
     * repaint erases the cell-inverting pointer gpm would otherwise draw. */
	struct ncplane *cursor;
	bool draw_cursor;
	int mouse_y, mouse_x;

	/* GPM - the bare-console mouse source (only when `console`). */
	bool gpm_active;
	int gpm_fd;

	/* mouse drag state */
	vp_dragkind drag;
	int drag_win; /* id of window being dragged */
	int drag_off_x; /* grab offset within frame (move) */
	int drag_off_y;
	int resize_edge; /* RZ_* bitmask while DRAG_RESIZE */
	int drag_ax; /* anchor: original right column (for left-edge resize) */
	int drag_ay; /* anchor: original bottom row (for top-edge resize) */
	vp_snapzone snap_preview; /* currently-shown snap outline */
	struct ncplane *snap_plane;

	/* Desktop-icon drag (DRAG_ICON). drag_off_{x,y} hold the grab offset within
     * the tile; drag_icon_{y,x}0 are the tile's top-left at grab time, so a
     * release that didn't move it can be treated as a plain click instead. */
	struct ncplane *drag_icon;
	int drag_icon_y0, drag_icon_x0;

	/* Title-bar double-click tracking (double-click toggles maximize). */
	uint64_t last_titleclick_ns; /* CLOCK_MONOTONIC of the last move-region click */
	int last_titleclick_win; /* id of the window it landed on (0 = none) */

	bool should_quit;
	bool should_kill_session;

	/* Set by mutations that change the display but don't go through a per-object
     * dirty flag (plane moves: dragged icons, the snap-preview outline, a closed
     * settings panel). wm_render skips the (expensive) notcurses_render entirely
     * when nothing - including this - is dirty, so idle/hover frames are free. */
	bool needs_render;

	/* Last state actually applied to the display, for change detection in
     * wm_render: the hardware text cursor and (console only) the software mouse
     * pointer cell. Lets a bare hover with no visible effect skip rendering. */
	bool cursor_on;
	int cursor_y, cursor_x;
	int ptr_y, ptr_x;

	VpConfig config;
	Settings settings;

	/* Active theme (a value copy: preset + config overrides), and the desktop
	 * background. For BG_IMAGE, bg_visual holds the decoded image (retained so it
	 * re-blits cheaply on resize/theme change) and bg_planes are its pixel-bitmap
	 * plane(s) - one for stretch/scale/center, several for a tiled background. */
	VpTheme theme;
	struct ncvisual *bg_visual;
	struct ncplane **bg_planes;
	int bg_nplanes;

	/* Persistent session connection. The daemon owns PTYs; the UI owns only
	 * notcurses/libvterm views and talks to the daemon over this Unix socket. */
	int session_fd;
} WM;

/* ------------------------------------------------------------------------- */
/* config.c                                                                  */
/* ------------------------------------------------------------------------- */

/* Resolve the config file path into buf: $XDG_CONFIG_HOME/viewpoint/viewpoint.conf,
 * falling back to ~/.config/viewpoint/viewpoint.conf. Returns false if neither
 * XDG_CONFIG_HOME nor HOME is set, or the path doesn't fit in buflen. */
bool config_path(char *buf, size_t buflen);

/* Reset cfg to built-in defaults (no file read). */
void config_defaults(VpConfig *cfg);

/* Set defaults, then overlay any settings from the config file. A missing file
 * is not an error: cfg is left at defaults. */
void config_load(VpConfig *cfg);

/* Release anything config_load/config_defaults allocated (the keymap). */
void config_free(VpConfig *cfg);

/* Write the current keymap + toggle key back to the config file (creating the
 * directory if needed). Overwrites the file. Returns false on I/O error. */
bool config_save(const VpConfig *cfg);

/* True if an in-app change would be overridden by the manual config section on
 * the next load (so the change won't fully persist). The *_setting form checks
 * a scalar setting (the manual section assigns it a non-default value); the
 * *_action form checks whether the manual section governs keybinding action
 * `act` or the chord (id,mods) being assigned to it. */
bool config_manual_shadows_setting(const VpConfig *live, vp_setting setting);
bool config_manual_shadows_action(const VpConfig *live, vp_action act,
				  uint32_t id, unsigned mods);

/* Drop the manual-section line(s) that would shadow an in-app change to this
 * setting, so the in-app value wins on the next load. Used by the Appearance
 * menu to make theme/background/color changes authoritative. */
void config_manual_override(VpConfig *cfg, vp_setting setting);
void config_manual_override_color(VpConfig *cfg, int field_idx);

/* Per-color override list maintenance (used by the Appearance editor). The
 * field index is a vp_theme_field_* index. */
bool config_set_color_override(VpConfig *cfg, int idx, vp_rgb color);
void config_clear_color_override(VpConfig *cfg, int idx);
bool config_has_color_override(const VpConfig *cfg, int idx, vp_rgb *out);

/* ------------------------------------------------------------------------- */
/* pty.c                                                                     */
/* ------------------------------------------------------------------------- */

/* forkpty() a child running $SHELL with the given grid size. Returns child pid
 * (>0) and writes the master fd to *master_out (set non-blocking). Returns -1
 * on failure. */
pid_t pty_spawn(int rows, int cols, int *master_out);

/* Push a new window size to the kernel so the child receives SIGWINCH. */
void pty_set_winsize(int master, int rows, int cols);

/* ------------------------------------------------------------------------- */
/* session.c                                                                 */
/* ------------------------------------------------------------------------- */

bool session_connect(WM *wm);
int session_fd(WM *wm);
bool session_request_new(WM *wm, int rows, int cols);
bool session_send_input(Window *w, const char *bytes, size_t len);
bool session_resize(Window *w, int rows, int cols);
bool session_close(Window *w);
bool session_shutdown(WM *wm);
void session_drain(WM *wm);
void session_close_client(WM *wm);
int session_server_main(void);

/* ------------------------------------------------------------------------- */
/* vt_bridge.c                                                               */
/* ------------------------------------------------------------------------- */

/* Build the VTerm + VTermScreen for a window, wiring callbacks (which use the
 * Window* as user data) and the output callback (which writes to w->pty). */
void vt_init(Window *w);

/* Feed bytes read from the PTY master into the emulator. */
void vt_feed(Window *w, const char *bytes, size_t n);

/* Sweep the whole VTerm grid into the window's content plane. */
void vt_render(Window *w);

/* Resize the emulator + content plane to the window's current grid dims. */
void vt_resize(Window *w, int rows, int cols);

/* Scroll the scrollback view by delta lines (+ = back into history, - = toward
 * the live screen). Clamped to [0, retained lines]; marks the window dirty. */
void vt_scroll(Window *w, int delta);

/* Set the window's scrollback line cap, repacking/trimming its retained history
 * to fit (dropping the oldest lines when shrinking). max <= 0 disables it. */
void vt_set_scrollback_max(Window *w, int max);

/* Send a key / unicode codepoint to the child (encodes via libvterm). */
void vt_key_unichar(Window *w, uint32_t c, VTermModifier mod);
void vt_key_special(Window *w, VTermKey key, VTermModifier mod);

/* Translate a notcurses key event and forward it to the child. */
void vt_send_key(Window *w, const ncinput *ni);

/* Forward a mouse event to the inner app. */
void vt_mouse_move(Window *w, int row, int col, VTermModifier mod);
void vt_mouse_button(Window *w, int button, bool pressed, VTermModifier mod);

void vt_free(Window *w);

/* Write a reply (DA/XTSMGRAPHICS responses) from the emulator back to the
 * child on the PTY master. */
void vt_reply(Window *w, const char *bytes, size_t len);

/* ------------------------------------------------------------------------- */
/* sixel.c                                                                   */
/* ------------------------------------------------------------------------- */

/* Accumulate one fragment of a sixel DCS payload (called from the libvterm DCS
 * fallback). On the final fragment the image is decoded and composited. */
void sixel_accumulate(Window *w, const char *command, size_t commandlen,
		      const char *str, size_t len, bool initial, bool final);

/* Reposition every live image plane from its absolute anchor to the current
 * visible row (parking off-screen when not fully visible), and evict images that
 * have scrolled out of retained history. Call once per render sweep. */
void sixel_reposition(Window *w);

/* Destroy every live image plane and reset the list (used on screen clear,
 * resize, and teardown). */
void sixel_images_clear(Window *w);

/* The inner app overwrote live-screen cells in [row0,row1) x [col0,col1) (a
 * clear, an alt-screen switch, a TUI redraw). Drop any image whose footprint
 * intersects that rectangle, so rewritten text shows through instead of stale
 * pixels. Coordinates are live-screen rows/cols. */
void sixel_damage(Window *w, int row0, int row1, int col0, int col1);

/* Destroy the visible image planes but keep their visuals, so they re-blit on
 * the next reposition. Used when a window is minimized: its frame parks
 * off-screen, and a pixel bitmap dragged off-screen scrolls some terminals. */
void sixel_planes_drop(Window *w);

/* Bracket a scene change that can occlude other windows' bitmaps (move, raise,
 * restore): drop every window's planes before, re-blit them all after. See the
 * definitions in sixel.c for why the two halves are separate. */
void sixel_drop_all(WM *wm);
void sixel_reblit_all(WM *wm);

/* Free all per-window sixel state: the accumulation buffer and image planes. */
void sixel_window_free(Window *w);

/* Answer an XTSMGRAPHICS probe (CSI ? Pi ; Pa ; Pv S): report sixel colour
 * registers (Pi=1) or maximum image geometry (Pi=2). Only called when the host
 * terminal supports pixels. */
void sixel_answer_xtsmgraphics(Window *w, const long *args, int argcount);

/* ------------------------------------------------------------------------- */
/* window.c                                                                  */
/* ------------------------------------------------------------------------- */

Window *window_create(WM *wm, int x, int y, int w, int h);
Window *window_create_attached(WM *wm, int id, int x, int y, int w, int h);
void window_destroy(WM *wm, Window *win);
void window_set_geometry(Window *win, int x, int y, int w, int h);
void window_draw_frame(WM *wm, Window *win);
/* Recompute the title from the PTY's foreground process (running program, or
 * user@host:cwd for an idle shell). Returns true if the title changed. */
bool window_refresh_title(Window *win);

/* ------------------------------------------------------------------------- */
/* wm.c                                                                      */
/* ------------------------------------------------------------------------- */

void wm_init(WM *wm, struct notcurses *nc);
/* Append win to the window list. Returns false if the list couldn't grow (OOM),
 * in which case the caller still owns win and must destroy it. */
bool wm_add_window(WM *wm, Window *win);
void wm_remove_window(WM *wm, Window *win);
int wm_index_of(WM *wm, const Window *win);
Window *wm_focused(WM *wm);
void wm_focus_index(WM *wm, int idx);
void wm_focus_window(WM *wm, Window *win);
void wm_focus_next(WM *wm, int dir);
Window *wm_spawn_window(WM *wm);
void wm_close_focused(WM *wm);
void wm_minimize(WM *wm, Window *win);
void wm_restore(WM *wm, Window *win);
void wm_toggle_maximize(WM *wm, Window *win);
void wm_move_focused(WM *wm, int dx, int dy);
void wm_resize_focused(WM *wm, int dw, int dh);
void wm_clamp_onscreen(WM *wm, Window *win);
void wm_handle_resize(WM *wm);
void wm_render(WM *wm);

/* Resolve the config's theme + overrides into wm->theme and apply it live:
 * recolor the persistent base planes (taskbar, cursor), repaint the desktop
 * background, and mark all chrome dirty. Safe to call before the taskbar/icons
 * exist (seeds the theme at startup) and on every later theme change. */
void theme_apply(WM *wm);

/* (Re)paint the desktop background for the current theme: SOLID/PATTERN drive
 * the std plane's base cell (+ a one-shot pattern paint); IMAGE blits the
 * decoded file as pixel bitmap plane(s) below every window. Called from
 * theme_apply, wm_init and wm_handle_resize. */
void background_apply(WM *wm);

/* Destroy the background image plane(s) and retained visual (teardown / before
 * re-applying an image background). */
void background_free(WM *wm);

/* Record the latest pointer position; the software cursor (console only)
 * follows it on the next render. Called from the mouse input path. */
void wm_set_mouse_pos(WM *wm, int y, int x);

/* topmost (highest z-order) window whose frame covers absolute cell (y,x);
 * NULL if none. Skips minimized windows. */
Window *wm_window_at(WM *wm, int y, int x);

/* ------------------------------------------------------------------------- */
/* input.c                                                                   */
/* ------------------------------------------------------------------------- */

/* Returns true if the key was consumed by the WM; false if it should be
 * forwarded to the focused window. Always consumes the toggle key. */
bool input_handle_key(WM *wm, const ncinput *ni);

/* Keymap construction. The default chord table and the action/key name tables
 * the config parser needs live in input.c, next to the dispatcher. */

/* Fill cfg->keymap with the built-in default chords and the default toggle key
 * (allocates cfg->keymap). */
void keymap_load_defaults(VpConfig *cfg);

/* Parse a "alt+shift+left"-style chord plus an action name and add/override the
 * binding in cfg. Returns false (and logs) on a parse error. */
bool keymap_bind(VpConfig *cfg, const char *chord, const char *action);

/* Remove any binding for the given chord. Returns false (and logs) only on a
 * parse error; an absent binding is not an error. */
bool keymap_unbind(VpConfig *cfg, const char *chord);

/* Parse a chord and set it as the mode-toggle key (modifiers are ignored, to
 * match how the toggle is dispatched). Returns false (and logs) on error. */
bool keymap_set_toggle(VpConfig *cfg, const char *chord);

/* Queries used by the in-app settings editor and the config writer. */
int keymap_action_count(void);
bool keymap_action_info(int row, vp_action *act, const char **label);
const char *keymap_action_name(vp_action act);
void keymap_format_chord(uint32_t id, unsigned mods, char *buf, size_t n);
/* Format the chord currently bound to `act` into buf; false (buf="(unbound)")
 * if nothing is bound to it. */
bool keymap_chord_for_action(const VpConfig *cfg, vp_action act, char *buf,
			     size_t n);
/* Translate a live keypress to a chord; false for bare modifiers/locks/mouse. */
bool keymap_chord_from_input(const ncinput *ni, uint32_t *id, unsigned *mods);
/* Bind `act` to exactly (id,mods), dropping its previous chord(s). */
void keymap_rebind_action(VpConfig *cfg, vp_action act, uint32_t id,
			  unsigned mods);
/* Remove every chord bound to `act` (leaving it unbound). */
void keymap_unbind_action(VpConfig *cfg, vp_action act);

/* ------------------------------------------------------------------------- */
/* settings.c                                                                */
/* ------------------------------------------------------------------------- */

/* Create the desktop launcher icon (kept just above the background). */
void settings_init(WM *wm);
/* Repaint the Settings / Exit launcher icons with the current theme colors
 * (their planes persist, so a theme change repaints them in place). */
void settings_icon_redraw(WM *wm);
void exit_icon_redraw(WM *wm);
void die_icon_redraw(WM *wm);
/* Re-clamp the launcher icon onto the screen after a resize (and honor a
 * user-dragged position stored in the config). */
void settings_icon_reflow(WM *wm);
/* Open / close the modal keybinding editor. Closing persists to the config. */
void settings_open(WM *wm);
void settings_close(WM *wm);
/* True if absolute cell (y,x) lands on the launcher icon. */
bool settings_icon_hit(WM *wm, int y, int x);
/* Modal input handlers (called only while the editor is open). */
void settings_handle_key(WM *wm, const ncinput *ni);
void settings_click(WM *wm, int btn, int y, int x);
void settings_scroll(WM *wm, int dir);
/* Redraw the panel if open and dirty (called from the WM render pass);
 * returns true if it actually drew. */
bool settings_render(WM *wm);
/* Destroy planes on shutdown. */
void settings_teardown(WM *wm);

/* Desktop "Exit" launcher icon (bottom-right; clicking it quits viewpoint). */
void exit_icon_init(WM *wm);
/* Re-anchor the icon to the bottom-right corner after a screen resize. */
void exit_icon_reflow(WM *wm);
void die_icon_reflow(WM *wm);
/* True if absolute cell (y,x) lands on the Exit icon. */
bool exit_icon_hit(WM *wm, int y, int x);
bool die_icon_hit(WM *wm, int y, int x);
/* Destroy the icon plane on shutdown. */
void exit_icon_teardown(WM *wm);

void input_route_mouse(WM *wm, const ncinput *ni);

/* GPM lifecycle + event pump (bare Linux console only). */
void gpm_setup(WM *wm);
void gpm_pump(WM *wm);
void gpm_teardown(WM *wm);

/* ------------------------------------------------------------------------- */
/* taskbar.c                                                                 */
/* ------------------------------------------------------------------------- */

void taskbar_create(WM *wm);
void taskbar_reflow(WM *wm);
void taskbar_draw(WM *wm);
/* Handle a click at absolute (y,x) on the taskbar; returns true if consumed. */
bool taskbar_click(WM *wm, int y, int x);
/* Scroll the window slots by `delta` slots (clamped). Used by the horizontal
 * scrollwheel, the ◄► taskbar arrows, and the scroll-taskbar chords. */
void taskbar_scroll_by(WM *wm, int delta);
/* Scroll the slots just enough to bring window index `win_idx` into view (no-op
 * if it already is, or if the slots don't overflow). Called on focus change. */
void taskbar_reveal(WM *wm, int win_idx);

#endif /* VIEWPOINT_H */
