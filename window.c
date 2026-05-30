/* window.c — per-window lifecycle and frame (chrome) drawing.
 *
 * A window is a decoration ("frame") plane that parents a "content" plane.
 * The frame draws the border ring + title bar; the content plane sits in the
 * 1-cell interior and shows the child's terminal grid. Because the content
 * plane is bound to the frame, moving/raising the frame carries the content
 * with it.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>

/* Border/title-bar colors. */
#define COL_FOCUS_FG   0xff, 0xff, 0xff
#define COL_FOCUS_BG   0x20, 0x40, 0x80
#define COL_UNFOCUS_FG 0xc0, 0xc0, 0xc0
#define COL_UNFOCUS_BG 0x30, 0x30, 0x30

static void clampgeo(int *w, int *h)
{
    if (*w < VP_MIN_W) *w = VP_MIN_W;
    if (*h < VP_MIN_H) *h = VP_MIN_H;
}

Window *window_create(WM *wm, int x, int y, int w, int h)
{
    clampgeo(&w, &h);

    Window *win = calloc(1, sizeof(*win));
    if (!win) {
        return NULL;
    }
    win->id = wm->next_id++;
    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->rows = h - 2 * VP_BORDER;
    win->cols = w - 2 * VP_BORDER;
    win->pty = -1;
    win->cursor_visible = true;
    snprintf(win->title, sizeof(win->title), "shell %d", win->id);

    /* Frame plane bound to the desktop (std) plane. */
    ncplane_options fopts = {0};
    fopts.y = y;
    fopts.x = x;
    fopts.rows = (unsigned)h;
    fopts.cols = (unsigned)w;
    fopts.userptr = win; /* lets wm_window_at map a plane back to its Window */
    win->frame = ncplane_create(wm->std, &fopts);
    if (!win->frame) {
        free(win);
        return NULL;
    }

    /* Content plane in the 1-cell interior, bound to the frame. */
    ncplane_options copts = {0};
    copts.y = VP_BORDER;
    copts.x = VP_BORDER;
    copts.rows = (unsigned)win->rows;
    copts.cols = (unsigned)win->cols;
    win->content = ncplane_create(win->frame, &copts);
    if (!win->content) {
        ncplane_destroy(win->frame);
        free(win);
        return NULL;
    }

    vt_init(win);

    win->child = pty_spawn(win->rows, win->cols, &win->pty);
    if (win->child < 0) {
        vt_free(win);
        ncplane_destroy(win->content);
        ncplane_destroy(win->frame);
        free(win);
        return NULL;
    }

    win->dirty = true;
    win->frame_dirty = true;
    return win;
}

void window_destroy(WM *wm, Window *win)
{
    (void)wm;
    if (!win) {
        return;
    }
    if (win->child > 0) {
        kill(win->child, SIGHUP);
    }
    vt_free(win);
    if (win->pty >= 0) {
        close(win->pty);
        win->pty = -1;
    }
    /* Destroying the frame also drops bound children, but be explicit. */
    if (win->content) {
        ncplane_destroy(win->content);
        win->content = NULL;
    }
    if (win->frame) {
        ncplane_destroy(win->frame);
        win->frame = NULL;
    }
    free(win);
}

void window_set_geometry(Window *win, int x, int y, int w, int h)
{
    clampgeo(&w, &h);
    bool resized = (w != win->w || h != win->h);

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;

    ncplane_move_yx(win->frame, y, x);
    vp_log("geom id=%d x=%d y=%d w=%d h=%d\n", win->id, x, y, w, h);

    if (resized) {
        ncplane_resize_simple(win->frame, (unsigned)h, (unsigned)w);
        int rows = h - 2 * VP_BORDER;
        int cols = w - 2 * VP_BORDER;
        vt_resize(win, rows, cols); /* resizes content plane + emulator */
        pty_set_winsize(win->pty, rows, cols);
    }
    win->frame_dirty = true;
}

/* Draw the border ring and title bar onto the frame plane. The content plane
 * (a child) covers the interior, so we only paint the perimeter. */
void window_draw_frame(WM *wm, Window *win)
{
    struct ncplane *f = win->frame;
    bool focused = (wm_focused(wm) == win);
    int w = win->w;
    int h = win->h;

    if (focused) {
        ncplane_set_fg_rgb8(f, COL_FOCUS_FG);
        ncplane_set_bg_rgb8(f, COL_FOCUS_BG);
    } else {
        ncplane_set_fg_rgb8(f, COL_UNFOCUS_FG);
        ncplane_set_bg_rgb8(f, COL_UNFOCUS_BG);
    }
    ncplane_set_styles(f, NCSTYLE_NONE);

    /* Side borders and bottom (interior is hidden by the content plane). */
    for (int row = 1; row < h - 1; row++) {
        ncplane_putegc_yx(f, row, 0, "│", NULL);       /* │ */
        ncplane_putegc_yx(f, row, w - 1, "│", NULL);
    }
    ncplane_putegc_yx(f, h - 1, 0, "└", NULL);          /* └ */
    for (int col = 1; col < w - 1; col++) {
        ncplane_putegc_yx(f, h - 1, col, "─", NULL);    /* ─ */
    }
    ncplane_putegc_yx(f, h - 1, w - 1, "┘", NULL);      /* ┘ */

    /* Title bar = top row. Layout:
     *   ┌ [P]/[K] title ........... [_][▢][x] ┐
     * The mode toggle indicator reflects the GLOBAL mode (clickable later). */
    ncplane_putegc_yx(f, 0, 0, "┌", NULL);              /* ┌ */
    for (int col = 1; col < w - 1; col++) {
        ncplane_putegc_yx(f, 0, col, "─", NULL);        /* ─ fill */
    }
    ncplane_putegc_yx(f, 0, w - 1, "┐", NULL);          /* ┐ */

    /* Mode indicator just inside the left corner. */
    const char *modi = (wm->mode == MODE_PASSTHROUGH) ? "[P]" : "[K]";
    if (w >= 6) {
        ncplane_putstr_yx(f, 0, 1, modi);
    }

    /* Window buttons near the right corner: minimize, maximize, close. */
    const char *btns = "[_][▢][x]"; /* [_][▢][x] : 9 columns wide */
    int btnw = 9;
    int btnx = w - 1 - btnw;
    if (btnx > 4) {
        ncplane_putstr_yx(f, 0, btnx, btns);
    } else {
        btnx = w; /* no room; title may use full width */
    }

    /* Title text between the mode indicator and the buttons. */
    int tstart = (w >= 6) ? 5 : 1; /* after "[K] " */
    int tend = (btnx < w) ? btnx - 1 : w - 1;
    int avail = tend - tstart;
    if (avail > 0) {
        char buf[VP_TITLE_MAX + 8];
        snprintf(buf, sizeof(buf), "%d:%s", win->id, win->title);
        if ((int)strlen(buf) > avail) {
            buf[avail] = '\0';
        }
        ncplane_putstr_yx(f, 0, tstart, buf);
    }

    win->frame_dirty = false;
}
