/* vt_bridge.c — the libvterm <-> notcurses ncplane bridge.
 *
 * Each window owns a VTerm instance fed by its PTY master's bytes. On any
 * screen change we (phase-1 strategy) mark the whole window dirty and re-sweep
 * the entire grid into the content plane at render time.
 */
#define _GNU_SOURCE
#include "viewpoint.h"

#include <string.h>
#include <unistd.h>
#include <errno.h>

/* ------------------------------------------------------------------------- */
/* libvterm callbacks                                                        */
/* ------------------------------------------------------------------------- */

static int cb_damage(VTermRect rect, void *user)
{
    (void)rect;
    ((Window *)user)->dirty = true;
    return 1;
}

static int cb_moverect(VTermRect dest, VTermRect src, void *user)
{
    (void)dest;
    (void)src;
    ((Window *)user)->dirty = true;
    return 1;
}

static int cb_movecursor(VTermPos pos, VTermPos oldpos, int visible, void *user)
{
    (void)oldpos;
    Window *w = user;
    w->currow = pos.row;
    w->curcol = pos.col;
    w->cursor_visible = visible != 0;
    return 1;
}

static int cb_settermprop(VTermProp prop, VTermValue *val, void *user)
{
    Window *w = user;
    switch (prop) {
    case VTERM_PROP_CURSORVISIBLE:
        w->cursor_visible = val->boolean != 0;
        break;
    case VTERM_PROP_TITLE:
        /* val->string is a fragment; accumulate into the title buffer. The
         * 'initial' flag marks the start of a fresh string. */
        if (val->string.initial) {
            w->title[0] = '\0';
        }
        {
            size_t cur = strlen(w->title);
            size_t avail = sizeof(w->title) - 1 - cur;
            size_t n = val->string.len;
            if (n > avail) {
                n = avail;
            }
            if (n > 0) {
                memcpy(w->title + cur, val->string.str, n);
                w->title[cur + n] = '\0';
            }
        }
        break;
    default:
        break;
    }
    return 1;
}

static int cb_bell(void *user)
{
    (void)user;
    /* Could flash the frame; ignored for now. */
    return 1;
}

static int cb_resize(int rows, int cols, void *user)
{
    /* We drive resizes ourselves (vt_resize); just note the dirty state. */
    (void)rows;
    (void)cols;
    ((Window *)user)->dirty = true;
    return 1;
}

static int cb_sb_pushline(int cols, const VTermScreenCell *cells, void *user)
{
    /* Scrollback not retained yet. */
    (void)cols;
    (void)cells;
    (void)user;
    return 0;
}

static int cb_sb_popline(int cols, VTermScreenCell *cells, void *user)
{
    (void)cols;
    (void)cells;
    (void)user;
    return 0;
}

static const VTermScreenCallbacks screen_cbs = {
    .damage      = cb_damage,
    .moverect    = cb_moverect,
    .movecursor  = cb_movecursor,
    .settermprop = cb_settermprop,
    .bell        = cb_bell,
    .resize      = cb_resize,
    .sb_pushline = cb_sb_pushline,
    .sb_popline  = cb_sb_popline,
    .sb_clear    = NULL,
};

/* Output callback: bytes the emulator wants to send back to the child. */
static void cb_output(const char *s, size_t len, void *user)
{
    Window *w = user;
    size_t off = 0;
    while (off < len) {
        ssize_t k = write(w->pty, s + off, len - off);
        if (k > 0) {
            off += (size_t)k;
        } else if (k < 0 && (errno == EINTR)) {
            continue;
        } else {
            /* EAGAIN or hard error: drop the rest rather than block. */
            break;
        }
    }
}

/* ------------------------------------------------------------------------- */
/* Lifecycle                                                                 */
/* ------------------------------------------------------------------------- */

void vt_init(Window *w)
{
    w->vt = vterm_new(w->rows, w->cols);
    vterm_set_utf8(w->vt, 1);

    w->vts = vterm_obtain_screen(w->vt);
    vterm_screen_set_callbacks(w->vts, &screen_cbs, w);
    vterm_screen_reset(w->vts, 1);

    vterm_output_set_callback(w->vt, cb_output, w);

    w->cursor_visible = true;
    w->dirty = true;
}

void vt_feed(Window *w, const char *bytes, size_t n)
{
    vterm_input_write(w->vt, bytes, n);
    w->dirty = true;
}

void vt_resize(Window *w, int rows, int cols)
{
    if (rows < 1) rows = 1;
    if (cols < 1) cols = 1;
    w->rows = rows;
    w->cols = cols;
    vterm_set_size(w->vt, rows, cols);
    if (w->content) {
        ncplane_resize_simple(w->content, (unsigned)rows, (unsigned)cols);
    }
    w->dirty = true;
}

void vt_free(Window *w)
{
    if (w->vt) {
        vterm_free(w->vt);
        w->vt = NULL;
        w->vts = NULL;
    }
}

/* ------------------------------------------------------------------------- */
/* Keyboard / mouse forwarding                                               */
/* ------------------------------------------------------------------------- */

void vt_key_unichar(Window *w, uint32_t c, VTermModifier mod)
{
    vterm_keyboard_unichar(w->vt, c, mod);
}

void vt_key_special(Window *w, VTermKey key, VTermModifier mod)
{
    vterm_keyboard_key(w->vt, key, mod);
}

void vt_mouse_move(Window *w, int row, int col, VTermModifier mod)
{
    vterm_mouse_move(w->vt, row, col, mod);
}

void vt_mouse_button(Window *w, int button, bool pressed, VTermModifier mod)
{
    vterm_mouse_button(w->vt, button, pressed, mod);
}

static VTermModifier vterm_mods(const ncinput *ni)
{
    VTermModifier mod = VTERM_MOD_NONE;
    if (ni->modifiers & NCKEY_MOD_SHIFT) mod |= VTERM_MOD_SHIFT;
    if (ni->modifiers & NCKEY_MOD_ALT)   mod |= VTERM_MOD_ALT;
    if (ni->modifiers & NCKEY_MOD_CTRL)  mod |= VTERM_MOD_CTRL;
    return mod;
}

void vt_send_key(Window *w, const ncinput *ni)
{
    VTermModifier mod = vterm_mods(ni);
    uint32_t id = ni->id;

    /* Map the synthesized/control keys libvterm has dedicated encodings for. */
    switch (id) {
    case NCKEY_ENTER:  vt_key_special(w, VTERM_KEY_ENTER, mod);     return;
    case NCKEY_TAB:    vt_key_special(w, VTERM_KEY_TAB, mod);       return;
    case NCKEY_BACKSPACE: vt_key_special(w, VTERM_KEY_BACKSPACE, mod); return;
    case NCKEY_ESC:    vt_key_special(w, VTERM_KEY_ESCAPE, mod);    return;
    case NCKEY_UP:     vt_key_special(w, VTERM_KEY_UP, mod);        return;
    case NCKEY_DOWN:   vt_key_special(w, VTERM_KEY_DOWN, mod);      return;
    case NCKEY_LEFT:   vt_key_special(w, VTERM_KEY_LEFT, mod);      return;
    case NCKEY_RIGHT:  vt_key_special(w, VTERM_KEY_RIGHT, mod);     return;
    case NCKEY_INS:    vt_key_special(w, VTERM_KEY_INS, mod);       return;
    case NCKEY_DEL:    vt_key_special(w, VTERM_KEY_DEL, mod);       return;
    case NCKEY_HOME:   vt_key_special(w, VTERM_KEY_HOME, mod);      return;
    case NCKEY_END:    vt_key_special(w, VTERM_KEY_END, mod);       return;
    case NCKEY_PGUP:   vt_key_special(w, VTERM_KEY_PAGEUP, mod);    return;
    case NCKEY_PGDOWN: vt_key_special(w, VTERM_KEY_PAGEDOWN, mod);  return;
    default:
        break;
    }

    /* Function keys F1..F60 map to VTERM_KEY_FUNCTION(n). */
    if (id >= NCKEY_F01 && id <= NCKEY_F60) {
        int n = (int)(id - NCKEY_F01) + 1;
        vt_key_special(w, VTERM_KEY_FUNCTION(n), mod);
        return;
    }

    /* Ignore the modifier-only and other synthesized events we don't forward. */
    if (nckey_synthesized_p(id)) {
        return;
    }

    /* A real Unicode codepoint. libvterm handles modifier + cursor-mode
     * encoding for us. */
    if (id != 0) {
        vt_key_unichar(w, id, mod);
    }
}

/* ------------------------------------------------------------------------- */
/* Rendering: sweep the grid into the content plane                          */
/* ------------------------------------------------------------------------- */

/* Append a unicode codepoint to a UTF-8 buffer; returns bytes written. */
static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    } else if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    } else if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    } else {
        out[0] = (char)(0xF0 | (cp >> 18));
        out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[3] = (char)(0x80 | (cp & 0x3F));
        return 4;
    }
}

static void set_plane_fg(struct ncplane *n, const VTermScreen *vts, VTermColor c)
{
    if (VTERM_COLOR_IS_DEFAULT_FG(&c)) {
        ncplane_set_fg_default(n);
        return;
    }
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        vterm_screen_convert_color_to_rgb(vts, &c);
    }
    ncplane_set_fg_rgb8(n, c.rgb.red, c.rgb.green, c.rgb.blue);
}

static void set_plane_bg(struct ncplane *n, const VTermScreen *vts, VTermColor c)
{
    if (VTERM_COLOR_IS_DEFAULT_BG(&c)) {
        ncplane_set_bg_default(n);
        return;
    }
    if (VTERM_COLOR_IS_INDEXED(&c)) {
        vterm_screen_convert_color_to_rgb(vts, &c);
    }
    ncplane_set_bg_rgb8(n, c.rgb.red, c.rgb.green, c.rgb.blue);
}

void vt_render(Window *w)
{
    struct ncplane *n = w->content;
    if (!n) {
        return;
    }

    for (int row = 0; row < w->rows; row++) {
        for (int col = 0; col < w->cols; col++) {
            VTermPos pos = { .row = row, .col = col };
            VTermScreenCell cell;
            if (vterm_screen_get_cell(w->vts, pos, &cell) == 0) {
                continue;
            }

            /* The trailing column of a wide glyph reports width 0; the lead
             * cell already painted it, so leave it alone. */
            if (cell.width == 0) {
                continue;
            }

            VTermColor fg = cell.fg;
            VTermColor bg = cell.bg;
            if (cell.attrs.reverse) {
                VTermColor t = fg;
                fg = bg;
                bg = t;
            }
            set_plane_fg(n, w->vts, fg);
            set_plane_bg(n, w->vts, bg);

            unsigned styles = NCSTYLE_NONE;
            if (cell.attrs.bold)      styles |= NCSTYLE_BOLD;
            if (cell.attrs.underline) styles |= NCSTYLE_UNDERLINE;
            if (cell.attrs.italic)    styles |= NCSTYLE_ITALIC;
            if (cell.attrs.strike)    styles |= NCSTYLE_STRUCK;
            ncplane_set_styles(n, styles);

            /* Build the EGC: base glyph plus any combining chars. */
            char egc[VTERM_MAX_CHARS_PER_CELL * 4 + 1];
            size_t off = 0;
            if (cell.chars[0] == 0) {
                egc[off++] = ' ';
            } else {
                for (int i = 0; i < VTERM_MAX_CHARS_PER_CELL && cell.chars[i]; i++) {
                    off += utf8_encode(cell.chars[i], egc + off);
                }
            }
            egc[off] = '\0';

            ncplane_putegc_yx(n, row, col, egc, NULL);

            if (cell.width == 2) {
                col++; /* skip the trailing half */
            }
        }
    }
    w->dirty = false;
}
