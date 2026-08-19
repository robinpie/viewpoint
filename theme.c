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

/* theme.c - the built-in color presets and the per-color override table.
 *
 * The whole UI reads its colors from one VpTheme (WM.theme). Preset 0
 * ("midnight") reproduces the original hardcoded look, so theming is invisible
 * until the user opts into another preset. g_fields[] lets the config and the
 * Appearance editor address any individual color by a stable name. */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <stddef.h>
#include <string.h>

void vp_setfg(struct ncplane *p, vp_rgb c)
{
	ncplane_set_fg_rgb8(p, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

void vp_setbg(struct ncplane *p, vp_rgb c)
{
	ncplane_set_bg_rgb8(p, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
}

static const VpTheme g_themes[] = {
	{
		.name = "midnight",
		.win_focus_fg = 0xffffff,
		.win_focus_bg = 0x204080,
		.win_unfocus_fg = 0xc0c0c0,
		.win_unfocus_bg = 0x303030,
		.bar_fg = 0xd0d0d0,
		.bar_bg = 0x181820,
		.bar_focus_fg = 0xffffff,
		.bar_focus_bg = 0x204080,
		.bar_min_fg = 0x808080,
		.bar_min_bg = 0x181820,
		.bar_slot_fg = 0xd0d0d0,
		.bar_slot_bg = 0x282830,
		.bar_arrow_bg = 0x282830,
		.bar_mode_fg = 0x101010,
		.bar_pass_bg = 0xe0a030,
		.bar_interp_bg = 0x40c060,
		.panel_fg = 0xd0d0d8,
		.panel_bg = 0x1c1c28,
		.panel_sel_fg = 0xffffff,
		.panel_sel_bg = 0x2050a0,
		.panel_cap_fg = 0x101010,
		.panel_cap_bg = 0xe0a030,
		.panel_accent = 0x6080c0,
		.panel_status = 0xc0c080,
		.panel_hint = 0x808090,
		.icon_fg = 0xe0e0e8,
		.icon_bg = 0x2a2a3c,
		.icon_border = 0x6080c0,
		.icon_glyph = 0x9ad0ff,
		.exit_fg = 0xf0e0e0,
		.exit_bg = 0x3c2a2a,
		.exit_border = 0xc06060,
		.exit_glyph = 0xff9a9a,
		.cursor_fg = 0xffffff,
		.cursor_bg = 0x000000,
		.snap_outline = 0x60d0ff,
		.bg_fg = 0x40444c,
		.bg_bg = 0x101216,
		.bg_mode = BG_SOLID,
		.bg_fit = FIT_STRETCH,
		.bg_glyph = "·",
	},
	{
		.name = "paper", /* light: dark chrome on a pale desktop */
		.win_focus_fg = 0xffffff,
		.win_focus_bg = 0x3060c0,
		.win_unfocus_fg = 0x404040,
		.win_unfocus_bg = 0xd8d8d0,
		.bar_fg = 0x303030,
		.bar_bg = 0xe8e8e0,
		.bar_focus_fg = 0xffffff,
		.bar_focus_bg = 0x3060c0,
		.bar_min_fg = 0x909090,
		.bar_min_bg = 0xe8e8e0,
		.bar_slot_fg = 0x303030,
		.bar_slot_bg = 0xd0d0c8,
		.bar_arrow_bg = 0xd0d0c8,
		.bar_mode_fg = 0x101010,
		.bar_pass_bg = 0xe0a030,
		.bar_interp_bg = 0x40b060,
		.panel_fg = 0x202020,
		.panel_bg = 0xf0f0e8,
		.panel_sel_fg = 0xffffff,
		.panel_sel_bg = 0x3060c0,
		.panel_cap_fg = 0x101010,
		.panel_cap_bg = 0xe0a030,
		.panel_accent = 0x3060c0,
		.panel_status = 0x606020,
		.panel_hint = 0x808080,
		.icon_fg = 0x202020,
		.icon_bg = 0xdadae4,
		.icon_border = 0x3060c0,
		.icon_glyph = 0x3060c0,
		.exit_fg = 0x402020,
		.exit_bg = 0xe6d2d2,
		.exit_border = 0xc06060,
		.exit_glyph = 0xc04040,
		.cursor_fg = 0x000000,
		.cursor_bg = 0xffffff,
		.snap_outline = 0x3060c0,
		.bg_fg = 0xc0c0b8,
		.bg_bg = 0xdcdcd4,
		.bg_mode = BG_SOLID,
		.bg_fit = FIT_STRETCH,
		.bg_glyph = " ",
	},
	{
		.name = "forest", /* greens, with a floral pattern desktop */
		.win_focus_fg = 0xeefff0,
		.win_focus_bg = 0x1f5030,
		.win_unfocus_fg = 0xa8c0a8,
		.win_unfocus_bg = 0x223526,
		.bar_fg = 0xc8e0c8,
		.bar_bg = 0x12200f,
		.bar_focus_fg = 0xffffff,
		.bar_focus_bg = 0x1f5030,
		.bar_min_fg = 0x6f846f,
		.bar_min_bg = 0x12200f,
		.bar_slot_fg = 0xc8e0c8,
		.bar_slot_bg = 0x1c3018,
		.bar_arrow_bg = 0x1c3018,
		.bar_mode_fg = 0x0a0a0a,
		.bar_pass_bg = 0xd8a020,
		.bar_interp_bg = 0x50c060,
		.panel_fg = 0xd0e8d0,
		.panel_bg = 0x16241a,
		.panel_sel_fg = 0xffffff,
		.panel_sel_bg = 0x1f5030,
		.panel_cap_fg = 0x101010,
		.panel_cap_bg = 0xd8a020,
		.panel_accent = 0x4f9060,
		.panel_status = 0xc0c080,
		.panel_hint = 0x789078,
		.icon_fg = 0xe0f0e0,
		.icon_bg = 0x1c3420,
		.icon_border = 0x4f9060,
		.icon_glyph = 0x8fd0a0,
		.exit_fg = 0xf0e0e0,
		.exit_bg = 0x34221c,
		.exit_border = 0xc07050,
		.exit_glyph = 0xffae8a,
		.cursor_fg = 0xffffff,
		.cursor_bg = 0x000000,
		.snap_outline = 0x70e090,
		.bg_fg = 0x2c5a36,
		.bg_bg = 0x0c160a,
		.bg_mode = BG_PATTERN,
		.bg_fit = FIT_STRETCH,
		.bg_glyph = "❀ ",
	},
	{
		.name = "amber", /* warm retro phosphor */
		.win_focus_fg = 0x201400,
		.win_focus_bg = 0xd89030,
		.win_unfocus_fg = 0xb08850,
		.win_unfocus_bg = 0x2a2018,
		.bar_fg = 0xe0b070,
		.bar_bg = 0x1a1208,
		.bar_focus_fg = 0x201400,
		.bar_focus_bg = 0xd89030,
		.bar_min_fg = 0x7a6440,
		.bar_min_bg = 0x1a1208,
		.bar_slot_fg = 0xe0b070,
		.bar_slot_bg = 0x2a1e10,
		.bar_arrow_bg = 0x2a1e10,
		.bar_mode_fg = 0x100a00,
		.bar_pass_bg = 0xe0a030,
		.bar_interp_bg = 0xb0c040,
		.panel_fg = 0xe8c890,
		.panel_bg = 0x201808,
		.panel_sel_fg = 0x201400,
		.panel_sel_bg = 0xd89030,
		.panel_cap_fg = 0x100a00,
		.panel_cap_bg = 0xe0c040,
		.panel_accent = 0xd8a850,
		.panel_status = 0xd0b060,
		.panel_hint = 0x8a7040,
		.icon_fg = 0x201400,
		.icon_bg = 0x2e2210,
		.icon_border = 0xd8a850,
		.icon_glyph = 0xf0c060,
		.exit_fg = 0xf0d0a0,
		.exit_bg = 0x301808,
		.exit_border = 0xc07030,
		.exit_glyph = 0xff9a4a,
		.cursor_fg = 0xffd080,
		.cursor_bg = 0x000000,
		.snap_outline = 0xf0b040,
		.bg_fg = 0x5a4418,
		.bg_bg = 0x100b04,
		.bg_mode = BG_SOLID,
		.bg_fit = FIT_STRETCH,
		.bg_glyph = "·",
	},
	{
		.name = "mono", /* grayscale */
		.win_focus_fg = 0x000000,
		.win_focus_bg = 0xc0c0c0,
		.win_unfocus_fg = 0x909090,
		.win_unfocus_bg = 0x202020,
		.bar_fg = 0xc0c0c0,
		.bar_bg = 0x101010,
		.bar_focus_fg = 0x000000,
		.bar_focus_bg = 0xc0c0c0,
		.bar_min_fg = 0x686868,
		.bar_min_bg = 0x101010,
		.bar_slot_fg = 0xc0c0c0,
		.bar_slot_bg = 0x282828,
		.bar_arrow_bg = 0x282828,
		.bar_mode_fg = 0x000000,
		.bar_pass_bg = 0xb0b0b0,
		.bar_interp_bg = 0x808080,
		.panel_fg = 0xd0d0d0,
		.panel_bg = 0x181818,
		.panel_sel_fg = 0x000000,
		.panel_sel_bg = 0xc0c0c0,
		.panel_cap_fg = 0x000000,
		.panel_cap_bg = 0x909090,
		.panel_accent = 0xa0a0a0,
		.panel_status = 0xb0b0b0,
		.panel_hint = 0x707070,
		.icon_fg = 0xd0d0d0,
		.icon_bg = 0x282828,
		.icon_border = 0xa0a0a0,
		.icon_glyph = 0xe0e0e0,
		.exit_fg = 0xd0d0d0,
		.exit_bg = 0x282828,
		.exit_border = 0xa0a0a0,
		.exit_glyph = 0xe0e0e0,
		.cursor_fg = 0xffffff,
		.cursor_bg = 0x000000,
		.snap_outline = 0xe0e0e0,
		.bg_fg = 0x303030,
		.bg_bg = 0x0a0a0a,
		.bg_mode = BG_SOLID,
		.bg_fit = FIT_STRETCH,
		.bg_glyph = "░",
	},
};

#define THEME_COUNT ((int)(sizeof(g_themes) / sizeof(g_themes[0])))

const VpTheme *vp_theme_default(void)
{
	return &g_themes[0];
}

int vp_theme_count(void)
{
	return THEME_COUNT;
}

const VpTheme *vp_theme_by_index(int i)
{
	if (i < 0 || i >= THEME_COUNT) {
		return NULL;
	}
	return &g_themes[i];
}

const VpTheme *vp_theme_builtin(const char *name)
{
	if (!name) {
		return NULL;
	}
	for (int i = 0; i < THEME_COUNT; i++) {
		if (strcmp(g_themes[i].name, name) == 0) {
			return &g_themes[i];
		}
	}
	return NULL;
}

typedef struct {
	const char *name;
	size_t off;
} theme_field;

/* Stable element names for the config "color = <name> <hex>" key and the
 * Appearance editor. Order is cosmetic (it's the editor's list order). */
static const theme_field g_fields[] = {
	{ "title_focus_fg", offsetof(VpTheme, win_focus_fg) },
	{ "title_focus_bg", offsetof(VpTheme, win_focus_bg) },
	{ "title_unfocus_fg", offsetof(VpTheme, win_unfocus_fg) },
	{ "title_unfocus_bg", offsetof(VpTheme, win_unfocus_bg) },
	{ "taskbar_fg", offsetof(VpTheme, bar_fg) },
	{ "taskbar_bg", offsetof(VpTheme, bar_bg) },
	{ "taskbar_focus_fg", offsetof(VpTheme, bar_focus_fg) },
	{ "taskbar_focus_bg", offsetof(VpTheme, bar_focus_bg) },
	{ "taskbar_min_fg", offsetof(VpTheme, bar_min_fg) },
	{ "taskbar_min_bg", offsetof(VpTheme, bar_min_bg) },
	{ "taskbar_slot_fg", offsetof(VpTheme, bar_slot_fg) },
	{ "taskbar_slot_bg", offsetof(VpTheme, bar_slot_bg) },
	{ "taskbar_arrow_bg", offsetof(VpTheme, bar_arrow_bg) },
	{ "mode_fg", offsetof(VpTheme, bar_mode_fg) },
	{ "mode_passthrough_bg", offsetof(VpTheme, bar_pass_bg) },
	{ "mode_interpret_bg", offsetof(VpTheme, bar_interp_bg) },
	{ "panel_fg", offsetof(VpTheme, panel_fg) },
	{ "panel_bg", offsetof(VpTheme, panel_bg) },
	{ "panel_sel_fg", offsetof(VpTheme, panel_sel_fg) },
	{ "panel_sel_bg", offsetof(VpTheme, panel_sel_bg) },
	{ "panel_capture_fg", offsetof(VpTheme, panel_cap_fg) },
	{ "panel_capture_bg", offsetof(VpTheme, panel_cap_bg) },
	{ "panel_accent", offsetof(VpTheme, panel_accent) },
	{ "panel_status", offsetof(VpTheme, panel_status) },
	{ "panel_hint", offsetof(VpTheme, panel_hint) },
	{ "icon_fg", offsetof(VpTheme, icon_fg) },
	{ "icon_bg", offsetof(VpTheme, icon_bg) },
	{ "icon_border", offsetof(VpTheme, icon_border) },
	{ "icon_glyph", offsetof(VpTheme, icon_glyph) },
	{ "exit_fg", offsetof(VpTheme, exit_fg) },
	{ "exit_bg", offsetof(VpTheme, exit_bg) },
	{ "exit_border", offsetof(VpTheme, exit_border) },
	{ "exit_glyph", offsetof(VpTheme, exit_glyph) },
	{ "cursor_fg", offsetof(VpTheme, cursor_fg) },
	{ "cursor_bg", offsetof(VpTheme, cursor_bg) },
	{ "snap_outline", offsetof(VpTheme, snap_outline) },
	{ "desktop_fg", offsetof(VpTheme, bg_fg) },
	{ "desktop_bg", offsetof(VpTheme, bg_bg) },
};
#define FIELD_COUNT ((int)(sizeof(g_fields) / sizeof(g_fields[0])))

int vp_theme_field_count(void)
{
	return FIELD_COUNT;
}

const char *vp_theme_field_name(int idx)
{
	if (idx < 0 || idx >= FIELD_COUNT) {
		return NULL;
	}
	return g_fields[idx].name;
}

int vp_theme_field_index(const char *name)
{
	if (!name) {
		return -1;
	}
	for (int i = 0; i < FIELD_COUNT; i++) {
		if (strcmp(g_fields[i].name, name) == 0) {
			return i;
		}
	}
	return -1;
}

vp_rgb vp_theme_field_get(const VpTheme *t, int idx)
{
	if (idx < 0 || idx >= FIELD_COUNT) {
		return 0;
	}
	return *(const vp_rgb *)((const char *)t + g_fields[idx].off);
}

void vp_theme_field_set(VpTheme *t, int idx, vp_rgb c)
{
	if (idx < 0 || idx >= FIELD_COUNT) {
		return;
	}
	*(vp_rgb *)((char *)t + g_fields[idx].off) = c;
}
