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
#include <pwd.h>
#include <limits.h>
#include <time.h>

/* Border/title-bar colors. */
#define COL_FOCUS_FG 0xff, 0xff, 0xff
#define COL_FOCUS_BG 0x20, 0x40, 0x80
#define COL_UNFOCUS_FG 0xc0, 0xc0, 0xc0
#define COL_UNFOCUS_BG 0x30, 0x30, 0x30

static void clampgeo(int *w, int *h)
{
	if (*w < VP_MIN_W)
		*w = VP_MIN_W;
	if (*h < VP_MIN_H)
		*h = VP_MIN_H;
}

Window *window_create(WM *wm, int x, int y, int w, int h)
{
	clampgeo(&w, &h);

	Window *win = calloc(1, sizeof(*win));
	if (!win) {
		return NULL;
	}
	win->id = wm->next_id++;
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

	/* Frame plane bound to the desktop (std) plane. */
	ncplane_options fopts = { 0 };
	fopts.y = y;
	fopts.x = x;
	fopts.rows = (unsigned)h;
	fopts.cols = (unsigned)w;
	fopts.userptr =
		win; /* lets wm_window_at map a plane back to its Window */
	win->frame = ncplane_create(wm->std, &fopts);
	if (!win->frame) {
		free(win);
		return NULL;
	}

	/* Content plane in the 1-cell interior, bound to the frame. */
	ncplane_options copts = { 0 };
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
		vt_resize(win, rows,
			  cols); /* resizes content plane + emulator */
		pty_set_winsize(win->pty, rows, cols);
	}
	/* A move slides the bound image planes along with the frame; re-evaluate
	 * their visibility now (this re-render won't run vt_render otherwise) so an
	 * image dragged past a screen edge has its bitmap destroyed rather than
	 * pushed off-screen, which scrolls some terminals. */
	if (win->nimages > 0) {
		sixel_reposition(win);
	}
	win->frame_dirty = true;
}

/* The login name and hostname are process-stable; resolve each once. */
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

/* Read pid's current working directory into out, abbreviating a leading $HOME
 * to "~". out is set empty if it can't be read. */
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

/* Recompute the title from the PTY's foreground process group: the running
 * program's name, or - when the shell itself is in the foreground (idle at its
 * prompt) - user@host:cwd. Returns true if the title changed (so callers can
 * refresh the frame/taskbar). */
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

	/* Deriving the title hits /proc (tcgetpgrp + readlink/fopen). wm_render
     * calls this for every window on every pass, so throttle each window to at
     * most one poll per VP_TITLE_POLL_MS - otherwise a window streaming output
     * (which renders constantly) would re-poll /proc on every frame. */
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

	/* Truncate to the stored title's capacity *before* comparing: win->title
     * already holds a truncated copy, so comparing the untruncated `next`
     * against it would report a spurious change every call once the title
     * exceeds VP_TITLE_MAX (e.g. a deep cwd), dirtying the frame/taskbar
     * needlessly on every render. */
	char fitted[VP_TITLE_MAX];
	snprintf(fitted, sizeof(fitted), "%.*s", (int)sizeof(fitted) - 1, next);
	if (strcmp(fitted, win->title) == 0) {
		return false;
	}
	memcpy(win->title, fitted, sizeof(fitted));
	win->frame_dirty = true;
	return true;
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

	/* Window buttons near the right corner: minimize, maximize, close. */
	const char *btns = "[_][▢][x]"; /* [_][▢][x] : 9 columns wide */
	int btnw = 9;
	int btnx = w - 1 - btnw;
	if (btnx > 4) {
		ncplane_putstr_yx(f, 0, btnx, btns);
	} else {
		btnx = w; /* no room; title may use full width */
	}

	/* Title text between the left corner and the buttons. */
	int tstart = 1; /* just inside the ┌ corner */
	int tend = (btnx < w) ? btnx - 1 : w - 1;
	int avail = tend - tstart;
	if (avail > 0) {
		char buf[VP_TITLE_MAX + 32];
		if (win->sb_offset > 0) {
			/* Scrolled into history: show how far back the view is. */
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

	win->frame_dirty = false;
}
