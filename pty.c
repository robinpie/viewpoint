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

/* pty.c - PTY allocation and child shell setup via forkpty(). */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <pty.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>

pid_t pty_spawn(int rows, int cols, int *master_out)
{
	return pty_spawn_cmd(rows, cols, master_out, NULL);
}

pid_t pty_spawn_cmd(int rows, int cols, int *master_out, const char *cmd)
{
	struct winsize ws = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
		.ws_xpixel = 0,
		.ws_ypixel = 0,
	};

	int master = -1;
	pid_t pid = forkpty(&master, NULL, NULL, &ws);
	if (pid < 0) {
		return -1;
	}

	if (pid == 0) {
		/* child: forkpty already made the slave our controlling tty and wired
         * it to stdin/stdout/stderr. Reset signal handlers the parent set. */
		signal(SIGCHLD, SIG_DFL);
		signal(SIGWINCH, SIG_DFL);
		signal(SIGINT, SIG_DFL);
		signal(SIGTERM, SIG_DFL);
		signal(SIGPIPE, SIG_DFL);

		sigset_t none;
		sigemptyset(&none);
		sigprocmask(SIG_SETMASK, &none, NULL);

		/* libvterm emulates xterm-256color; advertise that to the child. */
		setenv("TERM", "xterm-256color", 1);
		unsetenv("LINES");
		unsetenv("COLUMNS");

		const char *shell = getenv("SHELL");
		if (!shell || !*shell) {
			shell = "/bin/sh";
		}
		/* A launcher's command runs under the shell rather than being split
		 * here, so redirections, pipelines and aliases behave as the user
		 * typed them. Without one this is a plain interactive shell. */
		if (cmd && *cmd) {
			execlp(shell, shell, "-c", cmd, (char *)NULL);
		} else {
			execlp(shell, shell, (char *)NULL);
		}
		_exit(127);
	}

	/* parent: keep the master non-blocking so reads in the event loop never
     * stall the single thread. */
	int flags = fcntl(master, F_GETFL, 0);
	if (flags >= 0) {
		fcntl(master, F_SETFL, flags | O_NONBLOCK);
	}
	fcntl(master, F_SETFD, FD_CLOEXEC);

	*master_out = master;
	return pid;
}

void pty_set_winsize(int master, int rows, int cols)
{
	struct winsize ws = {
		.ws_row = (unsigned short)rows,
		.ws_col = (unsigned short)cols,
		.ws_xpixel = 0,
		.ws_ypixel = 0,
	};
	ioctl(master, TIOCSWINSZ, &ws);
}
