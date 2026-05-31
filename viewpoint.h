// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie
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

/* viewpoint.h — shared declarations for the viewpoint terminal multiplexer.
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

/* Off-screen parking row for hidden planes (minimized windows, idle snap
 * preview). Far below any real screen. */
#define VP_HIDDEN_Y 100000

/* How close (in cells) the pointer must come to a screen edge during a move
 * drag to arm an edge/corner snap. */
#define VP_SNAP_EDGE 2

/* ------------------------------------------------------------------------- */
/* Global mode                                                               */
/* ------------------------------------------------------------------------- */

typedef enum {
    MODE_INTERPRET = 0, /* WM chords handled, others forwarded */
    MODE_PASSTHROUGH,   /* everything (except toggle) forwarded */
} vp_mode;

/* Gated debug log (active only when $VP_DEBUG names a writable file). */
void vp_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ------------------------------------------------------------------------- */
/* Config                                                                    */
/* ------------------------------------------------------------------------- */

/* WM chord actions — the targets a key binding can be bound to. The string
 * names accepted in the config file live alongside the dispatcher in input.c. */
typedef enum {
    ACT_FOCUS_NEXT, ACT_FOCUS_PREV, ACT_CLOSE, ACT_NEW, ACT_MIN, ACT_MAXTOGGLE,
    ACT_MOVE_L, ACT_MOVE_R, ACT_MOVE_U, ACT_MOVE_D,
    ACT_RESIZE_L, ACT_RESIZE_R, ACT_RESIZE_U, ACT_RESIZE_D,
    /* Focus/restore the window in taskbar slot N. Kept contiguous (slot N is
     * ACT_SLOT_1 + (N-1)) so the dispatcher can recover the slot index. */
    ACT_SLOT_1, ACT_SLOT_2, ACT_SLOT_3, ACT_SLOT_4, ACT_SLOT_5,
    ACT_SLOT_6, ACT_SLOT_7, ACT_SLOT_8, ACT_SLOT_9,
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
    bool loaded;        /* a config file was found and parsed */

    keychord *keymap;   /* malloc'd chord table (defaults + user overrides) */
    int nkeys;
    int keycap;

    uint32_t toggle_key; /* the always-on INTERPRET↔PASSTHROUGH key */

    /* Verbatim text of the file's hand-written "manual" section, captured at
     * load. config_save() preserves it untouched and only rewrites the
     * app-managed section below it. NULL if there was none. */
    char *manual_text;
} VpConfig;

/* ------------------------------------------------------------------------- */
/* Window                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct Window {
    int id;             /* stable, monotonic per session */
    pid_t child;        /* child shell pid */
    int pty;            /* PTY master fd (non-blocking) */

    VTerm *vt;
    VTermScreen *vts;

    struct ncplane *frame;   /* decoration plane (border + title bar) */
    struct ncplane *content; /* inner terminal grid, child of frame */

    /* geometry of the *frame* in screen cells */
    int x, y, w, h;
    /* content grid dims = h-2, w-2 (kept in sync) */
    int rows, cols;

    bool minimized;
    bool maximized;
    /* saved frame geometry for un-maximize */
    int sx, sy, sw, sh;

    bool dirty;       /* content needs a re-sweep into the content plane */
    bool frame_dirty; /* frame chrome needs a redraw (geometry/title/focus) */
    bool dead;        /* child exited; destroy after the current loop pass */

    /* inner cursor position (content-relative) and visibility, tracked from
     * the vterm movecursor / settermprop callbacks */
    int currow, curcol;
    bool cursor_visible;

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
} vp_dragkind;

/* Resize-edge bitmask. The top edge is the title bar (drag = move), but its
 * two corner cells (┌ ┐) still resize diagonally. */
#define RZ_LEFT   1
#define RZ_RIGHT  2
#define RZ_BOTTOM 4
#define RZ_TOP    8

typedef enum {
    SNAP_NONE = 0,
    SNAP_LEFT,
    SNAP_RIGHT,
    SNAP_TL,
    SNAP_TR,
    SNAP_BL,
    SNAP_BR,
} vp_snapzone;

/* In-app keybinding editor. A modal panel opened from a desktop launcher icon;
 * while open it captures all input. See settings.c. */
typedef struct Settings {
    bool open;
    bool capturing;          /* waiting for a keypress to assign to row `sel` */
    int  sel;                /* selected row: 0..ACTION_COUNT-1, then the toggle */
    int  scroll;             /* first visible row in the scrolling list */
    bool dirty;              /* panel needs a redraw */
    struct ncplane *icon;    /* desktop launcher (low z, above the background) */
    struct ncplane *panel;   /* the modal panel (created on open) */
    char status[128];        /* transient status/hint line */
} Settings;

typedef struct WM {
    struct notcurses *nc;
    struct ncplane *std; /* standard plane (background / desktop) */

    Window **wins;
    int nwins;
    int cap;
    int focused;  /* index into wins, or -1 */
    int next_id;

    vp_mode mode;

    unsigned scr_rows, scr_cols; /* screen dims in cells */

    /* taskbar */
    struct ncplane *taskbar;
    bool taskbar_dirty;

    /* mouse drag state */
    vp_dragkind drag;
    int drag_win;       /* id of window being dragged */
    int drag_off_x;     /* grab offset within frame (move) */
    int drag_off_y;
    int resize_edge;    /* RZ_* bitmask while DRAG_RESIZE */
    int drag_ax;        /* anchor: original right column (for left-edge resize) */
    int drag_ay;        /* anchor: original bottom row (for top-edge resize) */
    vp_snapzone snap_preview; /* currently-shown snap outline */
    struct ncplane *snap_plane;

    /* gpm */
    bool gpm_active;
    int gpm_fd;

    bool should_quit;

    VpConfig config;
    Settings settings;
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

/* True if a binding just changed in-app would be overridden by the manual
 * config section on the next load (so the change won't fully persist). With
 * is_toggle=true it checks the toggle key; otherwise it checks whether the
 * manual section governs action `act` or the chord (id,mods). */
bool config_manual_shadows(const VpConfig *live, bool is_toggle,
                           vp_action act, uint32_t id, unsigned mods);

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

/* Send a key / unicode codepoint to the child (encodes via libvterm). */
void vt_key_unichar(Window *w, uint32_t c, VTermModifier mod);
void vt_key_special(Window *w, VTermKey key, VTermModifier mod);

/* Translate a notcurses key event and forward it to the child. */
void vt_send_key(Window *w, const ncinput *ni);

/* Forward a mouse event to the inner app. */
void vt_mouse_move(Window *w, int row, int col, VTermModifier mod);
void vt_mouse_button(Window *w, int button, bool pressed, VTermModifier mod);

void vt_free(Window *w);

/* ------------------------------------------------------------------------- */
/* window.c                                                                  */
/* ------------------------------------------------------------------------- */

Window *window_create(WM *wm, int x, int y, int w, int h);
void window_destroy(WM *wm, Window *win);
void window_set_geometry(Window *win, int x, int y, int w, int h);
void window_draw_frame(WM *wm, Window *win);

/* ------------------------------------------------------------------------- */
/* wm.c                                                                      */
/* ------------------------------------------------------------------------- */

void wm_init(WM *wm, struct notcurses *nc);
void wm_add_window(WM *wm, Window *win);
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
int  keymap_action_count(void);
bool keymap_action_info(int row, vp_action *act, const char **label);
const char *keymap_action_name(vp_action act);
void keymap_format_chord(uint32_t id, unsigned mods, char *buf, size_t n);
/* Format the chord currently bound to `act` into buf; false (buf="(unbound)")
 * if nothing is bound to it. */
bool keymap_chord_for_action(const VpConfig *cfg, vp_action act, char *buf, size_t n);
/* Translate a live keypress to a chord; false for bare modifiers/locks/mouse. */
bool keymap_chord_from_input(const ncinput *ni, uint32_t *id, unsigned *mods);
/* Bind `act` to exactly (id,mods), dropping its previous chord(s). */
void keymap_rebind_action(VpConfig *cfg, vp_action act, uint32_t id, unsigned mods);
/* Remove every chord bound to `act` (leaving it unbound). */
void keymap_unbind_action(VpConfig *cfg, vp_action act);

/* ------------------------------------------------------------------------- */
/* settings.c                                                                */
/* ------------------------------------------------------------------------- */

/* Create the desktop launcher icon (kept just above the background). */
void settings_init(WM *wm);
/* Open / close the modal keybinding editor. Closing persists to the config. */
void settings_open(WM *wm);
void settings_close(WM *wm);
/* True if absolute cell (y,x) lands on the launcher icon. */
bool settings_icon_hit(WM *wm, int y, int x);
/* Modal input handlers (called only while the editor is open). */
void settings_handle_key(WM *wm, const ncinput *ni);
void settings_click(WM *wm, int btn, int y, int x);
void settings_scroll(WM *wm, int dir);
/* Redraw the panel if open and dirty (called from the WM render pass). */
void settings_render(WM *wm);
/* Destroy planes on shutdown. */
void settings_teardown(WM *wm);

void input_route_mouse(WM *wm, const ncinput *ni);

/* GPM lifecycle + event pump. */
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

#endif /* VIEWPOINT_H */
