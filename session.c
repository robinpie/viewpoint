// SPDX-License-Identifier: GPL-3.0-only
/*
 * Copyright (C) 2026  robinpie <robin@dreamstation.systems>
 */

/* session.c - a tiny per-user daemon that owns PTYs across UI exits. */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pwd.h>
#include <limits.h>
#include <time.h>

/* Wire protocol id. Bumped whenever a message's layout changes, so a daemon left
 * running from an older build refuses the connection outright instead of
 * misparsing it (see evict_stale_server for what happens next). VPS2 is the
 * previous one: same messages, but MSG_NEW carried no command. */
#define VP_SESSION_MAGIC "VPS3"
#define VP_SESSION_LEGACY "VPS2"
#define VP_SESSION_ACK "OK"
#define VP_REPLAY_CAP (1024 * 1024)
#define VP_CMD_MAX 4096 /* longest launcher command the protocol carries */

enum {
	MSG_WINDOW = 'W',
	MSG_DATA = 'D',
	MSG_DEAD = 'X',
	MSG_NEW = 'N',
	MSG_INPUT = 'I',
	MSG_RESIZE = 'R',
	MSG_CLOSE = 'C',
	MSG_SHUTDOWN = 'Q',
	MSG_TITLE = 'T',
};

static bool session_path(char *buf, size_t buflen)
{
	const char *dir = getenv("XDG_RUNTIME_DIR");
	if (dir && *dir) {
		return snprintf(buf, buflen, "%s/viewpoint.sock", dir) <
		       (int)buflen;
	}
	return snprintf(buf, buflen, "/tmp/viewpoint-%ld.sock", (long)getuid()) <
	       (int)buflen;
}

static bool write_all(int fd, const void *buf, size_t len)
{
	const char *p = buf;
	while (len > 0) {
		ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

static bool read_all(int fd, void *buf, size_t len)
{
	char *p = buf;
	while (len > 0) {
		ssize_t n = read(fd, p, len);
		if (n > 0) {
			p += n;
			len -= (size_t)n;
		} else if (n < 0 && errno == EINTR) {
			continue;
		} else {
			return false;
		}
	}
	return true;
}

static void client_drop(WM *wm)
{
	if (wm->session_fd >= 0) {
		close(wm->session_fd);
		wm->session_fd = -1;
	}
}

static void put16(unsigned char *p, int v)
{
	p[0] = (unsigned char)((v >> 8) & 0xff);
	p[1] = (unsigned char)(v & 0xff);
}

static void put32(unsigned char *p, uint32_t v)
{
	p[0] = (unsigned char)((v >> 24) & 0xff);
	p[1] = (unsigned char)((v >> 16) & 0xff);
	p[2] = (unsigned char)((v >> 8) & 0xff);
	p[3] = (unsigned char)(v & 0xff);
}

static int get16(const unsigned char *p)
{
	return (p[0] << 8) | p[1];
}

static uint32_t get32(const unsigned char *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
	       ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static bool send_window_msg(int fd, int id, int rows, int cols)
{
	unsigned char h[9] = { MSG_WINDOW };
	put32(h + 1, (uint32_t)id);
	put16(h + 5, rows);
	put16(h + 7, cols);
	return write_all(fd, h, sizeof(h));
}

static bool send_data_msg(int fd, int id, const char *bytes, size_t len)
{
	unsigned char h[9] = { MSG_DATA };
	put32(h + 1, (uint32_t)id);
	put32(h + 5, (uint32_t)len);
	return write_all(fd, h, sizeof(h)) && write_all(fd, bytes, len);
}

static bool send_dead_msg(int fd, int id)
{
	unsigned char h[5] = { MSG_DEAD };
	put32(h + 1, (uint32_t)id);
	return write_all(fd, h, sizeof(h));
}

static bool send_title_msg(int fd, int id, const char *title)
{
	size_t len = strlen(title);
	if (len > VP_TITLE_MAX - 1) {
		len = VP_TITLE_MAX - 1;
	}
	unsigned char h[7] = { MSG_TITLE };
	put32(h + 1, (uint32_t)id);
	put16(h + 5, (int)len);
	return write_all(fd, h, sizeof(h)) && write_all(fd, title, len);
}

bool session_request_new(WM *wm, int rows, int cols)
{
	return session_request_new_cmd(wm, rows, cols, NULL);
}

bool session_request_new_cmd(WM *wm, int rows, int cols, const char *cmd)
{
	if (wm->session_fd < 0) {
		return false;
	}
	size_t len = cmd ? strlen(cmd) : 0;
	if (len > VP_CMD_MAX) {
		len = VP_CMD_MAX;
	}
	unsigned char h[7] = { MSG_NEW };
	put16(h + 1, rows);
	put16(h + 3, cols);
	put16(h + 5, (int)len);
	if (!write_all(wm->session_fd, h, sizeof(h))) {
		return false;
	}
	if (len > 0 && !write_all(wm->session_fd, cmd, len)) {
		return false;
	}
	struct pollfd pfd = { .fd = wm->session_fd, .events = POLLIN };
	if (poll(&pfd, 1, 1000) > 0 && (pfd.revents & POLLIN)) {
		session_drain(wm);
	}
	return true;
}

bool session_send_input(Window *w, const char *bytes, size_t len)
{
	if (!w || !w->wm || w->wm->session_fd < 0 || len > UINT32_MAX) {
		return false;
	}
	unsigned char h[9] = { MSG_INPUT };
	put32(h + 1, (uint32_t)w->id);
	put32(h + 5, (uint32_t)len);
	return write_all(w->wm->session_fd, h, sizeof(h)) &&
	       write_all(w->wm->session_fd, bytes, len);
}

bool session_resize(Window *w, int rows, int cols)
{
	if (!w || !w->wm || w->wm->session_fd < 0) {
		return false;
	}
	unsigned char h[9] = { MSG_RESIZE };
	put32(h + 1, (uint32_t)w->id);
	put16(h + 5, rows);
	put16(h + 7, cols);
	return write_all(w->wm->session_fd, h, sizeof(h));
}

bool session_close(Window *w)
{
	if (!w || !w->wm || w->wm->session_fd < 0) {
		return false;
	}
	unsigned char h[5] = { MSG_CLOSE };
	put32(h + 1, (uint32_t)w->id);
	return write_all(w->wm->session_fd, h, sizeof(h));
}

bool session_shutdown(WM *wm)
{
	if (!wm || wm->session_fd < 0) {
		return false;
	}
	unsigned char h = MSG_SHUTDOWN;
	return write_all(wm->session_fd, &h, 1);
}

int session_fd(WM *wm)
{
	return wm->session_fd;
}

static Window *client_window_by_id(WM *wm, int id)
{
	for (int i = 0; i < wm->nwins; i++) {
		if (wm->wins[i]->id == id) {
			return wm->wins[i];
		}
	}
	return NULL;
}

static void client_add_window(WM *wm, int id, int rows, int cols)
{
	if (client_window_by_id(wm, id)) {
		return;
	}
	int n = wm->nwins;
	int x = 16 + (n % 6) * 4;
	int y = 1 + (n % 6) * 2;
	Window *win = window_create_attached(wm, id, x, y, cols + 2 * VP_BORDER,
					     rows + 2 * VP_BORDER);
	if (!win) {
		return;
	}
	snprintf(win->title, sizeof(win->title), "shell %d", id);
	if (!wm_add_window(wm, win)) {
		window_destroy(wm, win);
		return;
	}
	wm_clamp_onscreen(wm, win);
	wm_focus_window(wm, win);
	vp_log("attach id=%d nwins=%d\n", win->id, wm->nwins);
}

void session_drain(WM *wm)
{
	if (wm->session_fd < 0) {
		return;
	}
	for (;;) {
		unsigned char type;
		ssize_t n = recv(wm->session_fd, &type, 1, MSG_DONTWAIT);
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			return;
		}
		if (n <= 0) {
			client_drop(wm);
			return;
		}
		if (type == MSG_WINDOW) {
			unsigned char h[8];
			if (!read_all(wm->session_fd, h, sizeof(h))) {
				client_drop(wm);
				return;
			}
			client_add_window(wm, (int)get32(h), get16(h + 4),
					  get16(h + 6));
		} else if (type == MSG_DATA) {
			unsigned char h[8];
			if (!read_all(wm->session_fd, h, sizeof(h))) {
				client_drop(wm);
				return;
			}
			int id = (int)get32(h);
			uint32_t len = get32(h + 4);
			char buf[8192];
			Window *win = client_window_by_id(wm, id);
			while (len > 0) {
				size_t chunk = len < sizeof(buf) ? len : sizeof(buf);
				if (!read_all(wm->session_fd, buf, chunk)) {
					client_drop(wm);
					return;
				}
				if (win) {
					vt_feed(win, buf, chunk);
				}
				len -= (uint32_t)chunk;
			}
		} else if (type == MSG_DEAD) {
			unsigned char h[4];
			if (!read_all(wm->session_fd, h, sizeof(h))) {
				client_drop(wm);
				return;
			}
			Window *win = client_window_by_id(wm, (int)get32(h));
			if (win) {
				win->dead = true;
			}
		} else if (type == MSG_TITLE) {
			unsigned char h[6];
			if (!read_all(wm->session_fd, h, sizeof(h))) {
				client_drop(wm);
				return;
			}
			int id = (int)get32(h);
			int len = get16(h + 4);
			if (len < 0 || len >= VP_TITLE_MAX) {
				client_drop(wm);
				return;
			}
			char title[VP_TITLE_MAX];
			if (!read_all(wm->session_fd, title, (size_t)len)) {
				client_drop(wm);
				return;
			}
			title[len] = '\0';
			Window *win = client_window_by_id(wm, id);
			if (win && strcmp(win->title, title) != 0) {
				snprintf(win->title, sizeof(win->title), "%s",
					 title);
				window_damage_frame(win);
				taskbar_damage(wm);
			}
		} else {
			client_drop(wm);
			return;
		}
	}
}

static bool start_server(void)
{
	char exe[4096];
	ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
	if (n < 0) {
		return false;
	}
	exe[n] = '\0';

	pid_t pid = fork();
	if (pid < 0) {
		return false;
	}
	if (pid == 0) {
		setsid();
		int nullfd = open("/dev/null", O_RDWR);
		if (nullfd >= 0) {
			dup2(nullfd, STDIN_FILENO);
			dup2(nullfd, STDOUT_FILENO);
			dup2(nullfd, STDERR_FILENO);
			if (nullfd > STDERR_FILENO) {
				close(nullfd);
			}
		}
		execl(exe, exe, "--server", (char *)NULL);
		_exit(127);
	}
	return true;
}

/* Connect to the daemon's socket. Returns the fd, or -1 if nothing is listening. */
static int session_dial(const char *path)
{
	int fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd < 0) {
		return -1;
	}
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (connect(fd, (struct sockaddr *)&sa, sizeof(sa)) != 0) {
		close(fd);
		return -1;
	}
	return fd;
}

/* Announce the protocol we speak and wait for the daemon's ack. A daemon built
 * against a different wire version closes the connection here rather than
 * misreading our messages. */
static bool session_hello(int fd, const char *magic)
{
	char ack[2];
	return write_all(fd, magic, 4) && read_all(fd, ack, sizeof(ack)) &&
	       memcmp(ack, VP_SESSION_ACK, sizeof(ack)) == 0;
}

/* A daemon from an older build is holding the socket and can't talk to us. Ask
 * it to shut down in the protocol it does speak - so it closes its windows
 * properly instead of being orphaned - and wait for it to let the socket go.
 * MSG_SHUTDOWN has meant the same single byte in every version so far. */
static void evict_stale_server(const char *path)
{
	int fd = session_dial(path);
	if (fd >= 0) {
		if (session_hello(fd, VP_SESSION_LEGACY)) {
			unsigned char q = MSG_SHUTDOWN;
			write_all(fd, &q, 1);
		}
		close(fd);
	}
	vp_log("session: daemon speaks an older protocol; retiring it\n");

	/* Wait for it to exit before starting ours: it unlinks the socket path on
	 * the way out, which would otherwise delete our new daemon's socket. */
	for (int i = 0; i < 40; i++) {
		usleep(50000);
		int probe = session_dial(path);
		if (probe < 0) {
			return;
		}
		close(probe);
	}
}

bool session_connect(WM *wm)
{
	wm->session_fd = -1;
	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (!session_path(path, sizeof(path))) {
		return false;
	}

	bool evicted = false;
	for (int attempt = 0; attempt < 30; attempt++) {
		int fd = session_dial(path);
		if (fd >= 0) {
			if (session_hello(fd, VP_SESSION_MAGIC)) {
				wm->session_fd = fd;
				return true;
			}
			close(fd);
			/* Refused: retire the old daemon and put ours in its place.
			 * Only once - a second refusal is a real failure, not skew. */
			if (evicted) {
				return false;
			}
			evicted = true;
			evict_stale_server(path);
			unlink(path);
			if (!start_server()) {
				return false;
			}
			usleep(50000);
			continue;
		}
		if (attempt == 0) {
			unlink(path);
			if (!start_server()) {
				return false;
			}
		}
		usleep(50000);
	}
	return false;
}

void session_close_client(WM *wm)
{
	client_drop(wm);
}

typedef struct SessWin {
	int id;
	pid_t child;
	int pty;
	int rows, cols;
	char *replay;
	size_t replay_len, replay_cap, replay_start;
	uint64_t title_poll_ns;
	char title[VP_TITLE_MAX];
	bool dead;
} SessWin;

static SessWin **g_wins;
static int g_nwins, g_cap, g_client = -1;

static void server_drop_client(void)
{
	if (g_client >= 0) {
		close(g_client);
		g_client = -1;
	}
}

/* Ids are handed out lowest-free-first. */
static int server_alloc_id(void)
{
	for (int id = 1;; id++) {
		bool taken = false;
		for (int i = 0; i < g_nwins; i++) {
			if (g_wins[i]->id == id) {
				taken = true;
				break;
			}
		}
		if (!taken) {
			return id;
		}
	}
}

static SessWin *server_by_id(int id)
{
	for (int i = 0; i < g_nwins; i++) {
		if (g_wins[i]->id == id) {
			return g_wins[i];
		}
	}
	return NULL;
}

static uint64_t mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
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

static bool server_refresh_title(SessWin *w, bool force)
{
	uint64_t now = mono_ns();
	if (!force && now < w->title_poll_ns) {
		return false;
	}

	pid_t fg = tcgetpgrp(w->pty);
	if (fg <= 0) {
		return false;
	}

	char next[PATH_MAX + 256];
	if (fg == w->child) {
		char loc[PATH_MAX];
		proc_cwd(w->child, loc, sizeof(loc));
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
			return false;
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

	char fitted[VP_TITLE_MAX];
	snprintf(fitted, sizeof(fitted), "%.*s", (int)sizeof(fitted) - 1, next);
	w->title_poll_ns = now + (uint64_t)VP_TITLE_POLL_MS * 1000000ull;
	if (strcmp(fitted, w->title) == 0) {
		return false;
	}
	snprintf(w->title, sizeof(w->title), "%s", fitted);
	return true;
}

static void replay_append(SessWin *w, const char *buf, size_t len)
{
	if (w->replay_cap == 0) {
		w->replay_cap = VP_REPLAY_CAP;
		w->replay = malloc(w->replay_cap);
	}
	if (!w->replay || len == 0) {
		return;
	}
	if (len >= w->replay_cap) {
		memcpy(w->replay, buf + len - w->replay_cap, w->replay_cap);
		w->replay_start = 0;
		w->replay_len = w->replay_cap;
		return;
	}
	while (w->replay_len + len > w->replay_cap) {
		w->replay_start = (w->replay_start + 1) % w->replay_cap;
		w->replay_len--;
	}
	size_t pos = (w->replay_start + w->replay_len) % w->replay_cap;
	size_t first = w->replay_cap - pos;
	if (first > len) {
		first = len;
	}
	memcpy(w->replay + pos, buf, first);
	memcpy(w->replay, buf + first, len - first);
	w->replay_len += len;
}

static void server_send_replay(SessWin *w)
{
	if (g_client < 0 || !send_window_msg(g_client, w->id, w->rows, w->cols)) {
		return;
	}
	if (server_refresh_title(w, true)) {
		send_title_msg(g_client, w->id, w->title);
	} else if (w->title[0]) {
		send_title_msg(g_client, w->id, w->title);
	}
	if (w->replay_len == 0) {
		return;
	}
	size_t first = w->replay_cap - w->replay_start;
	if (first > w->replay_len) {
		first = w->replay_len;
	}
	send_data_msg(g_client, w->id, w->replay + w->replay_start, first);
	if (first < w->replay_len) {
		send_data_msg(g_client, w->id, w->replay, w->replay_len - first);
	}
}

static bool server_add(SessWin *w)
{
	if (g_nwins == g_cap) {
		int ncap = g_cap ? g_cap * 2 : 8;
		SessWin **n = realloc(g_wins, (size_t)ncap * sizeof(*n));
		if (!n) {
			return false;
		}
		g_wins = n;
		g_cap = ncap;
	}
	g_wins[g_nwins++] = w;
	return true;
}

static SessWin *server_new_window(int rows, int cols, const char *cmd)
{
	if (rows < 1) {
		rows = 24;
	}
	if (cols < 1) {
		cols = 80;
	}
	SessWin *w = calloc(1, sizeof(*w));
	if (!w) {
		return NULL;
	}
	w->id = server_alloc_id();
	w->rows = rows;
	w->cols = cols;
	w->pty = -1;
	if (cmd && *cmd) {
		snprintf(w->title, sizeof(w->title), "%.*s",
			 (int)sizeof(w->title) - 1, cmd);
	} else {
		snprintf(w->title, sizeof(w->title), "shell %d", w->id);
	}
	w->child = pty_spawn_cmd(rows, cols, &w->pty, cmd);
	if (w->child < 0 || !server_add(w)) {
		if (w->pty >= 0) {
			close(w->pty);
		}
		free(w);
		return NULL;
	}
	if (g_client >= 0) {
		send_window_msg(g_client, w->id, w->rows, w->cols);
		if (server_refresh_title(w, true)) {
			send_title_msg(g_client, w->id, w->title);
		}
	}
	return w;
}

static void server_destroy(int idx, bool kill_child)
{
	SessWin *w = g_wins[idx];
	if (kill_child && w->child > 0) {
		killpg(w->child, SIGHUP);
	}
	if (w->pty >= 0) {
		close(w->pty);
	}
	free(w->replay);
	free(w);
	for (int i = idx; i < g_nwins - 1; i++) {
		g_wins[i] = g_wins[i + 1];
	}
	g_nwins--;
}

static void server_accept(int lfd)
{
	int fd = accept(lfd, NULL, NULL);
	if (fd < 0) {
		return;
	}
	char magic[4];
	if (!read_all(fd, magic, sizeof(magic)) ||
	    memcmp(magic, VP_SESSION_MAGIC, 4) != 0) {
		close(fd);
		return;
	}
	if (!write_all(fd, VP_SESSION_ACK, 2)) {
		close(fd);
		return;
	}
	if (g_client >= 0) {
		server_drop_client();
	}
	g_client = fd;
	for (int i = 0; i < g_nwins; i++) {
		server_send_replay(g_wins[i]);
	}
}

static bool server_client_msg(bool *shutdown)
{
	unsigned char type;
	if (!read_all(g_client, &type, 1)) {
		server_drop_client();
		return false;
	}
	if (type == MSG_NEW) {
		unsigned char h[6];
		if (!read_all(g_client, h, sizeof(h))) {
			server_drop_client();
			return false;
		}
		int len = get16(h + 4);
		if (len > VP_CMD_MAX) {
			server_drop_client(); /* not a message we can resync from */
			return false;
		}
		char cmd[VP_CMD_MAX + 1];
		if (len > 0 && !read_all(g_client, cmd, (size_t)len)) {
			server_drop_client();
			return false;
		}
		cmd[len] = '\0';
		server_new_window(get16(h), get16(h + 2), cmd);
	} else if (type == MSG_INPUT) {
		unsigned char h[8];
		if (!read_all(g_client, h, sizeof(h))) {
			server_drop_client();
			return false;
		}
		SessWin *w = server_by_id((int)get32(h));
		uint32_t len = get32(h + 4);
		char buf[8192];
		while (len > 0) {
			size_t chunk = len < sizeof(buf) ? len : sizeof(buf);
			if (!read_all(g_client, buf, chunk)) {
				server_drop_client();
				return false;
			}
			if (w && w->pty >= 0) {
				size_t off = 0;
				while (off < chunk) {
					ssize_t k = write(w->pty, buf + off,
							  chunk - off);
					if (k > 0) {
						off += (size_t)k;
					} else if (k < 0 && errno == EINTR) {
						continue;
					} else {
						break;
					}
				}
			}
			len -= (uint32_t)chunk;
		}
	} else if (type == MSG_RESIZE) {
		unsigned char h[8];
		if (!read_all(g_client, h, sizeof(h))) {
			server_drop_client();
			return false;
		}
		SessWin *w = server_by_id((int)get32(h));
		if (w) {
			w->rows = get16(h + 4);
			w->cols = get16(h + 6);
			pty_set_winsize(w->pty, w->rows, w->cols);
		}
	} else if (type == MSG_CLOSE) {
		unsigned char h[4];
		if (!read_all(g_client, h, sizeof(h))) {
			server_drop_client();
			return false;
		}
		int id = (int)get32(h);
		for (int i = 0; i < g_nwins; i++) {
			if (g_wins[i]->id == id) {
				g_wins[i]->dead = true;
				killpg(g_wins[i]->child, SIGHUP);
				break;
			}
		}
	} else if (type == MSG_SHUTDOWN) {
		*shutdown = true;
	} else {
		server_drop_client();
	}
	return true;
}

static void reap_server_children(void)
{
	int status;
	while (waitpid(-1, &status, WNOHANG) > 0) {
	}
}

int session_server_main(void)
{
	sigset_t none;
	sigemptyset(&none);
	sigprocmask(SIG_SETMASK, &none, NULL);

	signal(SIGPIPE, SIG_IGN);
	signal(SIGCHLD, SIG_IGN);

	char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
	if (!session_path(path, sizeof(path))) {
		return 1;
	}
	int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (lfd < 0) {
		return 1;
	}
	unlink(path);
	struct sockaddr_un sa = { .sun_family = AF_UNIX };
	snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", path);
	if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0 ||
	    listen(lfd, 4) != 0) {
		close(lfd);
		return 1;
	}
	chmod(path, 0600);

	bool shutdown = false;
	while (!shutdown) {
		int need = g_nwins + 2;
		struct pollfd *pfds = calloc((size_t)need, sizeof(*pfds));
		if (!pfds) {
			break;
		}
		int nfd = 0;
		pfds[nfd++] = (struct pollfd){ .fd = lfd, .events = POLLIN };
		int client_idx = -1;
		if (g_client >= 0) {
			client_idx = nfd;
			pfds[nfd++] =
				(struct pollfd){ .fd = g_client, .events = POLLIN };
		}
		int first_win = nfd;
		for (int i = 0; i < g_nwins; i++) {
			pfds[nfd++] = (struct pollfd){ .fd = g_wins[i]->pty,
						       .events = POLLIN | POLLHUP };
		}
		int timeout = (g_client >= 0 && g_nwins > 0) ? VP_TITLE_POLL_MS :
								 -1;
		int pr = poll(pfds, (nfds_t)nfd, timeout);
		if (pr < 0) {
			free(pfds);
			if (errno == EINTR) {
				continue;
			}
			break;
		}
		if (pfds[0].revents & POLLIN) {
			server_accept(lfd);
		}
		if (client_idx >= 0 && (pfds[client_idx].revents & POLLIN)) {
			server_client_msg(&shutdown);
		}
		for (int i = 0; i < g_nwins; i++) {
			if (pfds[first_win + i].revents & (POLLIN | POLLHUP)) {
				char buf[8192];
				for (;;) {
					ssize_t n = read(g_wins[i]->pty, buf, sizeof(buf));
					if (n > 0) {
						replay_append(g_wins[i], buf, (size_t)n);
						if (g_client >= 0) {
							send_data_msg(g_client, g_wins[i]->id,
								      buf, (size_t)n);
						}
					} else if (n < 0 && errno == EINTR) {
						continue;
					} else if (n < 0 && (errno == EAGAIN ||
							      errno == EWOULDBLOCK)) {
						break;
					} else {
						g_wins[i]->dead = true;
						if (g_client >= 0) {
							send_dead_msg(g_client, g_wins[i]->id);
						}
						break;
					}
				}
			}
			if (g_client >= 0 &&
			    server_refresh_title(g_wins[i], false)) {
				send_title_msg(g_client, g_wins[i]->id,
					       g_wins[i]->title);
			}
		}
		free(pfds);
		reap_server_children();
		for (int i = 0; i < g_nwins;) {
			if (g_wins[i]->dead) {
				server_destroy(i, false);
			} else {
				i++;
			}
		}
	}
	for (int i = 0; i < g_nwins;) {
		server_destroy(i, true);
	}
	server_drop_client();
	close(lfd);
	unlink(path);
	return 0;
}
