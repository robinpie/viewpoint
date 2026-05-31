/* wm.c — window-manager core: the window list, focus, z-order/stacking,
 * layout (move/resize/min/max), spawning/closing, and the render pass.
 *
 * notcurses' plane z-order is the source of truth for stacking; we raise the
 * focused window's frame (and its bound content) to the top.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>

/* Off-screen parking row for minimized windows. */
#define VP_HIDDEN_Y 100000

void wm_init(WM *wm, struct notcurses *nc)
{
    memset(wm, 0, sizeof(*wm));
    wm->nc = nc;
    wm->std = notcurses_stdplane(nc);
    wm->focused = -1;
    wm->next_id = 1;
    wm->mode = MODE_INTERPRET;
    wm->gpm_fd = -1;
    notcurses_stddim_yx(nc, &wm->scr_rows, &wm->scr_cols);

    /* Desktop background: a dim field so floating windows stand out. */
    uint64_t ch = 0;
    ncchannels_set_fg_rgb8(&ch, 0x40, 0x44, 0x4c);
    ncchannels_set_bg_rgb8(&ch, 0x10, 0x12, 0x16);
    ncplane_set_base(wm->std, "·", 0, ch);
}

void wm_add_window(WM *wm, Window *win)
{
    if (wm->nwins == wm->cap) {
        int ncap = wm->cap ? wm->cap * 2 : 8;
        Window **n = realloc(wm->wins, (size_t)ncap * sizeof(*n));
        if (!n) {
            return;
        }
        wm->wins = n;
        wm->cap = ncap;
    }
    wm->wins[wm->nwins++] = win;
    wm->taskbar_dirty = true;
}

void wm_remove_window(WM *wm, Window *win)
{
    int idx = wm_index_of(wm, win);
    if (idx < 0) {
        return;
    }
    for (int i = idx; i < wm->nwins - 1; i++) {
        wm->wins[i] = wm->wins[i + 1];
    }
    wm->nwins--;
    wm->taskbar_dirty = true;

    /* Fix up the focused index. */
    if (wm->nwins == 0) {
        wm->focused = -1;
    } else if (wm->focused == idx) {
        /* focus the now-topmost remaining window */
        wm->focused = -1;
        /* pick the highest-z non-minimized window */
        for (struct ncplane *p = notcurses_top(wm->nc); p; p = ncplane_below(p)) {
            Window *cand = ncplane_userptr(p);
            for (int i = 0; i < wm->nwins; i++) {
                if (wm->wins[i] == cand && !cand->minimized) {
                    wm_focus_index(wm, i);
                    break;
                }
            }
            if (wm->focused >= 0) {
                break;
            }
        }
        if (wm->focused < 0 && wm->nwins > 0) {
            wm_focus_index(wm, wm->nwins - 1);
        }
    } else if (wm->focused > idx) {
        wm->focused--;
    }
}

int wm_index_of(WM *wm, const Window *win)
{
    for (int i = 0; i < wm->nwins; i++) {
        if (wm->wins[i] == win) {
            return i;
        }
    }
    return -1;
}

Window *wm_focused(WM *wm)
{
    if (wm->focused < 0 || wm->focused >= wm->nwins) {
        return NULL;
    }
    return wm->wins[wm->focused];
}

void wm_focus_index(WM *wm, int idx)
{
    if (idx < 0 || idx >= wm->nwins) {
        return;
    }
    Window *prev = wm_focused(wm);
    Window *win = wm->wins[idx];

    wm->focused = idx;
    vp_log("focus id=%d idx=%d\n", win->id, idx);
    /* Raise the focused frame and its bound content to the top. */
    ncplane_move_family_top(win->frame);

    if (prev && prev != win) {
        prev->frame_dirty = true;
    }
    win->frame_dirty = true;
    wm->taskbar_dirty = true;

    /* Keep the taskbar above all windows. */
    if (wm->taskbar) {
        ncplane_move_top(wm->taskbar);
    }
}

void wm_focus_window(WM *wm, Window *win)
{
    int idx = wm_index_of(wm, win);
    if (idx >= 0) {
        wm_focus_index(wm, idx);
    }
}

void wm_focus_next(WM *wm, int dir)
{
    if (wm->nwins == 0) {
        return;
    }
    int start = wm->focused < 0 ? 0 : wm->focused;
    for (int step = 1; step <= wm->nwins; step++) {
        int i = ((start + dir * step) % wm->nwins + wm->nwins) % wm->nwins;
        if (!wm->wins[i]->minimized) {
            wm_focus_index(wm, i);
            return;
        }
    }
}

Window *wm_spawn_window(WM *wm)
{
    /* Cascade new windows from the top-left. */
    int n = wm->nwins;
    int x = 2 + (n % 6) * 4;
    int y = 1 + (n % 6) * 2;
    int w = (int)wm->scr_cols * 2 / 3;
    int h = (int)wm->scr_rows * 2 / 3;
    if (w < VP_MIN_W * 2) w = (int)wm->scr_cols - x - 1;
    if (h < VP_MIN_H * 2) h = (int)wm->scr_rows - y - 1;

    Window *win = window_create(wm, x, y, w, h);
    if (!win) {
        return NULL;
    }
    wm_add_window(wm, win);
    wm_clamp_onscreen(wm, win);
    wm_focus_window(wm, win);
    vp_log("spawn id=%d nwins=%d\n", win->id, wm->nwins);
    return win;
}

void wm_close_focused(WM *wm)
{
    Window *win = wm_focused(wm);
    if (win) {
        win->dead = true; /* destroyed at end of the loop pass */
        vp_log("close id=%d\n", win->id);
    }
}

void wm_minimize(WM *wm, Window *win)
{
    if (!win || win->minimized) {
        return;
    }
    win->minimized = true;
    vp_log("minimize id=%d\n", win->id);
    /* Park the frame off-screen (content rides along as a bound child). */
    ncplane_move_yx(win->frame, VP_HIDDEN_Y, win->x);

    if (wm_focused(wm) == win) {
        /* shift focus to the next visible window */
        wm->focused = -1;
        for (int i = 0; i < wm->nwins; i++) {
            if (!wm->wins[i]->minimized) {
                wm_focus_index(wm, i);
                break;
            }
        }
    }
    wm->taskbar_dirty = true;
}

void wm_restore(WM *wm, Window *win)
{
    if (!win || !win->minimized) {
        return;
    }
    win->minimized = false;
    vp_log("restore id=%d\n", win->id);
    ncplane_move_yx(win->frame, win->y, win->x);
    win->frame_dirty = true;
    win->dirty = true;
    wm_focus_window(wm, win);
    wm->taskbar_dirty = true;
}

void wm_toggle_maximize(WM *wm, Window *win)
{
    if (!win) {
        return;
    }
    int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
    vp_log("maxtoggle id=%d was_max=%d\n", win->id, win->maximized);
    if (!win->maximized) {
        win->sx = win->x;
        win->sy = win->y;
        win->sw = win->w;
        win->sh = win->h;
        win->maximized = true;
        window_set_geometry(win, 0, 0, (int)wm->scr_cols, avail_h);
    } else {
        win->maximized = false;
        window_set_geometry(win, win->sx, win->sy, win->sw, win->sh);
    }
    win->dirty = true;
}

void wm_move_focused(WM *wm, int dx, int dy)
{
    Window *win = wm_focused(wm);
    if (!win || win->minimized) {
        return;
    }
    if (win->maximized) {
        win->maximized = false; /* moving un-maximizes */
    }
    window_set_geometry(win, win->x + dx, win->y + dy, win->w, win->h);
    wm_clamp_onscreen(wm, win);
}

void wm_resize_focused(WM *wm, int dw, int dh)
{
    Window *win = wm_focused(wm);
    if (!win || win->minimized) {
        return;
    }
    if (win->maximized) {
        win->maximized = false;
    }
    window_set_geometry(win, win->x, win->y, win->w + dw, win->h + dh);
    wm_clamp_onscreen(wm, win);
}

void wm_clamp_onscreen(WM *wm, Window *win)
{
    int maxw = (int)wm->scr_cols;
    int maxh = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);

    int w = win->w, h = win->h;
    if (w > maxw) w = maxw;
    if (h > maxh) h = maxh;

    int x = win->x, y = win->y;
    if (x + w > maxw) x = maxw - w;
    if (y + h > maxh) y = maxh - h;
    if (x < 0) x = 0;
    if (y < 0) y = 0;

    if (x != win->x || y != win->y || w != win->w || h != win->h) {
        window_set_geometry(win, x, y, w, h);
    }
}

void wm_handle_resize(WM *wm)
{
    notcurses_refresh(wm->nc, NULL, NULL);
    notcurses_stddim_yx(wm->nc, &wm->scr_rows, &wm->scr_cols);

    if (wm->taskbar) {
        taskbar_reflow(wm);
    }
    for (int i = 0; i < wm->nwins; i++) {
        Window *win = wm->wins[i];
        if (win->maximized) {
            int avail_h = (int)wm->scr_rows - (wm->taskbar ? 1 : 0);
            window_set_geometry(win, 0, 0, (int)wm->scr_cols, avail_h);
        } else if (!win->minimized) {
            wm_clamp_onscreen(wm, win);
        }
        win->dirty = true;
        win->frame_dirty = true;
    }
    wm->taskbar_dirty = true;
}

Window *wm_window_at(WM *wm, int y, int x)
{
    /* Walk the pile from top to bottom; first frame that contains (y,x) and
     * isn't minimized wins. */
    for (struct ncplane *p = notcurses_top(wm->nc); p; p = ncplane_below(p)) {
        Window *win = ncplane_userptr(p);
        if (!win) {
            continue;
        }
        /* userptr is only set on frame planes; confirm it's a live window */
        if (win->frame != p || win->minimized) {
            continue;
        }
        if (y >= win->y && y < win->y + win->h &&
            x >= win->x && x < win->x + win->w) {
            return win;
        }
    }
    return NULL;
}

void wm_render(WM *wm)
{
    for (int i = 0; i < wm->nwins; i++) {
        Window *win = wm->wins[i];
        if (win->minimized) {
            continue;
        }
        if (win->frame_dirty) {
            window_draw_frame(wm, win);
        }
        if (win->dirty) {
            vt_render(win);
        }
    }

    if (wm->taskbar && wm->taskbar_dirty) {
        taskbar_draw(wm);
    }

    /* Inner cursor only for the focused window. */
    Window *f = wm_focused(wm);
    if (f && !f->minimized && f->cursor_visible &&
        f->currow >= 0 && f->currow < f->rows &&
        f->curcol >= 0 && f->curcol < f->cols) {
        int ay, ax;
        ncplane_abs_yx(f->content, &ay, &ax);
        notcurses_cursor_enable(wm->nc, ay + f->currow, ax + f->curcol);
    } else {
        notcurses_cursor_disable(wm->nc);
    }

    notcurses_render(wm->nc);
}
