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

/* config_test.c - a setting's round trip through config.c.
 *
 * Every in-app setting has the same five obligations: a default, a parse, a
 * write that stays quiet at the default, a reload that agrees with the write,
 * and an honest answer about whether the manual section shadows it. This walks
 * size_indicator_fade through all five; a new setting can copy the shape.
 *
 * $XDG_CONFIG_HOME is repointed at a temporary directory, so a run never reads
 * or writes the developer's real config.
 */
#define _GNU_SOURCE
#include "../../viewpoint.h"

#include "check.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static char tmpdir[256];

static void conf_path(char *buf, size_t n)
{
	if (!config_path(buf, n)) {
		fprintf(stderr, "config_path failed\n");
		exit(2);
	}
}

static void write_conf(const char *body)
{
	char path[512];
	conf_path(path, sizeof(path));

	/* config_save makes its own parents; a hand-written file needs them too. */
	char dir[512];
	snprintf(dir, sizeof(dir), "%s", path);
	char *slash = strrchr(dir, '/');
	if (slash) {
		*slash = '\0';
		char parents[600];
		snprintf(parents, sizeof(parents), "mkdir -p '%s'", dir);
		if (system(parents) != 0) {
			fprintf(stderr, "could not create %s\n", dir);
			exit(2);
		}
	}
	FILE *f = fopen(path, "w");
	if (!f) {
		fprintf(stderr, "could not write %s\n", path);
		exit(2);
	}
	fputs(body, f);
	fclose(f);
}

static const char *read_conf(void)
{
	static char buf[8192];
	char path[512];
	conf_path(path, sizeof(path));
	FILE *f = fopen(path, "r");
	size_t n = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
	buf[n] = '\0';
	if (f) {
		fclose(f);
	}
	return buf;
}

static void wipe_conf(void)
{
	char cmd[600];
	snprintf(cmd, sizeof(cmd), "rm -rf '%s'/*", tmpdir);
	if (system(cmd) != 0) {
		/* an empty directory is fine */
	}
}

int main(void)
{
	snprintf(tmpdir, sizeof(tmpdir), "/tmp/vp-config-test-%ld",
		 (long)getpid());
	if (mkdir(tmpdir, 0700) != 0) {
		fprintf(stderr, "could not create %s\n", tmpdir);
		return 2;
	}
	setenv("XDG_CONFIG_HOME", tmpdir, 1);

	VpConfig c;

	printf("default:\n");
	config_defaults(&c);
	CHECK(c.sizeosd_fade == true, "the fade starts on");
	config_free(&c);

	printf("parsing:\n");
	static const struct {
		const char *val;
		bool want;
	} spellings[] = {
		{ "true", true },   { "1", true },  { "yes", true },
		{ "false", false }, { "0", false }, { "no", false },
	};
	for (size_t i = 0; i < sizeof(spellings) / sizeof(spellings[0]); i++) {
		char body[128];
		snprintf(body, sizeof(body), "size_indicator_fade = %s\n",
			 spellings[i].val);
		write_conf(body);
		config_defaults(&c);
		config_load(&c);
		CHECK(c.sizeosd_fade == spellings[i].want, "\"%s\" -> %s",
		      spellings[i].val, spellings[i].want ? "on" : "off");
		config_free(&c);
	}

	write_conf("size_indicator_fade = banana\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(c.sizeosd_fade == true, "a value it can't read leaves the default alone");
	config_free(&c);

	printf("saving:\n");
	wipe_conf();
	config_defaults(&c);
	config_save(&c);
	CHECK(strstr(read_conf(), "size_indicator_fade") == NULL,
	      "at the default, the key is left out of the file entirely");
	c.sizeosd_fade = false;
	config_save(&c);
	CHECK(strstr(read_conf(), "size_indicator_fade = false") != NULL,
	      "changed from the default, the key is written");
	config_free(&c);

	printf("reloading:\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(c.sizeosd_fade == false, "what was saved is what comes back");
	config_free(&c);

	printf("manual section:\n");
	write_conf("# manual configuration:\nsize_indicator_fade = false\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(c.sizeosd_fade == false, "a hand-written value is honored");
	CHECK(config_manual_shadows_setting(&c, SETTING_SIZEOSD_FADE),
	      "and is reported as shadowing, so the panel can warn");
	config_free(&c);

	write_conf("# manual configuration:\ntheme = forest\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(!config_manual_shadows_setting(&c, SETTING_SIZEOSD_FADE),
	      "an unrelated hand-written key is not reported as shadowing");
	config_free(&c);

	printf("clipboard_command (a string setting, same five obligations):\n");
	config_defaults(&c);
	CHECK(c.clipboard_cmd == NULL, "no clipboard helper by default");
	config_free(&c);

	wipe_conf();
	write_conf("clipboard_command = wl-copy\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(c.clipboard_cmd && strcmp(c.clipboard_cmd, "wl-copy") == 0,
	      "a command is parsed (\"%s\")",
	      c.clipboard_cmd ? c.clipboard_cmd : "(null)");
	config_free(&c);

	write_conf("clipboard_command =\n");
	config_defaults(&c);
	config_load(&c);
	CHECK(c.clipboard_cmd == NULL, "an empty value clears it again");
	config_free(&c);

	wipe_conf();
	config_defaults(&c);
	config_save(&c);
	CHECK(strstr(read_conf(), "clipboard_command") == NULL,
	      "unset, the key is left out of the file entirely");
	c.clipboard_cmd = strdup("xclip -selection clipboard");
	config_save(&c);
	CHECK(strstr(read_conf(),
		     "clipboard_command = xclip -selection clipboard") != NULL,
	      "set, it is written whole - arguments and all");
	config_free(&c);

	config_defaults(&c);
	config_load(&c);
	CHECK(c.clipboard_cmd &&
		      strcmp(c.clipboard_cmd, "xclip -selection clipboard") == 0,
	      "and comes back the same on reload");
	config_free(&c);

	char cleanup[600];
	snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", tmpdir);
	if (system(cleanup) != 0) {
		fprintf(stderr, "warning: could not remove %s\n", tmpdir);
	}
	return vp_test_report();
}
