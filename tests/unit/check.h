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

/* check.h - the whole test framework, which is one macro and a counter.
 *
 * Every check prints its own line whether it passed or not, so a run reads as
 * a description of the behavior rather than as silence punctuated by failures.
 */
#ifndef VP_TEST_CHECK_H
#define VP_TEST_CHECK_H

#include <stdio.h>

static int vp_test_fails;

#define CHECK(cond, ...)                                  \
	do {                                              \
		bool _ok = (cond);                        \
		printf("  %s: ", _ok ? "ok  " : "FAIL");  \
		printf(__VA_ARGS__);                      \
		printf("\n");                             \
		if (!_ok) {                               \
			vp_test_fails++;                  \
		}                                         \
	} while (0)

static inline int vp_test_report(void)
{
	printf("\n");
	if (vp_test_fails) {
		printf("%d check(s) FAILED\n", vp_test_fails);
		return 1;
	}
	printf("all checks passed\n");
	return 0;
}

/* viewpoint's logger lives in main.c, which these binaries replace. */
void vp_log(const char *fmt, ...)
{
	(void)fmt;
}

#endif /* VP_TEST_CHECK_H */
