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

/* input.c — input routing: keyboard (WM chords vs. forward-to-app) and mouse
 * (focus, title-bar drag-move, border-resize, content forwarding, taskbar),
 * plus the GPM merge for the bare Linux console.
 *
 * PHASE 2 implements: the always-on mode toggle, click-to-focus, and
 * title-bar drag-to-move. The chord keymap (phase 3), border resize, content
 * forwarding, snapping and GPM (phase 4) are layered on in later passes.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

/* Step sizes for keyboard move/resize chords. */
#define MOVE_STEP   2
#define RESIZE_STEP 1

static void toggle_mode(WM *wm)
{
    wm->mode = (wm->mode == MODE_INTERPRET) ? MODE_PASSTHROUGH : MODE_INTERPRET;
    wm->taskbar_dirty = true;
    for (int i = 0; i < wm->nwins; i++) {
        wm->wins[i]->frame_dirty = true; /* per-window mode indicator */
    }
    vp_log("mode=%s\n", wm->mode == MODE_PASSTHROUGH ? "PASSTHRU" : "INTERPRET");
}

/* The bare-chord action set. */
typedef enum {
    ACT_FOCUS_NEXT, ACT_FOCUS_PREV, ACT_CLOSE, ACT_NEW, ACT_MIN, ACT_MAXTOGGLE,
    ACT_MOVE_L, ACT_MOVE_R, ACT_MOVE_U, ACT_MOVE_D,
    ACT_RESIZE_L, ACT_RESIZE_R, ACT_RESIZE_U, ACT_RESIZE_D,
} vp_action;

/* The keymap, in one editable table. 'mods' is matched exactly against the
 * SHIFT/ALT/CTRL bits (lock bits are ignored). */
typedef struct {
    uint32_t id;
    unsigned mods;
    vp_action act;
} keychord;

#define A  NCKEY_MOD_ALT
#define AS (NCKEY_MOD_ALT | NCKEY_MOD_SHIFT)

static const keychord g_keymap[] = {
    { NCKEY_TAB,   A,  ACT_FOCUS_NEXT },
    { NCKEY_TAB,   AS, ACT_FOCUS_PREV },
    { NCKEY_F04,   A,  ACT_CLOSE },
    { 'n',         A,  ACT_NEW },
    { 'm',         A,  ACT_MIN },
    { 'x',         A,  ACT_MAXTOGGLE },
    { NCKEY_LEFT,  A,  ACT_MOVE_L },
    { NCKEY_RIGHT, A,  ACT_MOVE_R },
    { NCKEY_UP,    A,  ACT_MOVE_U },
    { NCKEY_DOWN,  A,  ACT_MOVE_D },
    { NCKEY_LEFT,  AS, ACT_RESIZE_L },
    { NCKEY_RIGHT, AS, ACT_RESIZE_R },
    { NCKEY_UP,    AS, ACT_RESIZE_U },
    { NCKEY_DOWN,  AS, ACT_RESIZE_D },
};

#undef A
#undef AS

/* Effective modifier mask. notcurses sets the NCKEY_MOD_* bits when a rich
 * keyboard protocol (e.g. Kitty) is active, but in legacy input mode it only
 * sets the deprecated ni->alt/shift/ctrl bools — e.g. Konsole's ESC-prefixed
 * Alt+key arrives as alt=1 with modifiers==0. Fold both sources together so
 * chords match regardless of which the terminal gave us. */
static unsigned eff_mods(const ncinput *ni)
{
    unsigned mods = ni->modifiers & (NCKEY_MOD_SHIFT | NCKEY_MOD_ALT | NCKEY_MOD_CTRL);
    if (ni->alt)   mods |= NCKEY_MOD_ALT;
    if (ni->shift) mods |= NCKEY_MOD_SHIFT;
    if (ni->ctrl)  mods |= NCKEY_MOD_CTRL;
    return mods;
}

static void do_action(WM *wm, vp_action act)
{
    switch (act) {
    case ACT_FOCUS_NEXT: wm_focus_next(wm, +1); break;
    case ACT_FOCUS_PREV: wm_focus_next(wm, -1); break;
    case ACT_CLOSE:      wm_close_focused(wm); break;
    case ACT_NEW:        wm_spawn_window(wm); break;
    case ACT_MIN:        wm_minimize(wm, wm_focused(wm)); break;
    case ACT_MAXTOGGLE:  wm_toggle_maximize(wm, wm_focused(wm)); break;
    case ACT_MOVE_L:     wm_move_focused(wm, -MOVE_STEP, 0); break;
    case ACT_MOVE_R:     wm_move_focused(wm, +MOVE_STEP, 0); break;
    case ACT_MOVE_U:     wm_move_focused(wm, 0, -MOVE_STEP); break;
    case ACT_MOVE_D:     wm_move_focused(wm, 0, +MOVE_STEP); break;
    case ACT_RESIZE_L:   wm_resize_focused(wm, -RESIZE_STEP, 0); break;
    case ACT_RESIZE_R:   wm_resize_focused(wm, +RESIZE_STEP, 0); break;
    case ACT_RESIZE_U:   wm_resize_focused(wm, 0, -RESIZE_STEP); break;
    case ACT_RESIZE_D:   wm_resize_focused(wm, 0, +RESIZE_STEP); break;
    }
}

bool input_handle_key(WM *wm, const ncinput *ni)
{
    /* The always-on toggle works in BOTH modes and is never forwarded. */
    if (ni->id == VP_TOGGLE_KEY) {
        toggle_mode(wm);
        return true;
    }

    /* In PASSTHROUGH, nothing else is interpreted — everything goes to the app. */
    if (wm->mode != MODE_INTERPRET) {
        return false;
    }

    unsigned mods = eff_mods(ni);

    /* Alt+1..9: focus/restore the window in taskbar slot N. */
    if (mods == NCKEY_MOD_ALT && ni->id >= '1' && ni->id <= '9') {
        int slot = (int)(ni->id - '1');
        if (slot < wm->nwins) {
            Window *win = wm->wins[slot];
            if (win->minimized) {
                wm_restore(wm, win);
            } else {
                wm_focus_window(wm, win);
            }
        }
        return true;
    }

    for (size_t i = 0; i < sizeof(g_keymap) / sizeof(g_keymap[0]); i++) {
        if (g_keymap[i].id == ni->id && g_keymap[i].mods == mods) {
            do_action(wm, g_keymap[i].act);
            return true;
        }
    }

    /* Any non-chord key in INTERPRET mode still goes to the focused window. */
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

/* ------------------------------------------------------------------------- */
/* Source-agnostic mouse model. notcurses and GPM both translate their native
 * events into mouse_event(), so the WM mouse logic lives in exactly one place. */
/* ------------------------------------------------------------------------- */

typedef enum {
    MEV_PRESS, MEV_RELEASE, MEV_MOTION, MEV_SCROLL_UP, MEV_SCROLL_DOWN,
} mev_type;

static VTermModifier to_vmod(unsigned mods)
{
    VTermModifier m = VTERM_MOD_NONE;
    if (mods & NCKEY_MOD_SHIFT) m |= VTERM_MOD_SHIFT;
    if (mods & NCKEY_MOD_ALT)   m |= VTERM_MOD_ALT;
    if (mods & NCKEY_MOD_CTRL)  m |= VTERM_MOD_CTRL;
    return m;
}

/* ----- title-bar hit testing (must mirror window_draw_frame's layout) ----- */

typedef enum { HIT_MOVE, HIT_MODE, HIT_MIN, HIT_MAX, HIT_CLOSE } titlehit;

static titlehit titlebar_hit(const Window *win, int relx)
{
    int w = win->w;
    if (w >= 6 && relx >= 1 && relx <= 3) {
        return HIT_MODE; /* the "[K]"/"[P]" indicator */
    }
    int btnx = w - 1 - 9; /* "[_][▢][x]" is 9 columns wide */
    if (btnx > 4) {
        if (relx >= btnx && relx < btnx + 3)         return HIT_MIN;
        if (relx >= btnx + 3 && relx < btnx + 6)     return HIT_MAX;
        if (relx >= btnx + 6 && relx < btnx + 9)     return HIT_CLOSE;
    }
    return HIT_MOVE;
}

/* Which resize edges (if any) the interior border cell (rely,relx) sits on.
 * rely==0 is the title bar and is handled before this is consulted. */
static int border_edge(const Window *win, int rely, int relx)
{
    int edge = 0;
    if (relx == 0)            edge |= RZ_LEFT;
    if (relx == win->w - 1)   edge |= RZ_RIGHT;
    if (rely == win->h - 1)   edge |= RZ_BOTTOM;
    return edge;
}

/* ----- snap preview ----- */

static void snap_geom(WM *wm, vp_snapzone z, int *gx, int *gy, int *gw, int *gh)
{
    int W = (int)wm->scr_cols;
    int H = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
    int hw = W / 2, hh = H / 2;
    switch (z) {
    case SNAP_LEFT:  *gx = 0;  *gy = 0;  *gw = hw;     *gh = H;      break;
    case SNAP_RIGHT: *gx = hw; *gy = 0;  *gw = W - hw; *gh = H;      break;
    case SNAP_TL:    *gx = 0;  *gy = 0;  *gw = hw;     *gh = hh;     break;
    case SNAP_TR:    *gx = hw; *gy = 0;  *gw = W - hw; *gh = hh;     break;
    case SNAP_BL:    *gx = 0;  *gy = hh; *gw = hw;     *gh = H - hh; break;
    case SNAP_BR:    *gx = hw; *gy = hh; *gw = W - hw; *gh = H - hh; break;
    default:         *gx = 0;  *gy = 0;  *gw = W;      *gh = H;      break;
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
    if (L) return top ? SNAP_TL : bot ? SNAP_BL : SNAP_LEFT;
    if (R) return top ? SNAP_TR : bot ? SNAP_BR : SNAP_RIGHT;
    return SNAP_NONE;
}

static void snap_hide(WM *wm)
{
    if (wm->snap_plane) {
        ncplane_move_yx(wm->snap_plane, VP_HIDDEN_Y, 0);
    }
    wm->snap_preview = SNAP_NONE;
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

    if (!wm->snap_plane) {
        ncplane_options o = {0};
        o.y = gy; o.x = gx;
        o.rows = (unsigned)gh; o.cols = (unsigned)gw;
        wm->snap_plane = ncplane_create(wm->std, &o);
        if (!wm->snap_plane) {
            return;
        }
        uint64_t base = 0; /* transparent interior so windows show through */
        ncchannels_set_fg_alpha(&base, NCALPHA_TRANSPARENT);
        ncchannels_set_bg_alpha(&base, NCALPHA_TRANSPARENT);
        ncplane_set_base(wm->snap_plane, "", 0, base);
    } else {
        ncplane_resize_simple(wm->snap_plane, (unsigned)gh, (unsigned)gw);
        ncplane_move_yx(wm->snap_plane, gy, gx);
    }

    struct ncplane *p = wm->snap_plane;
    ncplane_erase(p);
    ncplane_set_fg_rgb8(p, 0x60, 0xd0, 0xff);
    ncplane_set_bg_alpha(p, NCALPHA_TRANSPARENT);
    ncplane_putegc_yx(p, 0, 0, "╔", NULL);
    ncplane_putegc_yx(p, 0, gw - 1, "╗", NULL);
    ncplane_putegc_yx(p, gh - 1, 0, "╚", NULL);
    ncplane_putegc_yx(p, gh - 1, gw - 1, "╝", NULL);
    for (int c = 1; c < gw - 1; c++) {
        ncplane_putegc_yx(p, 0, c, "═", NULL);
        ncplane_putegc_yx(p, gh - 1, c, "═", NULL);
    }
    for (int r = 1; r < gh - 1; r++) {
        ncplane_putegc_yx(p, r, 0, "║", NULL);
        ncplane_putegc_yx(p, r, gw - 1, "║", NULL);
    }
    ncplane_move_top(p);
    wm->snap_preview = z;
}

/* ----- content-area forwarding to the inner app ----- */

static void content_forward(Window *win, mev_type t, int btn,
                            int y, int x, unsigned mods)
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
    case MEV_PRESS:       vt_mouse_button(win, btn, true, vm);  break;
    case MEV_RELEASE:     vt_mouse_button(win, btn, false, vm); break;
    case MEV_SCROLL_UP:   vt_mouse_button(win, 4, true, vm);    break;
    case MEV_SCROLL_DOWN: vt_mouse_button(win, 5, true, vm);    break;
    case MEV_MOTION:      break; /* the move above is the event */
    }
}

/* ----- drag updates ----- */

static void update_drag(WM *wm, int y, int x)
{
    Window *win = find_by_id(wm, wm->drag_win);
    if (!win) {
        wm->drag = DRAG_NONE;
        return;
    }

    if (wm->drag == DRAG_MOVE) {
        int nx = x - wm->drag_off_x;
        int ny = y - wm->drag_off_y;
        if (ny < 0) ny = 0;
        if (ny > (int)wm->scr_rows - 1) ny = (int)wm->scr_rows - 1;
        if (nx < -(win->w - 2)) nx = -(win->w - 2);
        if (nx > (int)wm->scr_cols - 2) nx = (int)wm->scr_cols - 2;
        window_set_geometry(win, nx, ny, win->w, win->h);
        /* Arm/disarm the edge snap based on the pointer, and keep the dragged
         * window above the outline so it stays visible. */
        snap_show(wm, snap_zone_at(wm, y, x));
        ncplane_move_family_top(win->frame);
        if (wm->taskbar) {
            ncplane_move_top(wm->taskbar);
        }
        return;
    }

    if (wm->drag == DRAG_RESIZE) {
        int nx = win->x, ny = win->y, nw = win->w, nh = win->h;
        if (wm->resize_edge & RZ_RIGHT)  nw = x - win->x + 1;
        if (wm->resize_edge & RZ_BOTTOM) nh = y - win->y + 1;
        if (wm->resize_edge & RZ_LEFT) {
            int right = wm->drag_ax;
            nx = x;
            nw = right - x + 1;
            if (nw < VP_MIN_W) { nw = VP_MIN_W; nx = right - nw + 1; }
            if (nx < 0) { nx = 0; nw = right + 1; }
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
        content_forward(wm_window_at(wm, y, x), MEV_PRESS, btn, y, x, mods);
        return;
    }
    if (taskbar_click(wm, y, x)) {
        return;
    }
    Window *win = wm_window_at(wm, y, x);
    if (!win) {
        return;
    }
    wm_focus_window(wm, win);

    int rely = y - win->y;
    int relx = x - win->x;

    if (rely == 0) {
        switch (titlebar_hit(win, relx)) {
        case HIT_MODE:  toggle_mode(wm); return;
        case HIT_MIN:   wm_minimize(wm, win); return;
        case HIT_MAX:   wm_toggle_maximize(wm, win); return;
        case HIT_CLOSE: win->dead = true; vp_log("close id=%d (button)\n", win->id); return;
        case HIT_MOVE:  break;
        }
        if (win->maximized) {
            win->maximized = false;
        }
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
        wm->drag_ax = win->x + win->w - 1; /* anchor right for left-edge drags */
        return;
    }

    /* Interior: forward to the app and lock subsequent motion/release to it. */
    wm->drag = DRAG_CONTENT;
    wm->drag_win = win->id;
    content_forward(win, MEV_PRESS, btn, y, x, mods);
}

static void mouse_release(WM *wm, int btn, int y, int x, unsigned mods)
{
    /* Apply the final move/resize from the release position. Terminals that
     * report button-held motion (drag) have already been updating live; those
     * that report only press+release (e.g. Konsole) get the whole drag applied
     * here, so the window snaps to the drop point either way. update_drag also
     * re-evaluates the snap zone from this final position. */
    if (wm->drag == DRAG_MOVE || wm->drag == DRAG_RESIZE) {
        update_drag(wm, y, x);
    }

    if (wm->drag == DRAG_MOVE && wm->snap_preview != SNAP_NONE) {
        Window *win = find_by_id(wm, wm->drag_win);
        if (win) {
            int gx, gy, gw, gh;
            snap_geom(wm, wm->snap_preview, &gx, &gy, &gw, &gh);
            win->maximized = false;
            window_set_geometry(win, gx, gy, gw, gh);
        }
    } else if (wm->drag == DRAG_CONTENT) {
        content_forward(find_by_id(wm, wm->drag_win), MEV_RELEASE, btn, y, x, mods);
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
    /* Bare hover: forward to the hovered app (only matters in MOUSE_MOVE mode). */
    content_forward(wm_window_at(wm, y, x), MEV_MOTION, 1, y, x, mods);
}

static void mouse_event(WM *wm, mev_type t, int btn, int y, int x, unsigned mods)
{
    switch (t) {
    case MEV_PRESS:       mouse_press(wm, btn, y, x, mods); break;
    case MEV_RELEASE:     mouse_release(wm, btn, y, x, mods); break;
    case MEV_MOTION:      mouse_motion(wm, y, x, mods); break;
    case MEV_SCROLL_UP:
    case MEV_SCROLL_DOWN:
        content_forward(wm_window_at(wm, y, x), t, btn, y, x, mods);
        break;
    }
}

/* ----- notcurses mouse source ----- */

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
        } else if (wm->drag != DRAG_NONE || ni->evtype == NCTYPE_REPEAT) {
            /* notcurses reports a held-button drag as a fresh PRESS, so once a
             * drag is in progress we treat any non-release button event as
             * motion (the REPEAT case covers terminals that do distinguish). */
            mouse_event(wm, MEV_MOTION, btn, y, x, mods);
        } else {
            mouse_event(wm, MEV_PRESS, btn, y, x, mods);
        }
        break;
    }
    case NCKEY_BUTTON4: mouse_event(wm, MEV_SCROLL_UP, 4, y, x, mods); break;
    case NCKEY_BUTTON5: mouse_event(wm, MEV_SCROLL_DOWN, 5, y, x, mods); break;
    case NCKEY_MOTION:  mouse_event(wm, MEV_MOTION, 0, y, x, mods); break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* GPM — the bare Linux console mouse. Active only when running on a real VT;  */
/* Gpm_Open fails (or returns the "under X/term" sentinel) otherwise, in which */
/* case we fall back to notcurses' own mouse decoding.                         */
/* ------------------------------------------------------------------------- */

#include <gpm.h>

void gpm_setup(WM *wm)
{
    wm->gpm_active = false;
    wm->gpm_fd = -1;

    Gpm_Connect conn;
    conn.eventMask   = (unsigned short)~0;  /* all event types */
    conn.defaultMask = 0;                   /* don't let anything pass to the VT */
    conn.minMod      = 0;
    conn.maxMod      = (unsigned short)~0;
    conn.pid         = 0;
    conn.vc          = 0;                    /* current virtual console */

    int fd = Gpm_Open(&conn, 0);
    if (fd >= 0) {
        wm->gpm_active = true;
        wm->gpm_fd = fd;
        vp_log("gpm: active fd=%d\n", fd);
    } else {
        vp_log("gpm: unavailable (%d); using notcurses mouse\n", fd);
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
        /* EOF/error: the GPM server went away — drop back to no console mouse. */
        gpm_teardown(wm);
        return;
    }

    int y = ev.y - 1; /* GPM is 1-based */
    int x = ev.x - 1;
    if (y < 0) y = 0;
    if (x < 0) x = 0;

    unsigned mods = 0; /* GPM modifier bits aren't mapped; chords stay keyboard */
    int btn = (ev.buttons & GPM_B_RIGHT) ? 3 :
              (ev.buttons & GPM_B_MIDDLE) ? 2 : 1;

    if (ev.wdy > 0) {
        mouse_event(wm, MEV_SCROLL_UP, 4, y, x, mods);
    } else if (ev.wdy < 0) {
        mouse_event(wm, MEV_SCROLL_DOWN, 5, y, x, mods);
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
