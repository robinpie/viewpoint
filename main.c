// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie
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

/* main.c - initialization and the single poll(2)-based event loop (the spine).
 *
 * One poll set contains: notcurses' input fd, the GPM fd (bare console only),
 * and every window's PTY master fd. In a GUI terminal notcurses decodes the
 * mouse on its input fd; on the bare console we read GPM ourselves (the two are
 * mutually exclusive). After draining ready fds we do one render pass over the
 * window stack + taskbar.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <poll.h>
#include <signal.h>
#include <locale.h>
#include <stdarg.h>
#include <sys/wait.h>

static FILE *g_dbg = NULL;

void vp_log(const char *fmt, ...)
{
    if (!g_dbg) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dbg, fmt, ap);
    va_end(ap);
    fflush(g_dbg);
}

static volatile sig_atomic_t g_sigchld = 0;

static void on_sigchld(int sig)
{
    (void)sig;
    g_sigchld = 1;
}

static void reap_children(void)
{
    int status;
    while (waitpid(-1, &status, WNOHANG) > 0) {
        /* EOF/EIO on the master fd is what actually drives window teardown */
    }
}

/* Drain all currently-available host input events. */
static void handle_input(WM *wm, bool *quit)
{
    ncinput ni;
    uint32_t id;
    while ((id = notcurses_get_nblock(wm->nc, &ni)) != 0) {
        if (id == (uint32_t)-1 || id == NCKEY_EOF) {
            *quit = true;
            return;
        }
        if (id == NCKEY_RESIZE) {
            wm_handle_resize(wm);
            continue;
        }
        if (nckey_mouse_p(id)) {
            input_route_mouse(wm, &ni);
            continue;
        }
        if (ni.evtype == NCTYPE_RELEASE) {
            continue; /* forward presses/repeats only */
        }
        /* WM chords first; anything not consumed goes to the focused window. */
        if (input_handle_key(wm, &ni)) {
            continue;
        }
        Window *f = wm_focused(wm);
        if (f) {
            vt_send_key(f, &ni);
        }
    }
}

/* Read available output from one window's PTY into its emulator. */
static void drain_window_pty(Window *win)
{
    char buf[8192];
    for (;;) {
        ssize_t n = read(win->pty, buf, sizeof(buf));
        if (n > 0) {
            vt_feed(win, buf, (size_t)n);
        } else if (n == 0) {
            win->dead = true; /* EOF: child exited */
            return;
        } else {
            if (errno == EINTR) {
                continue;
            }
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                return; /* drained */
            }
            /* Linux PTY master returns -1/EIO once the slave is gone. */
            win->dead = true;
            return;
        }
    }
}

int main(void)
{
    setlocale(LC_ALL, "");

    const char *dbgpath = getenv("VP_DEBUG");
    if (dbgpath) {
        g_dbg = fopen(dbgpath, "w");
    }

    struct sigaction sa = {0};
    sa.sa_handler = on_sigchld;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);

    notcurses_options opts = {0};
    opts.flags = NCOPTION_SUPPRESS_BANNERS;
    struct notcurses *nc = notcurses_init(&opts, stdout);
    if (!nc) {
        fprintf(stderr, "viewpoint: failed to initialize notcurses\n");
        return 1;
    }

    /* Own the terminal fully: stop the line discipline from turning ctrl+c,
     * ctrl+\ and ctrl+z into SIGINT/SIGQUIT/SIGTSTP for *us*. As a multiplexer
     * we must deliver those bytes to the focused window's program instead - and
     * without this, ctrl+c would just kill viewpoint (notcurses' own quit
     * sighandler tears down and exits) even in PASSTHROUGH mode. */
    notcurses_linesigs_disable(nc);

    WM wm;
    wm_init(&wm, nc);

    /* Exactly one mouse source. On the bare console we drive GPM ourselves with
     * the full event mask (so bare hover motion arrives); enabling notcurses'
     * mice there too would open a second in-process libgpm client and the two
     * would collide over GPM's shared global state. In a GUI terminal notcurses
     * decodes the mouse and the emulator draws the pointer. */
    if (wm.console) {
        gpm_setup(&wm);
    } else {
        notcurses_mice_enable(nc, NCMICE_ALL_EVENTS);
    }

    taskbar_create(&wm);
    settings_init(&wm);  /* desktop launcher icon (top-left), below the windows */
    exit_icon_init(&wm); /* desktop Exit icon (bottom-right), below the windows */

    /* Start with two overlapping shells. */
    wm_spawn_window(&wm);
    wm_spawn_window(&wm);
    if (wm.nwins == 0) {
        notcurses_stop(nc);
        fprintf(stderr, "viewpoint: failed to spawn initial window\n");
        return 1;
    }

    /* GUI terminal only: notcurses enables any-motion tracking (?1003h) but then
     * enables X11 press/release tracking (?1000h). On terminals where the
     * last-set mouse tracking mode wins (e.g. Konsole), that leaves us with
     * press/release only and no button-held motion - so drags don't update live.
     * Render once so notcurses flushes its own mouse setup, then re-assert
     * button-event (drag) tracking + SGR encoding so a motion-reporting mode is
     * the active one. These xterm sequences are meaningless on the console. */
    wm_render(&wm);
    if (!wm.console) {
        static const char reassert[] = "\x1b[?1002h\x1b[?1003h\x1b[?1006h";
        ssize_t rc = write(STDOUT_FILENO, reassert, sizeof(reassert) - 1);
        (void)rc;
    }

    struct pollfd *pfds = NULL;
    int pfds_cap = 0;
    bool quit = false;

    while (!quit) {
        /* pollfds: [0]=notcurses input, [1]=gpm (console only), then one per
         * window PTY */
        int need = wm.nwins + 2;
        if (need > pfds_cap) {
            struct pollfd *np = realloc(pfds, (size_t)need * sizeof(*np));
            if (!np) {
                break;
            }
            pfds = np;
            pfds_cap = need;
        }

        int nfd = 0;
        pfds[nfd].fd = notcurses_inputready_fd(nc);
        pfds[nfd].events = POLLIN;
        pfds[nfd].revents = 0;
        int input_idx = nfd++;

        int gpm_idx = -1;
        if (wm.gpm_active && wm.gpm_fd >= 0) {
            pfds[nfd].fd = wm.gpm_fd;
            pfds[nfd].events = POLLIN;
            pfds[nfd].revents = 0;
            gpm_idx = nfd++;
        }

        int first_win = nfd;
        for (int i = 0; i < wm.nwins; i++) {
            pfds[nfd].fd = wm.wins[i]->pty;
            pfds[nfd].events = POLLIN;
            pfds[nfd].revents = 0;
            nfd++;
        }

        int pr = poll(pfds, (nfds_t)nfd, -1);
        if (pr < 0) {
            if (errno == EINTR) {
                if (g_sigchld) {
                    g_sigchld = 0;
                    reap_children();
                }
                continue;
            }
            break;
        }

        if (pfds[input_idx].revents & POLLIN) {
            handle_input(&wm, &quit);
        }
        if (gpm_idx >= 0 && (pfds[gpm_idx].revents & POLLIN)) {
            gpm_pump(&wm);
        }
        for (int i = 0; i < wm.nwins; i++) {
            if (pfds[first_win + i].revents & (POLLIN | POLLHUP)) {
                drain_window_pty(wm.wins[i]);
            }
        }

        if (g_sigchld) {
            g_sigchld = 0;
            reap_children();
        }

        /* Reap dead windows (deferred so we never mutate the list mid-scan). */
        for (int i = 0; i < wm.nwins; ) {
            Window *win = wm.wins[i];
            if (win->dead) {
                wm_remove_window(&wm, win);
                window_destroy(&wm, win);
                /* wm_remove_window compacted the array; don't advance i */
            } else {
                i++;
            }
        }
        /* Closing the last window no longer quits: the desktop persists with
         * its taskbar and launcher icons. The only ways out are the Exit icon
         * (wm.should_quit) and host EOF on the input fd. */
        if (wm.should_quit) {
            quit = true;
        }

        if (!quit) {
            wm_render(&wm);
        }
    }

    /* Teardown. */
    for (int i = 0; i < wm.nwins; i++) {
        window_destroy(&wm, wm.wins[i]);
    }
    free(wm.wins);
    free(pfds);
    exit_icon_teardown(&wm);
    settings_teardown(&wm);
    config_free(&wm.config);
    gpm_teardown(&wm);
    /* Undo the motion-tracking modes we re-asserted (notcurses_stop resets the
     * modes it set itself, but not our extra 1002). Console runs never set them. */
    if (!wm.console) {
        static const char off[] = "\x1b[?1002l\x1b[?1003l";
        ssize_t rc = write(STDOUT_FILENO, off, sizeof(off) - 1);
        (void)rc;
    }
    notcurses_stop(nc);
    reap_children();
    if (g_dbg) {
        fclose(g_dbg);
    }
    return 0;
}
