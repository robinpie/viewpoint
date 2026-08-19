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

/* ptycontrol - the control arm of the resize harness.
 *
 * Does exactly what viewpoint's session daemon does and nothing else: forkpty,
 * exec the app, then TIOCSWINSZ the master a few times. No compositor, no
 * libvterm, no socket, no window manager.
 *
 * If the probe tracks every size here but not under viewpoint, the difference
 * is viewpoint. If it fails here too, the fault is in the probe or in the app,
 * and the harness itself is what needs fixing first.
 *
 * Usage: ptycontrol [--start RxC] [--then RxC]... -- CMD [ARGS...]
 * Sizes are applied one second apart, then the child is sent 'q' and reaped.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pty.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#define MAX_STEPS 8

struct size {
	int rows, cols;
};

static int parse_size(const char *s, struct size *out)
{
	int r, c;
	if (sscanf(s, "%dx%d", &r, &c) != 2 || r < 1 || c < 1) {
		return -1;
	}
	out->rows = r;
	out->cols = c;
	return 0;
}

/* Drain the master for `ms` milliseconds so the app never blocks on a full pty
 * buffer - the daemon does the same thing by reading it into libvterm. */
static void drain(int fd, int ms)
{
	struct timespec deadline;
	clock_gettime(CLOCK_MONOTONIC, &deadline);
	long end_ms = deadline.tv_sec * 1000 + deadline.tv_nsec / 1000000 + ms;
	for (;;) {
		struct timespec now;
		clock_gettime(CLOCK_MONOTONIC, &now);
		long now_ms = now.tv_sec * 1000 + now.tv_nsec / 1000000;
		if (now_ms >= end_ms) {
			return;
		}
		struct pollfd pfd = { .fd = fd, .events = POLLIN };
		if (poll(&pfd, 1, (int)(end_ms - now_ms)) > 0) {
			char buf[4096];
			if (read(fd, buf, sizeof(buf)) <= 0 && errno != EAGAIN &&
			    errno != EINTR) {
				return;
			}
		}
	}
}

int main(int argc, char **argv)
{
	struct size steps[MAX_STEPS];
	int nsteps = 0;
	struct size start = { 24, 80 };
	int i = 1;

	for (; i < argc; i++) {
		if (strcmp(argv[i], "--start") == 0 && i + 1 < argc) {
			if (parse_size(argv[++i], &start) != 0) {
				fprintf(stderr, "bad --start\n");
				return 2;
			}
		} else if (strcmp(argv[i], "--then") == 0 && i + 1 < argc) {
			if (nsteps >= MAX_STEPS ||
			    parse_size(argv[++i], &steps[nsteps]) != 0) {
				fprintf(stderr, "bad --then\n");
				return 2;
			}
			nsteps++;
		} else if (strcmp(argv[i], "--") == 0) {
			i++;
			break;
		} else {
			fprintf(stderr,
				"usage: %s [--start RxC] [--then RxC]... -- CMD [ARGS...]\n",
				argv[0]);
			return 2;
		}
	}
	if (i >= argc) {
		fprintf(stderr, "no command given\n");
		return 2;
	}

	struct winsize ws = { .ws_row = (unsigned short)start.rows,
			      .ws_col = (unsigned short)start.cols };
	int master = -1;
	pid_t pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0) {
		perror("forkpty");
		return 1;
	}
	if (pid == 0) {
		signal(SIGWINCH, SIG_DFL);
		setenv("TERM", "xterm-256color", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");
		execvp(argv[i], &argv[i]);
		_exit(127);
	}

	fprintf(stderr, "ptycontrol: child %ld start %dx%d\n", (long)pid,
		start.rows, start.cols);
	drain(master, 1000);

	for (int s = 0; s < nsteps; s++) {
		struct winsize nws = {
			.ws_row = (unsigned short)steps[s].rows,
			.ws_col = (unsigned short)steps[s].cols
		};
		if (ioctl(master, TIOCSWINSZ, &nws) != 0) {
			perror("TIOCSWINSZ");
		}
		fprintf(stderr, "ptycontrol: resized to %dx%d\n", steps[s].rows,
			steps[s].cols);
		drain(master, 1000);
	}

	ssize_t w = write(master, "q", 1);
	(void)w;
	drain(master, 500);
	kill(pid, SIGHUP);

	int status = 0;
	waitpid(pid, &status, 0);
	close(master);
	return 0;
}
