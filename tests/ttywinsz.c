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

/* ttywinsz - print the window size the kernel holds for another process's tty.
 *
 * The probe can only report the size it was *told* about. This reports the size
 * the kernel actually has for that pty, which is the other half of the
 * question: if these two disagree the app missed a SIGWINCH, and if they agree
 * but neither matches the window then viewpoint never issued the TIOCSWINSZ.
 *
 * Usage: ttywinsz PID...        prints "PID rows cols /dev/pts/N" per line
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

static int report(const char *pidstr)
{
	char path[64], tty[256];
	snprintf(path, sizeof(path), "/proc/%s/fd/0", pidstr);

	ssize_t n = readlink(path, tty, sizeof(tty) - 1);
	if (n < 0) {
		printf("%s ? ? <unreadable>\n", pidstr);
		return 1;
	}
	tty[n] = '\0';

	/* O_NOCTTY so opening someone else's terminal can never make it ours,
	 * O_NONBLOCK so a pty with no writer does not stall the open. */
	int fd = open(path, O_RDONLY | O_NOCTTY | O_NONBLOCK);
	if (fd < 0) {
		printf("%s ? ? %s\n", pidstr, tty);
		return 1;
	}

	struct winsize ws;
	int rc = ioctl(fd, TIOCGWINSZ, &ws);
	close(fd);
	if (rc != 0) {
		printf("%s ? ? %s\n", pidstr, tty);
		return 1;
	}

	printf("%s %u %u %s\n", pidstr, ws.ws_row, ws.ws_col, tty);
	return 0;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s PID...\n", argv[0]);
		return 2;
	}
	int bad = 0;
	for (int i = 1; i < argc; i++) {
		bad |= report(argv[i]);
	}
	return bad;
}
