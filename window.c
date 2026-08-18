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

/* window.c - per-window lifecycle and frame (chrome) drawing.
 *
 * A window is a decoration ("frame") layer with a "content" sublayer showing
 * the child's terminal grid; as a sublayer, content (and any anchored image)
 * moves, raises and hides with the frame as one thing to the compositor.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <signal.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <time.h>

static void clampgeo(int *w, int *h)
{
	if (*w < VP_MIN_W)
		*w = VP_MIN_W;
	if (*h < VP_MIN_H)
		*h = VP_MIN_H;
}

static void frame_paint(struct ncplane *p, bool full, void *user);
static void content_paint(struct ncplane *p, bool full, void *user);

void window_damage_frame(Window *win)
{
	if (win) {
		comp_layer_damage(win->frame);
	}
}

/* `full` forces a from-scratch sweep of the grid rather than an incremental one
 * over the rows libvterm reported as damaged. */
void window_damage_content(Window *win, bool full)
{
	if (!win) {
		return;
	}
	if (full) {
		win->dmg_all = true;
	}
	comp_layer_damage(win->content);
}

Window *window_create_attached(WM *wm, int id, int x, int y, int w, int h)
{
	clampgeo(&w, &h);

	Window *win = calloc(1, sizeof(*win));
	if (!win) {
		return NULL;
	}
	win->id = id;
	if (wm->next_id <= id) {
		wm->next_id = id + 1;
	}
	win->wm = wm; /* set early: vt_free in the error paths below needs it */
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;
	win->rows = h - 2 * VP_BORDER;
	win->cols = w - 2 * VP_BORDER;
	win->pty = -1;
	win->cursor_visible = true;
	win->sb_max = wm->config.scrollback_max;
	snprintf(win->title, sizeof(win->title), "shell %d", win->id);

	/* The frame is the window as far as the compositor is concerned: it
	 * carries the band/order that decides stacking, and the user pointer
	 * that turns a hit test back into a Window. */
	vp_rect fr = { y, x, h, w };
	win->frame =
		comp_layer_new(wm->comp, VP_BAND_WINDOW, fr, frame_paint, win);
	if (!win->frame) {
		free(win);
		return NULL;
	}

	vp_rect cr = { VP_BORDER, VP_BORDER, win->rows, win->cols };
	win->content = comp_sublayer_new(win->frame, cr, content_paint, win);
	if (!win->content) {
		comp_layer_destroy(win->frame);
		free(win);
		return NULL;
	}

	vt_init(win);
	return win;
}

Window *window_create(WM *wm, int x, int y, int w, int h)
{
	return window_create_attached(wm, wm->next_id++, x, y, w, h);
}

void window_destroy(WM *wm, Window *win)
{
	(void)wm;
	if (!win) {
		return;
	}
	if (win->pty >= 0 && win->child > 0) {
		/* forkpty() put the child in its own session as group leader, so its
         * pgid equals its pid. SIGHUP the whole group, not just the shell:
         * that reaches jobs the shell started in its group. */
		killpg(win->child, SIGHUP);
	}
	vt_free(win);
	if (win->pty >= 0) {
		/* Hang up the slave by closing the master. The kernel then delivers
         * SIGHUP to the terminal's foreground process group, which covers a
         * full-screen app (vim, less, …) running in its own group — something
         * the killpg above, aimed at the shell's group, would miss. */
		close(win->pty);
		win->pty = -1;
	}
	/* One call takes the whole family: the content sublayer and every image
	 * anchored in it are owned by the frame. */
	comp_layer_destroy(win->frame);
	win->frame = NULL;
	win->content = NULL;
	free(win);
}

void window_set_geometry(Window *win, int x, int y, int w, int h)
{
	clampgeo(&w, &h);
	bool resized = (w != win->w || h != win->h);

	/* Moving a window is just a geometry change now. It used to have to tear
	 * down and re-blit every image in every window around it, because nothing
	 * knew which bitmaps the move would occlude; the compositor works that
	 * out for itself from the new scene. */
	win->x = x;
	win->y = y;
	win->w = w;
	win->h = h;

	comp_layer_move(win->frame, y, x);
	vp_log("geom id=%d x=%d y=%d w=%d h=%d\n", win->id, x, y, w, h);

	if (resized) {
		comp_layer_resize(win->frame, h, w);
		int rows = h - 2 * VP_BORDER;
		int cols = w - 2 * VP_BORDER;
		vt_resize(win, rows,
			  cols); /* resizes content layer + emulator */
		if (win->pty >= 0) {
			pty_set_winsize(win->pty, rows, cols);
		} else {
			session_resize(win, rows, cols);
		}
		/* Every resize route lands here, so the size indicator only has to
		 * be armed in this one place (it decides for itself whether this
		 * window is the one the user is working on). */
		sizeosd_show(win->wm, win);
	}
	window_damage_frame(win);
}

static const char *login_name(void)
{
	static char name[64];
	if (!name[0]) {
		struct passwd *pw = getpwuid(getuid());
		const char *u = (pw && pw->pw_name) ? pw->pw_name :
						      getenv("USER");
		snprintf(name, sizeof(name), "%s", (u && *u) ? u : "user");
	}
	return name;
}

static const char *host_name(void)
{
	static char host[65];
	if (!host[0]) {
		if (gethostname(host, sizeof(host) - 1) != 0 || !host[0]) {
			snprintf(host, sizeof(host), "localhost");
		}
		host[sizeof(host) - 1] = '\0';
	}
	return host;
}

static void proc_cwd(pid_t pid, char *out, size_t outlen)
{
	char link[64];
	char cwd[PATH_MAX];
	snprintf(link, sizeof(link), "/proc/%ld/cwd", (long)pid);
	ssize_t n = readlink(link, cwd, sizeof(cwd) - 1);
	if (n < 0) {
		out[0] = '\0';
		return;
	}
	cwd[n] = '\0';

	const char *home = getenv("HOME");
	if (home && *home) {
		size_t hl = strlen(home);
		if (strncmp(cwd, home, hl) == 0 &&
		    (cwd[hl] == '/' || cwd[hl] == '\0')) {
			snprintf(out, outlen, "~%s", cwd + hl);
			return;
		}
	}
	snprintf(out, outlen, "%s", cwd);
}

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

bool window_refresh_title(Window *win)
{
	if (win->pty < 0 || win->child <= 0) {
		return false;
	}

	/* Deriving the title hits /proc; throttle to one poll per VP_TITLE_POLL_MS
	 * so a window streaming output doesn't re-poll on every render pass. */
	uint64_t now = mono_ns();
	if (now < win->title_poll_ns) {
		return false;
	}
	win->title_poll_ns = now + (uint64_t)VP_TITLE_POLL_MS * 1000000ull;

	/* Foreground process group of the slave terminal; its pgid is the leader's
     * pid, which equals the shell's pid exactly when no command is running. */
	pid_t fg = tcgetpgrp(win->pty);
	if (fg <= 0) {
		return false; /* can't tell right now; leave the title as-is */
	}

	/* Assemble into a generous buffer, then copy in with an explicit precision
     * bound: cwd paths can far exceed VP_TITLE_MAX, and the title is meant to be
     * truncated to fit the bar anyway. */
	char next[PATH_MAX + 256];
	if (fg == win->child) {
		char loc[PATH_MAX];
		proc_cwd(win->child, loc, sizeof(loc));
		if (loc[0]) {
			snprintf(next, sizeof(next), "%s@%s:%s", login_name(),
				 host_name(), loc);
		} else {
			snprintf(next, sizeof(next), "%s@%s", login_name(),
				 host_name());
		}
	} else {
		char path[64];
		char comm[64];
		snprintf(path, sizeof(path), "/proc/%ld/comm", (long)fg);
		FILE *fp = fopen(path, "r");
		if (!fp) {
			return false; /* process vanished between calls; keep current */
		}
		char *got = fgets(comm, sizeof(comm), fp);
		fclose(fp);
		if (!got) {
			return false;
		}
		comm[strcspn(comm, "\n")] = '\0';
		if (!comm[0]) {
			return false;
		}
		snprintf(next, sizeof(next), "%s", comm);
	}

	/* Truncate before comparing: win->title is already truncated, so comparing
	 * against the untruncated `next` would report a spurious change on every
	 * call once the title exceeds VP_TITLE_MAX. */
	char fitted[VP_TITLE_MAX];
	snprintf(fitted, sizeof(fitted), "%.*s", (int)sizeof(fitted) - 1, next);
	if (strcmp(fitted, win->title) == 0) {
		return false;
	}
	memcpy(win->title, fitted, sizeof(fitted));
	window_damage_frame(win);
	return true;
}

/* Draw the border ring and title bar onto the frame plane. The content plane
 * (a child) covers the interior, so we only paint the perimeter. */
static void frame_paint(struct ncplane *f, bool full, void *user)
{
	(void)full; /* the chrome is cheap and always redrawn in full */
	Window *win = user;
	WM *wm = win->wm;
	bool focused = (wm_focused(wm) == win);
	int w = win->w;
	int h = win->h;

	if (focused) {
		vp_setfg(f, wm->theme.win_focus_fg);
		vp_setbg(f, wm->theme.win_focus_bg);
	} else {
		vp_setfg(f, wm->theme.win_unfocus_fg);
		vp_setbg(f, wm->theme.win_unfocus_bg);
	}
	ncplane_set_styles(f, NCSTYLE_NONE);

	for (int row = 1; row < h - 1; row++) {
		ncplane_putegc_yx(f, row, 0, "│", NULL); /* │ */
		ncplane_putegc_yx(f, row, w - 1, "│", NULL);
	}
	ncplane_putegc_yx(f, h - 1, 0, "└", NULL); /* └ */
	for (int col = 1; col < w - 1; col++) {
		ncplane_putegc_yx(f, h - 1, col, "─", NULL); /* ─ */
	}
	ncplane_putegc_yx(f, h - 1, w - 1, "┘", NULL); /* ┘ */

	/* Title bar = top row. Layout:
     *   ┌ title ........... [_][▢][x] ┐ */
	ncplane_putegc_yx(f, 0, 0, "┌", NULL); /* ┌ */
	for (int col = 1; col < w - 1; col++) {
		ncplane_putegc_yx(f, 0, col, "─", NULL); /* ─ fill */
	}
	ncplane_putegc_yx(f, 0, w - 1, "┐", NULL); /* ┐ */

	const char *btns = "[_][▢][x]"; /* [_][▢][x] : 9 columns wide */
	int btnw = 9;
	int btnx = w - 1 - btnw;
	if (btnx > 4) {
		ncplane_putstr_yx(f, 0, btnx, btns);
	} else {
		btnx = w; /* no room; title may use full width */
	}

	int tstart = 1; /* just inside the ┌ corner */
	int tend = (btnx < w) ? btnx - 1 : w - 1;
	int avail = tend - tstart;
	if (avail > 0) {
		char buf[VP_TITLE_MAX + 32];
		if (win->sb_offset > 0) {
			snprintf(buf, sizeof(buf), "%d:[↑%d] %s", win->id,
				 win->sb_offset, win->title);
		} else {
			snprintf(buf, sizeof(buf), "%d:%s", win->id,
				 win->title);
		}
		if ((int)strlen(buf) > avail) {
			buf[avail] = '\0';
		}
		ncplane_putstr_yx(f, 0, tstart, buf);
	}
}

/* Painter for the terminal grid. `full` says a pixel bitmap that overlapped
 * this window has just been torn down, so the cells it had annihilated have to
 * be repainted - the emulator's own row damage knows nothing about that. */
static void content_paint(struct ncplane *p, bool full, void *user)
{
	(void)p;
	Window *win = user;
	if (full) {
		win->dmg_all = true;
	}
	/* Images are pinned to absolute scrollback rows; resolve those anchors
	 * against the current view before the grid is swept over them. */
	sixel_sync(win);
	vt_render(win);
}
