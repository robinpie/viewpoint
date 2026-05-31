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

    unsigned mods = ni->modifiers & (NCKEY_MOD_SHIFT | NCKEY_MOD_ALT | NCKEY_MOD_CTRL);

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

static void begin_drag_or_focus(WM *wm, int y, int x)
{
    Window *win = wm_window_at(wm, y, x);
    if (!win) {
        return;
    }
    wm_focus_window(wm, win);

    if (y == win->y) {
        /* Title-bar row: start a move drag, remembering the grab offset. */
        wm->drag = DRAG_MOVE;
        wm->drag_win = win->id;
        wm->drag_off_x = x - win->x;
        wm->drag_off_y = y - win->y;
    }
    /* Border-drag resize and content forwarding arrive in phase 4. */
}

static void update_drag(WM *wm, int y, int x)
{
    if (wm->drag == DRAG_NONE) {
        return;
    }
    Window *win = find_by_id(wm, wm->drag_win);
    if (!win) {
        wm->drag = DRAG_NONE;
        return;
    }
    if (wm->drag == DRAG_MOVE) {
        int nx = x - wm->drag_off_x;
        int ny = y - wm->drag_off_y;
        /* Keep the title bar reachable: clamp the origin loosely on-screen. */
        if (ny < 0) ny = 0;
        if (ny > (int)wm->scr_rows - 1) ny = (int)wm->scr_rows - 1;
        if (nx < -(win->w - 2)) nx = -(win->w - 2);
        if (nx > (int)wm->scr_cols - 2) nx = (int)wm->scr_cols - 2;
        window_set_geometry(win, nx, ny, win->w, win->h);
    }
}

void input_route_mouse(WM *wm, const ncinput *ni)
{
    int y = ni->y;
    int x = ni->x;

    if (ni->id == NCKEY_BUTTON1) {
        if (ni->evtype == NCTYPE_RELEASE) {
            wm->drag = DRAG_NONE;
            return;
        }
        if (wm->drag != DRAG_NONE) {
            update_drag(wm, y, x); /* button held & moving */
            return;
        }
        /* Fresh press: taskbar first, then windows. */
        if (taskbar_click(wm, y, x)) {
            return;
        }
        begin_drag_or_focus(wm, y, x);
        return;
    }

    if (ni->id == NCKEY_MOTION) {
        update_drag(wm, y, x);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* GPM (real implementation in phase 4)                                      */
/* ------------------------------------------------------------------------- */

void gpm_setup(WM *wm)
{
    wm->gpm_active = false;
    wm->gpm_fd = -1;
}

void gpm_pump(WM *wm)
{
    (void)wm;
}

void gpm_teardown(WM *wm)
{
    (void)wm;
}
