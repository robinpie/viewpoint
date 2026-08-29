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

/* sel_test.c - what a selection covers, and what comes out of it.
 *
 * Selection is the one part of the feature that can be checked without a
 * terminal at all: it is a pure question about two points over a window's
 * text. The window here is built by hand - a real libvterm screen for the live
 * rows, hand-rolled sb_lines for the history - so the extractor is asked the
 * same question it gets in the app, across the seam between the two stores
 * that is the whole reason it can go wrong.
 */
#define _GNU_SOURCE
#include "../../viewpoint.h"

#include "check.h"

#include <stdlib.h>
#include <string.h>

#define ROWS 4
#define COLS 20

static WM wm;
static Window win;
static Window *winlist[1];

/* A retained history line holding `text`, padded to COLS with blanks. */
static sb_line make_line(const char *text)
{
	sb_line ln;
	ln.cols = COLS;
	ln.cells = calloc(COLS, sizeof(*ln.cells));
	for (int i = 0; i < COLS; i++) {
		ln.cells[i].width = 1;
		ln.cells[i].chars[0] =
			(size_t)i < strlen(text) ? (uint32_t)text[i] : 0;
	}
	return ln;
}

/* A window showing `live` on the live screen with `hist` lines of history
 * above it, scrolled so the whole history is visible. */
static void build(const char *live, const char **hist, int nhist)
{
	memset(&win, 0, sizeof(win));
	win.rows = ROWS;
	win.cols = COLS;
	win.wm = &wm;

	win.vt = vterm_new(ROWS, COLS);
	vterm_set_utf8(win.vt, 1);
	win.vts = vterm_obtain_screen(win.vt);
	vterm_screen_reset(win.vts, 1);
	vterm_input_write(win.vt, live, strlen(live));
	vterm_screen_flush_damage(win.vts);

	if (nhist > 0) {
		win.sb = calloc((size_t)nhist, sizeof(*win.sb));
		win.sb_cap = nhist;
		win.sb_count = nhist;
		win.sb_head = 0;
		for (int i = 0; i < nhist; i++) {
			win.sb[i] = make_line(hist[i]);
		}
	}
	/* `nhist` lines have scrolled off, and the view is wound all the way
	 * back, so visible row 0 is the oldest retained line. */
	win.scroll_base = nhist;
	win.sb_offset = nhist;

	memset(&wm, 0, sizeof(wm));
	winlist[0] = &win;
	wm.wins = winlist;
	wm.nwins = 1;
	wm.console = true; /* no OSC 52 out of a test run */
}

static void teardown(void)
{
	for (int i = 0; i < win.sb_count; i++) {
		free(win.sb[i].cells);
	}
	free(win.sb);
	vterm_free(win.vt);
	clipboard_free(&wm);
	memset(&win, 0, sizeof(win));
}

/* Select from (r0,c0) to (r1,c1) in visible rows and return the text. */
static char *pick(int r0, int c0, int r1, int c1, vp_selmode mode)
{
	sel_start(&wm, &win, r0, c0, mode);
	sel_extend(&win, r1, c1);
	return sel_text(&win, NULL);
}

static bool text_is(char *got, const char *want)
{
	bool ok = got && strcmp(got, want) == 0;
	if (!ok) {
		printf("       got %s, want \"%s\"\n", got ? got : "(null)",
		       want);
	}
	free(got);
	return ok;
}

int main(void)
{
	printf("a selection over the live screen:\n");
	build("hello world\r\nsecond line\r\n", NULL, 0);

	CHECK(text_is(pick(0, 0, 0, 4, SEL_CHAR), "hello"),
	      "chars 0..4 of row 0 -> \"hello\"");
	CHECK(text_is(pick(0, 6, 0, 19, SEL_CHAR), "world"),
	      "a span running past the text stops at the last glyph");
	CHECK(text_is(pick(0, 0, 1, 5, SEL_CHAR), "hello world\nsecond"),
	      "two rows join with a newline");

	printf("\nword and line granularity:\n");
	CHECK(text_is(pick(0, 8, 0, 8, SEL_WORD), "world"),
	      "a word-mode click inside \"world\" takes the whole word");
	CHECK(text_is(pick(0, 5, 0, 5, SEL_WORD), " "),
	      "a word-mode click on the space between takes just it");
	CHECK(text_is(pick(0, 3, 0, 3, SEL_LINE), "hello world"),
	      "a line-mode click takes the row, trailing blanks trimmed");
	CHECK(text_is(pick(0, 3, 1, 3, SEL_LINE), "hello world\nsecond line"),
	      "line mode over two rows takes both whole");

	printf("\nthe drag runs either way:\n");
	CHECK(text_is(pick(1, 5, 0, 6, SEL_CHAR), "world\nsecond"),
	      "dragging up selects the same span as dragging down");
	teardown();

	printf("\nacross the seam into retained history:\n");
	const char *hist[] = { "first old line", "second old line" };
	build("live one\r\nlive two\r\n", hist, 2);

	CHECK(text_is(pick(0, 0, 0, 4, SEL_CHAR), "first"),
	      "a span inside history reads out of the ring");
	CHECK(text_is(pick(1, 0, 2, 3, SEL_CHAR), "second old line\nlive"),
	      "a span crossing history into the live screen reads from both");
	CHECK(text_is(pick(0, 0, 3, 7, SEL_CHAR),
		      "first old line\nsecond old line\nlive one\nlive two"),
	      "the whole window, history first");

	printf("\nwhat the highlight covers:\n");
	int c0 = 0, c1 = 0;
	sel_start(&wm, &win, 1, 4, SEL_CHAR);
	sel_extend(&win, 2, 3);
	CHECK(sel_row_span(&win, 1, &c0, &c1) && c0 == 4 && c1 == COLS,
	      "the first row of a multi-row span runs to the window edge (%d..%d)",
	      c0, c1);
	CHECK(sel_row_span(&win, 2, &c0, &c1) && c0 == 0 && c1 == 4,
	      "the last row runs from column 0 to the cursor (%d..%d)", c0, c1);
	CHECK(!sel_row_span(&win, 0, &c0, &c1),
	      "a row above the selection has nothing highlighted");
	CHECK(!sel_row_span(&win, 3, &c0, &c1),
	      "a row below it has nothing highlighted either");

	printf("\nhistory the ring has since dropped:\n");
	sel_start(&wm, &win, 0, 0, SEL_CHAR);
	sel_extend(&win, 0, 5);
	/* Both retained lines age out from under the selection. */
	win.scroll_base += 10;
	int64_t r0, r1;
	int rc0, rc1;
	CHECK(!sel_range(&win, &r0, &rc0, &r1, &rc1),
	      "a selection whose text is gone reports no range");
	CHECK(sel_text(&win, NULL) == NULL, "and extracts as nothing");
	win.scroll_base -= 10;

	printf("\nthe clipboard register:\n");
	sel_start(&wm, &win, 3, 0, SEL_LINE);
	sel_copy(&wm, &win);
	size_t len = 0;
	const char *clip = clipboard_get(&wm, &len);
	CHECK(clip && strcmp(clip, "live two") == 0,
	      "a copy puts the selection in the register (\"%s\")",
	      clip ? clip : "(null)");
	CHECK(len == strlen("live two"), "with its length (%zu)", len);

	sel_clear(&win);
	CHECK(!win.sel_active, "clearing drops the selection");
	CHECK(clipboard_get(&wm, &len) != NULL,
	      "but leaves what was already copied on the clipboard");

	printf("\nwhat counts as typing (a selection dies on real input only):\n");
	static const struct {
		uint32_t id;
		const char *name;
		bool modifier;
	} keys[] = {
		{ NCKEY_LALT, "left alt", true },
		{ NCKEY_RSHIFT, "right shift", true },
		{ NCKEY_LCTRL, "left ctrl", true },
		{ NCKEY_CAPS_LOCK, "caps lock", true },
		{ 'c', "the letter c", false },
		{ NCKEY_ENTER, "enter", false },
		{ NCKEY_LEFT, "the left arrow", false },
	};
	for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
		ncinput ni = { 0 };
		ni.id = keys[i].id;
		CHECK(input_key_is_modifier(&ni) == keys[i].modifier,
		      "%s is %s", keys[i].name,
		      keys[i].modifier ?
			      "a modifier, so it leaves a selection alone" :
			      "input, so it ends one");
	}

	printf("\npaste, as the child sees it:\n");
	clipboard_set(&wm, "one\ntwo", 7);
	char out[256];
	(void)vterm_output_read(win.vt, out, sizeof(out)); /* drain */
	clipboard_paste(&wm, &win);
	size_t n = vterm_output_read(win.vt, out, sizeof(out) - 1);
	out[n] = '\0';
	CHECK(strstr(out, "one") && strstr(out, "two"),
	      "both lines reach the pty");
	CHECK(strchr(out, '\r') != NULL,
	      "the newline between them arrives as Enter");

	teardown();
	return vp_test_report();
}
