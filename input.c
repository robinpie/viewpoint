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

static void toggle_mode(WM *wm)
{
    wm->mode = (wm->mode == MODE_INTERPRET) ? MODE_PASSTHROUGH : MODE_INTERPRET;
    wm->taskbar_dirty = true;
    for (int i = 0; i < wm->nwins; i++) {
        wm->wins[i]->frame_dirty = true; /* per-window mode indicator */
    }
}

bool input_handle_key(WM *wm, const ncinput *ni)
{
    /* The always-on toggle works in BOTH modes and is never forwarded. */
    if (ni->id == VP_TOGGLE_KEY) {
        toggle_mode(wm);
        return true;
    }
    /* Phase 3 layers the bare-chord keymap here. For now nothing else is
     * consumed; everything goes to the focused window. */
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
