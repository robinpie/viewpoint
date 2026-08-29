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

/* selection.c - selecting text with the mouse, and the clipboard it feeds.
 *
 * A selection is two points on a window's text: the anchor where the drag
 * began and the cursor where it is now. Both are stored as *absolute*
 * scrollback rows (Window.scroll_base's coordinate space, the same one sixel
 * images are anchored in) rather than screen rows, so the highlight stays on
 * the text it was drawn over while the program keeps printing underneath it or
 * the user wheels back through history.
 *
 * The points are kept raw and unordered. sel_range() is the one place that
 * orders them, clamps them to the history that still exists, and applies the
 * selection mode (a word- or line-mode drag expands its ends outward); the
 * render path and the text extractor both ask it rather than deciding for
 * themselves.
 *
 * A copy goes three places, because no one of them works everywhere: the
 * internal register (always, and the only one a paste reads back), the host
 * terminal via OSC 52 (works over ssh, but plenty of terminals ship with it
 * disabled), and the shell command in config.clipboard_cmd (the escape hatch,
 * e.g. wl-copy).
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

/* ------------------------------------------------------------------------- */
/* Coordinates                                                               */
/* ------------------------------------------------------------------------- */

/* Visible row <-> absolute row. Visible row r shows combined index
 * (sb_count - sb_offset + r), whose absolute coordinate is the same expression
 * measured from scroll_base - which makes both directions this one line. */
static int64_t row_to_abs(const Window *w, int row)
{
	return w->scroll_base - w->sb_offset + row;
}

/* The oldest / newest absolute row that still exists (retained history through
 * the bottom of the live screen). */
static int64_t abs_oldest(const Window *w)
{
	return w->scroll_base - w->sb_count;
}

static int64_t abs_newest(const Window *w)
{
	return w->scroll_base + w->rows - 1;
}

/* ------------------------------------------------------------------------- */
/* Cell classification                                                       */
/* ------------------------------------------------------------------------- */

/* Word-mode boundaries. Alphanumerics are word content, and so are the few
 * punctuation marks that show up *inside* things people double-click to grab
 * whole - paths, URLs, flags, hostnames. Anything non-ASCII counts as content
 * too: it is either a letter in some script or part of a word we have no
 * business splitting. */
static bool word_char(const VTermScreenCell *c)
{
	uint32_t ch = c->chars[0];
	if (ch == 0) {
		return false;
	}
	if (ch < 0x80) {
		return isalnum((int)ch) || strchr("_-./~:@+", (int)ch) != NULL;
	}
	return true;
}

static bool blank_cell(const VTermScreenCell *c)
{
	return c->chars[0] == 0 || c->chars[0] == ' ';
}

static int word_start(const Window *w, int64_t row, int col)
{
	VTermScreenCell cell;
	if (!vt_cell_abs(w, row, col, &cell) || !word_char(&cell)) {
		return col;
	}
	while (col > 0) {
		if (!vt_cell_abs(w, row, col - 1, &cell) || !word_char(&cell)) {
			break;
		}
		col--;
	}
	return col;
}

/* Exclusive end. */
static int word_end(const Window *w, int64_t row, int col)
{
	VTermScreenCell cell;
	if (!vt_cell_abs(w, row, col, &cell) || !word_char(&cell)) {
		return col + 1;
	}
	int width = vt_row_width(w, row);
	while (col + 1 < width) {
		if (!vt_cell_abs(w, row, col + 1, &cell) || !word_char(&cell)) {
			break;
		}
		col++;
	}
	return col + 1;
}

/* ------------------------------------------------------------------------- */
/* Selection state                                                           */
/* ------------------------------------------------------------------------- */

void sel_clear(Window *w)
{
	if (!w || !w->sel_active) {
		return;
	}
	w->sel_active = false;
	window_damage_content(w, true);
}

void sel_clear_all(WM *wm)
{
	for (int i = 0; i < wm->nwins; i++) {
		sel_clear(wm->wins[i]);
	}
}

void sel_start(WM *wm, Window *w, int row, int col, vp_selmode mode)
{
	if (!w) {
		return;
	}
	sel_clear_all(wm); /* only one selection is live at a time */
	w->sel_active = true;
	w->sel_moved = false;
	w->sel_mode = mode;
	w->sel_ar = w->sel_br = row_to_abs(w, row);
	w->sel_ac = w->sel_bc = col;
	window_damage_content(w, true);
}

void sel_extend(Window *w, int row, int col)
{
	if (!w || !w->sel_active) {
		return;
	}
	int64_t abs = row_to_abs(w, row);
	if (abs == w->sel_br && col == w->sel_bc) {
		return;
	}
	w->sel_br = abs;
	w->sel_bc = col;
	w->sel_moved = true;
	window_damage_content(w, true);
}

bool sel_range(const Window *w, int64_t *r0_out, int *c0_out, int64_t *r1_out,
	       int *c1_out)
{
	if (!w || !w->sel_active) {
		return false;
	}
	/* A press that never moved is a click, not a selection: it sets the anchor
	 * (so a drag can grow from it) but selects nothing, or every click would
	 * leave a stray highlighted cell behind. Word and line modes are the
	 * exception - there the click itself is the whole gesture. */
	if (!w->sel_moved && w->sel_mode == SEL_CHAR) {
		return false;
	}
	int64_t r0 = w->sel_ar, r1 = w->sel_br;
	int c0 = w->sel_ac, c1 = w->sel_bc;
	if (r1 < r0 || (r1 == r0 && c1 < c0)) {
		int64_t tr = r0;
		int tc = c0;
		r0 = r1;
		c0 = c1;
		r1 = tr;
		c1 = tc;
	}

	/* Clamp to the text that still exists: history the ring has since evicted
	 * takes the top of the selection with it. */
	int64_t oldest = abs_oldest(w), newest = abs_newest(w);
	if (r1 < oldest || r0 > newest) {
		return false;
	}
	if (r0 < oldest) {
		r0 = oldest;
		c0 = 0;
	}
	if (r1 > newest) {
		r1 = newest;
		c1 = w->cols - 1;
	}

	int start = c0, end = c1 + 1; /* end is exclusive */
	switch (w->sel_mode) {
	case SEL_LINE:
		start = 0;
		end = INT_MAX; /* clamped per row against its own width */
		break;
	case SEL_WORD:
		start = word_start(w, r0, c0);
		end = word_end(w, r1, c1);
		break;
	case SEL_CHAR:
		break;
	}
	if (start < 0) {
		start = 0;
	}

	*r0_out = r0;
	*c0_out = start;
	*r1_out = r1;
	*c1_out = end;
	return true;
}

/* The selected columns of one absolute row, clamped to what that row holds.
 * `width` is how far the caller is willing to go (the row's own width when
 * extracting text, the window width when painting - a selection that runs past
 * a short history line still highlights the blanks after it). */
static bool row_span_abs(const Window *w, int64_t abs, int width, int *c0_out,
			 int *c1_out)
{
	int64_t r0, r1;
	int c0, c1;
	if (!sel_range(w, &r0, &c0, &r1, &c1)) {
		return false;
	}
	if (abs < r0 || abs > r1) {
		return false;
	}
	int start = (abs == r0) ? c0 : 0;
	int end = (abs == r1) ? c1 : INT_MAX;
	if (end > width) {
		end = width;
	}
	if (start >= end) {
		return false;
	}
	*c0_out = start;
	*c1_out = end;
	return true;
}

bool sel_row_span(const Window *w, int row, int *c0, int *c1)
{
	if (!w || !w->sel_active) {
		return false;
	}
	return row_span_abs(w, row_to_abs(w, row), w->cols, c0, c1);
}

/* ------------------------------------------------------------------------- */
/* Text extraction                                                           */
/* ------------------------------------------------------------------------- */

/* A growable byte buffer. Append failures leave `ok` false and are checked
 * once at the end rather than at every call site. */
typedef struct {
	char *buf;
	size_t len, cap;
	bool ok;
} strbuf;

static void sb_put(strbuf *sb, const char *bytes, size_t n)
{
	if (!sb->ok) {
		return;
	}
	if (sb->len + n + 1 > sb->cap) {
		size_t ncap = sb->cap ? sb->cap * 2 : 256;
		while (ncap < sb->len + n + 1) {
			ncap *= 2;
		}
		char *nb = realloc(sb->buf, ncap);
		if (!nb) {
			sb->ok = false;
			return;
		}
		sb->buf = nb;
		sb->cap = ncap;
	}
	memcpy(sb->buf + sb->len, bytes, n);
	sb->len += n;
	sb->buf[sb->len] = '\0';
}

static size_t utf8_put(uint32_t cp, char *out)
{
	if (cp < 0x80) {
		out[0] = (char)cp;
		return 1;
	}
	if (cp < 0x800) {
		out[0] = (char)(0xC0 | (cp >> 6));
		out[1] = (char)(0x80 | (cp & 0x3F));
		return 2;
	}
	if (cp < 0x10000) {
		out[0] = (char)(0xE0 | (cp >> 12));
		out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
		out[2] = (char)(0x80 | (cp & 0x3F));
		return 3;
	}
	out[0] = (char)(0xF0 | (cp >> 18));
	out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
	out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
	out[3] = (char)(0x80 | (cp & 0x3F));
	return 4;
}

char *sel_text(const Window *w, size_t *len_out)
{
	int64_t r0, r1;
	int c0, c1;
	if (!sel_range(w, &r0, &c0, &r1, &c1)) {
		return NULL;
	}

	strbuf sb = { NULL, 0, 0, true };
	for (int64_t r = r0; r <= r1; r++) {
		if (r != r0) {
			sb_put(&sb, "\n", 1);
		}
		int width = vt_row_width(w, r);
		int start, end;
		if (!row_span_abs(w, r, width, &start, &end)) {
			continue; /* nothing selected on this row: a bare newline */
		}
		/* Blanks at the end of a row are the grid padding the line out, not
		 * text anyone selected - but only when the span actually runs to the
		 * row's edge. A span the user ended mid-row is taken as written,
		 * spaces and all, which is what makes selecting a run of whitespace
		 * possible at all. */
		bool trim = end >= width;
		size_t line0 = sb.len;
		size_t keep = sb.len;
		for (int col = start; col < end; col++) {
			VTermScreenCell cell;
			if (!vt_cell_abs(w, r, col, &cell)) {
				break;
			}
			if (cell.width == 0) {
				continue; /* trailing half of a wide glyph */
			}
			if (cell.chars[0] == 0) {
				sb_put(&sb, " ", 1);
			} else {
				char egc[VTERM_MAX_CHARS_PER_CELL * 4];
				size_t n = 0;
				for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL &&
						cell.chars[i];
				     i++) {
					n += utf8_put(cell.chars[i], egc + n);
				}
				sb_put(&sb, egc, n);
			}
			if (!blank_cell(&cell)) {
				keep = sb.len;
			}
		}
		if (trim && keep < sb.len && keep >= line0) {
			sb.len = keep;
			if (sb.buf) {
				sb.buf[sb.len] = '\0';
			}
		}
	}

	if (!sb.ok || sb.len == 0) {
		free(sb.buf);
		return NULL;
	}
	if (len_out) {
		*len_out = sb.len;
	}
	return sb.buf;
}

/* ------------------------------------------------------------------------- */
/* Clipboard                                                                 */
/* ------------------------------------------------------------------------- */

static const char b64_alphabet[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 into a malloc'd NUL-terminated string, or NULL. */
static char *base64(const char *in, size_t len)
{
	size_t out_len = ((len + 2) / 3) * 4;
	char *out = malloc(out_len + 1);
	if (!out) {
		return NULL;
	}
	size_t o = 0;
	for (size_t i = 0; i < len; i += 3) {
		unsigned v = (unsigned char)in[i] << 16;
		if (i + 1 < len) {
			v |= (unsigned char)in[i + 1] << 8;
		}
		if (i + 2 < len) {
			v |= (unsigned char)in[i + 2];
		}
		out[o++] = b64_alphabet[(v >> 18) & 0x3F];
		out[o++] = b64_alphabet[(v >> 12) & 0x3F];
		out[o++] = (i + 1 < len) ? b64_alphabet[(v >> 6) & 0x3F] : '=';
		out[o++] = (i + 2 < len) ? b64_alphabet[v & 0x3F] : '=';
	}
	out[o] = '\0';
	return out;
}

/* Hand the text to the host terminal's own clipboard. Meaningless on the bare
 * console (nothing is listening), and skipped for very long selections. */
static void clipboard_to_host(const WM *wm, const char *text, size_t len)
{
	if (wm->console || len == 0 || len > VP_OSC52_MAX) {
		return;
	}
	char *b64 = base64(text, len);
	if (!b64) {
		return;
	}
	size_t blen = strlen(b64);
	char *seq = malloc(blen + 16);
	if (seq) {
		int head = snprintf(seq, blen + 16, "\x1b]52;c;%s\a", b64);
		if (head > 0) {
			size_t total = (size_t)head, off = 0;
			while (off < total) {
				ssize_t n = write(STDOUT_FILENO, seq + off,
						  total - off);
				if (n <= 0) {
					break;
				}
				off += (size_t)n;
			}
		}
		free(seq);
	}
	free(b64);
}

/* Pipe the text into the user's clipboard helper. The write is non-blocking
 * with a short deadline: a helper that never drains us must not stall the
 * event loop (and so the whole UI) on a copy. The child is left for the main
 * loop's reaper, which already collects everything. */
static void clipboard_to_command(const char *cmd, const char *text, size_t len)
{
	if (!cmd || !*cmd || len == 0) {
		return;
	}
	int fds[2];
	if (pipe(fds) != 0) {
		vp_log("clipboard: pipe failed: %s\n", strerror(errno));
		return;
	}
	pid_t pid = fork();
	if (pid < 0) {
		vp_log("clipboard: fork failed: %s\n", strerror(errno));
		close(fds[0]);
		close(fds[1]);
		return;
	}
	if (pid == 0) {
		close(fds[1]);
		dup2(fds[0], STDIN_FILENO);
		if (fds[0] != STDIN_FILENO) {
			close(fds[0]);
		}
		/* The helper's own chatter would land on the screen we are drawing. */
		int devnull = open("/dev/null", O_WRONLY);
		if (devnull >= 0) {
			dup2(devnull, STDOUT_FILENO);
			dup2(devnull, STDERR_FILENO);
			if (devnull > STDERR_FILENO) {
				close(devnull);
			}
		}
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}

	close(fds[0]);
	int flags = fcntl(fds[1], F_GETFL, 0);
	if (flags >= 0) {
		fcntl(fds[1], F_SETFL, flags | O_NONBLOCK);
	}
	size_t off = 0;
	const int deadline_ms = 250;
	int spent = 0;
	while (off < len && spent < deadline_ms) {
		ssize_t n = write(fds[1], text + off, len - off);
		if (n > 0) {
			off += (size_t)n;
			continue;
		}
		if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
			struct pollfd pfd = { fds[1], POLLOUT, 0 };
			int rc = poll(&pfd, 1, 25);
			spent += 25;
			if (rc <= 0) {
				continue;
			}
			continue;
		}
		break; /* EPIPE or another hard error: the helper is gone */
	}
	if (off < len) {
		vp_log("clipboard: '%s' took only %zu of %zu bytes\n", cmd, off,
		       len);
	}
	close(fds[1]);
	/* Best-effort immediate reap; the main loop's reaper gets the rest. */
	int status;
	(void)waitpid(pid, &status, WNOHANG);
}

void clipboard_set(WM *wm, const char *text, size_t len)
{
	if (!text || len == 0) {
		return;
	}
	char *copy = malloc(len + 1);
	if (!copy) {
		return;
	}
	memcpy(copy, text, len);
	copy[len] = '\0';
	free(wm->clipboard);
	wm->clipboard = copy;
	wm->clip_len = len;

	clipboard_to_host(wm, text, len);
	clipboard_to_command(wm->config.clipboard_cmd, text, len);
	vp_log("clipboard: %zu bytes copied\n", len);
}

const char *clipboard_get(const WM *wm, size_t *len)
{
	if (len) {
		*len = wm->clip_len;
	}
	return wm->clipboard;
}

void clipboard_free(WM *wm)
{
	free(wm->clipboard);
	wm->clipboard = NULL;
	wm->clip_len = 0;
}

void sel_copy(WM *wm, Window *w)
{
	if (!w) {
		return;
	}
	size_t len = 0;
	char *text = sel_text(w, &len);
	if (!text) {
		return;
	}
	clipboard_set(wm, text, len);
	free(text);
}

void clipboard_paste(WM *wm, Window *w)
{
	if (!w || !wm->clipboard || wm->clip_len == 0) {
		return;
	}
	vt_paste(w, wm->clipboard, wm->clip_len);
}
