// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie <robin@dreamstation.systems>
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

/* winsize_probe - a stand-in for htop, for reproducing the resize bug.
 *
 * htop makes a poor oracle: you can only tell it failed to reflow by looking at
 * it. This draws the same shape of thing but says out loud what size it thinks
 * the terminal is, and records every size it ever observed to a log.
 *
 * It paints its *entire* believed screen area with a solid background, so a
 * screenshot answers the question directly: if the coloured block does not fill
 * the viewpoint window, the app never learned the window's new size.
 *
 * Two independent detectors run side by side, which is what splits the bug in
 * half:
 *
 *   sigwinch  the kernel delivered SIGWINCH, i.e. someone did TIOCSWINSZ on the
 *             master and the size genuinely changed.
 *   poll      a TIOCGWINSZ every 200ms noticed a change the signal did not
 *             report.
 *
 * "poll saw it, sigwinch did not" means the signal was lost or coalesced.
 * Neither of them seeing it means the resize never reached the pty at all.
 *
 * Usage: winsize_probe [--alt] [--log PATH] [--label TEXT]
 *   --alt    run on the alternate screen, as htop and every other full-screen
 *            curses app does - libvterm resizes the alt buffer down a different
 *            path than the primary one, so this is not cosmetic.
 * Quits on 'q' or ctrl+c.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_winch;
static int g_wake[2] = { -1, -1 };

static void on_winch(int sig)
{
	(void)sig;
	g_winch = 1;
	if (g_wake[1] >= 0) {
		ssize_t r = write(g_wake[1], "w", 1);
		(void)r;
	}
}

static FILE *g_log;
static struct timespec g_t0;
static const char *g_label = "probe";
static bool g_alt;
static struct termios g_saved_tio;
static bool g_tio_saved;

static long ms_since_start(void)
{
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	return (now.tv_sec - g_t0.tv_sec) * 1000 +
	       (now.tv_nsec - g_t0.tv_nsec) / 1000000;
}

static void logf_(const char *event, int rows, int cols, int nwinch, int npoll)
{
	if (!g_log) {
		return;
	}
	fprintf(g_log, "%6ld %-8s rows=%-4d cols=%-4d sigwinch=%-3d polled=%d\n",
		ms_since_start(), event, rows, cols, nwinch, npoll);
	fflush(g_log);
}

static bool query_size(int *rows, int *cols)
{
	struct winsize ws;
	if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) != 0) {
		return false;
	}
	*rows = ws.ws_row ? ws.ws_row : 24;
	*cols = ws.ws_col ? ws.ws_col : 80;
	return true;
}

/* Paint the whole believed screen, edge to edge. Every drawn cell is one the
 * app is convinced it owns, so the painted region *is* the app's model of the
 * window - which is exactly the thing under test. */
static void redraw(int rows, int cols, int nwinch, int npoll)
{
	char *line = malloc((size_t)cols + 1);
	if (!line) {
		return;
	}

	/* white on blue for the field, so it reads as a solid block against
	 * both the window frame and the desktop behind it */
	printf("\033[H\033[0m\033[44;97m");

	for (int y = 0; y < rows; y++) {
		for (int x = 0; x < cols; x++) {
			line[x] = ' ';
		}
		line[cols] = '\0';

		if (y == 0 || y == rows - 1) {
			/* Solid rules top and bottom, with the true corners
			 * marked. If the app is drawing to a stale size these
			 * land inside the window instead of on its edges. */
			for (int x = 0; x < cols; x++) {
				line[x] = '-';
			}
			line[0] = '+';
			line[cols - 1] = '+';
		} else if (y == 1 && cols > 10) {
			/* column ruler: a digit every 10 columns, so a width
			 * mismatch is readable off a screenshot */
			for (int x = 0; x < cols; x += 10) {
				char tag[8];
				int n = snprintf(tag, sizeof(tag), "%d", x);
				for (int k = 0; k < n && x + k < cols; k++) {
					line[x + k] = tag[k];
				}
			}
		} else if (y == rows / 2 && cols > 24) {
			char mid[256];
			int n = snprintf(mid, sizeof(mid),
					 " %s  %d x %d  (h x w)  sigwinch=%d polled=%d  pid=%ld ",
					 g_label, rows, cols, nwinch, npoll,
					 (long)getpid());
			int x0 = (cols - n) / 2;
			if (x0 < 0) {
				x0 = 0;
			}
			for (int k = 0; k < n && x0 + k < cols; k++) {
				line[x0 + k] = mid[k];
			}
		} else if (y == rows / 2 + 1 && cols > 24) {
			char mid[128];
			int n = snprintf(mid, sizeof(mid),
					 " last row is %d, last col is %d ",
					 rows - 1, cols - 1);
			int x0 = (cols - n) / 2;
			if (x0 < 0) {
				x0 = 0;
			}
			for (int k = 0; k < n && x0 + k < cols; k++) {
				line[x0 + k] = mid[k];
			}
		}

		/* Absolute cursor address per row rather than a trailing
		 * newline: a newline at the real bottom would scroll, and
		 * scrolling is its own bug class. */
		printf("\033[%d;1H%s", y + 1, line);
	}

	printf("\033[0m");
	fflush(stdout);
	free(line);
}

static void restore_tty(void)
{
	if (g_tio_saved) {
		tcsetattr(STDIN_FILENO, TCSANOW, &g_saved_tio);
	}
	if (g_alt) {
		printf("\033[?1049l");
	}
	printf("\033[0m\033[?25h");
	fflush(stdout);
}

int main(int argc, char **argv)
{
	const char *logpath = NULL;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--alt") == 0) {
			g_alt = true;
		} else if (strcmp(argv[i], "--log") == 0 && i + 1 < argc) {
			logpath = argv[++i];
		} else if (strcmp(argv[i], "--label") == 0 && i + 1 < argc) {
			g_label = argv[++i];
		} else {
			fprintf(stderr,
				"usage: %s [--alt] [--log PATH] [--label TEXT]\n",
				argv[0]);
			return 2;
		}
	}

	clock_gettime(CLOCK_MONOTONIC, &g_t0);
	if (logpath) {
		g_log = fopen(logpath, "w");
		if (g_log) {
			/* The driver reads the pid back off this line so it can
			 * ask the kernel what size *it* thinks the pty is.
			 *
			 * The rest is why a SIGWINCH might never arrive even
			 * though the ioctl succeeded. The kernel signals the
			 * tty's foreground process group, so if tcpgrp is not
			 * our pgrp the signal goes to someone else - and a
			 * blocked SIGWINCH is inherited across both fork and
			 * exec, so a parent that blocked it hands that down to
			 * everything it ever spawns. */
			sigset_t blocked;
			sigemptyset(&blocked);
			sigprocmask(SIG_BLOCK, NULL, &blocked);
			fprintf(g_log,
				"# pid=%ld pgrp=%ld tcpgrp=%ld sid=%ld alt=%d label=%s\n",
				(long)getpid(), (long)getpgrp(),
				(long)tcgetpgrp(STDOUT_FILENO), (long)getsid(0),
				g_alt ? 1 : 0, g_label);
			fprintf(g_log, "# SIGWINCH blocked at startup: %s\n",
				sigismember(&blocked, SIGWINCH) ? "YES - it can never be delivered" :
								  "no");
			fflush(g_log);
		}
	}

	if (pipe(g_wake) != 0) {
		return 1;
	}
	fcntl(g_wake[0], F_SETFL, O_NONBLOCK);
	fcntl(g_wake[1], F_SETFL, O_NONBLOCK);

	struct sigaction sa = { 0 };
	sa.sa_handler = on_winch;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = SA_RESTART;
	sigaction(SIGWINCH, &sa, NULL);

	if (tcgetattr(STDIN_FILENO, &g_saved_tio) == 0) {
		g_tio_saved = true;
		struct termios raw = g_saved_tio;
		raw.c_lflag &= (tcflag_t) ~(ICANON | ECHO);
		raw.c_cc[VMIN] = 0;
		raw.c_cc[VTIME] = 0;
		tcsetattr(STDIN_FILENO, TCSANOW, &raw);
	}
	atexit(restore_tty);

	if (g_alt) {
		printf("\033[?1049h");
	}
	printf("\033[?25l");
	fflush(stdout);

	int rows = 24, cols = 80;
	query_size(&rows, &cols);
	int nwinch = 0, npoll = 0;
	logf_("start", rows, cols, nwinch, npoll);
	redraw(rows, cols, nwinch, npoll);

	for (;;) {
		struct pollfd pfds[2] = {
			{ .fd = STDIN_FILENO, .events = POLLIN },
			{ .fd = g_wake[0], .events = POLLIN },
		};
		int n = poll(pfds, 2, 200);
		if (n < 0 && errno != EINTR) {
			break;
		}

		bool got_signal = false;
		if (g_winch) {
			g_winch = 0;
			got_signal = true;
			char drain[64];
			while (read(g_wake[0], drain, sizeof(drain)) > 0) {
			}
		}

		if (pfds[0].revents & POLLIN) {
			char buf[64];
			ssize_t k = read(STDIN_FILENO, buf, sizeof(buf));
			if (k > 0 && memchr(buf, 'q', (size_t)k)) {
				break;
			}
			if (k == 0) {
				break; /* pty hung up */
			}
		}

		int nr = rows, nc = cols;
		bool ok = query_size(&nr, &nc);
		bool changed = ok && (nr != rows || nc != cols);

		if (got_signal) {
			nwinch++;
		}
		/* A change nobody signalled: either the signal was lost or it
		 * arrived and coalesced with one we already counted. */
		if (changed && !got_signal) {
			npoll++;
		}

		if (got_signal || changed) {
			rows = nr;
			cols = nc;
			logf_(got_signal ? (changed ? "sigwinch" : "winch-nop") :
					   "polled",
			      rows, cols, nwinch, npoll);
			redraw(rows, cols, nwinch, npoll);
		}
	}

	logf_("exit", rows, cols, nwinch, npoll);
	if (g_log) {
		fclose(g_log);
	}
	return 0;
}
