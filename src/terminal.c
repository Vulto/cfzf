/* terminal.c - raw-termios terminal renderer.
   Port of the drawing mechanics of fzf src/tui/light.go. */
#define _POSIX_C_SOURCE 200809L
#include "terminal.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <termios.h>
#include <sys/ioctl.h>
#include <signal.h>
#include <errno.h>

#define DEFAULT_WIDTH  80
#define DEFAULT_HEIGHT 24
#define DEFAULT_ESC_DELAY 100

static struct termios g_orig_termios;
static bool g_termios_saved = false;

/* ---- buffered output ---- */
static void queued_append(mm_renderer_t *r, const char *s, size_t n) {
    if (r->queued_len + n + 1 > r->queued_cap) {
        size_t nc = r->queued_cap ? r->queued_cap : 4096;
        while (r->queued_len + n + 1 > nc) nc *= 2;
        r->queued = realloc(r->queued, nc);
        r->queued_cap = nc;
    }
    memcpy(r->queued + r->queued_len, s, n);
    r->queued_len += n;
    r->queued[r->queued_len] = 0;
}

static void flush_raw(mm_renderer_t *r, const char *s, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(r->ttyout, s + off, n - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            break;
        }
        off += (size_t)w;
    }
}

static void csi_fmt(mm_renderer_t *r, const char *fmt, int a) {
    char buf[48];
    int n = snprintf(buf, sizeof(buf), fmt, a);
    if (n > 0) {
        queued_append(r, "\x1b[", 2);
        queued_append(r, buf, (size_t)n);
    }
}

void mm_renderer_csi(mm_renderer_t *r, const char *code) {
    queued_append(r, "\x1b[", 2);
    queued_append(r, code, strlen(code));
}

void mm_renderer_set_sgr(mm_renderer_t *r, const char *code) {
    if (strcmp(code, r->sgr) != 0) {
        queued_append(r, code, strlen(code));
        snprintf(r->sgr, sizeof(r->sgr), "%s", code);
    }
}

void mm_renderer_stderr(mm_renderer_t *r, const char *str) {
    queued_append(r, str, strlen(str));
}

void mm_renderer_move(mm_renderer_t *r, int y, int x) {
    /* The cursor is addressed with an absolute move: the tracking of the
       printed text is not reliable enough to compute relative moves (a
       line that fills the row completely leaves the cursor in a pending
       wrap state). */
    if (y < 0) y = 0;
    if (x < 0) x = 0;
    char buf[48];
    int n = snprintf(buf, sizeof(buf), "%d;%dH", y + 1, x + 1);
    if (n > 0) {
        queued_append(r, "\x1b[", 2);
        queued_append(r, buf, (size_t)n);
    }
    r->y = y;
    r->x = x;
}

void mm_renderer_origin(mm_renderer_t *r) { mm_renderer_move(r, 0, 0); }

void mm_renderer_flush(mm_renderer_t *r) {
    if (r->queued_len > 0) {
        mm_renderer_set_sgr(r, "\x1b[0m");
        static const char pre[] = "\x1b[?2026h\x1b[?7l\x1b[?25l";
        static const char post[] = "\x1b[?7h\x1b[?2026l";
        flush_raw(r, pre, sizeof(pre) - 1);
        flush_raw(r, r->queued, r->queued_len);
        if (r->show_cursor) flush_raw(r, "\x1b[?25h", 6);
        flush_raw(r, post, sizeof(post) - 1);
        r->queued_len = 0;
        if (r->queued) r->queued[0] = 0;
    }
}

static void enable_modes(mm_renderer_t *r) {
    if (r->mouse) {
        mm_renderer_csi(r, "?1000h");
        mm_renderer_csi(r, "?1002h");
        mm_renderer_csi(r, "?1006h");
    }
    mm_renderer_csi(r, "?2004h");
}

static void disable_modes(mm_renderer_t *r) {
    if (r->mouse) {
        mm_renderer_csi(r, "?1000l");
        mm_renderer_csi(r, "?1002l");
        mm_renderer_csi(r, "?1006l");
    }
    if (r->have_paste_was_set && r->paste_was_set)
        mm_renderer_csi(r, "?2004h");
    else
        mm_renderer_csi(r, "?2004l");
}

void mm_renderer_update_size(mm_renderer_t *r) {
    struct winsize ws;
    if (ioctl(r->ttyout, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0 && ws.ws_row > 0) {
        r->width = ws.ws_col;
        r->height = ws.ws_row;
    } else {
        const char *c = getenv("COLUMNS"), *l = getenv("LINES");
        r->width = c ? atoi(c) : DEFAULT_WIDTH;
        r->height = l ? atoi(l) : DEFAULT_HEIGHT;
        if (r->width <= 0) r->width = DEFAULT_WIDTH;
        if (r->height <= 0) r->height = DEFAULT_HEIGHT;
    }
}

int mm_renderer_max_x(mm_renderer_t *r) { return r->width; }
int mm_renderer_max_y(mm_renderer_t *r) {
    if (r->height == 0) mm_renderer_update_size(r);
    return r->height;
}
int mm_renderer_top(mm_renderer_t *r) { return r->yoffset; }

bool mm_renderer_init(mm_renderer_t *r, int ttyin, int ttyout, bool mouse,
                      int tabstop, bool clear_on_exit, bool fullscreen) {
    memset(r, 0, sizeof(*r));
    r->ttyin = ttyin;
    r->ttyout = ttyout;
    r->mouse = mouse;
    r->tabstop = tabstop ? tabstop : 8;
    r->clear_on_exit = clear_on_exit;
    r->fullscreen = fullscreen;
    r->show_cursor = true;
    r->sgr[0] = 0;
    const char *esc = getenv("ESCDELAY");
    r->esc_delay = esc ? atoi(esc) : DEFAULT_ESC_DELAY;

    if (isatty(ttyin)) {
        if (tcgetattr(ttyin, &g_orig_termios) == 0) {
            g_termios_saved = true;
            struct termios raw = g_orig_termios;
            raw.c_lflag &= (tcflag_t)~(ECHO | ICANON | IEXTEN);
            raw.c_iflag &= (tcflag_t)~(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
            raw.c_oflag &= (tcflag_t)~(OPOST);
            raw.c_cflag |= CS8;
            raw.c_cc[VMIN] = 0;
            raw.c_cc[VTIME] = 1;
            if (tcsetattr(ttyin, TCSANOW, &raw) != 0) return false;
        }
    }
    mm_renderer_update_size(r);
    if (fullscreen) {
        mm_renderer_flush(r);
        flush_raw(r, "\x1b[?1049h", 8);
    }
    enable_modes(r);
    int my = mm_renderer_max_y(r);
    if (r->inline_mode) {
        /* The inline UI is drawn inside the terminal, starting at the top. */
        mm_renderer_csi(r, "H");
        mm_renderer_csi(r, "J");
    } else {
        csi_fmt(r, "%dA", my - 1);
        mm_renderer_csi(r, "G");
        mm_renderer_csi(r, "K");
        if (!clear_on_exit && !fullscreen) mm_renderer_stderr(r, "\033""7");
    }
    return true;
}

void mm_renderer_close(mm_renderer_t *r) {
    if (r->clear_on_exit) {
        if (r->fullscreen) {
            mm_renderer_flush(r);
            flush_raw(r, "\x1b[?1049l", 8);
        } else if (r->inline_mode) {
            /* Erase the rows the inline UI occupied. */
            mm_renderer_csi(r, "H");
            for (int i = 0; i < r->inline_height; i++) {
                mm_renderer_csi(r, "K");
                if (i + 1 < r->inline_height) mm_renderer_csi(r, "1B");
            }
            mm_renderer_csi(r, "H");
        } else {
            mm_renderer_origin(r);
            if (r->up_one_line) mm_renderer_csi(r, "A");
            mm_renderer_csi(r, "J");
        }
    } else if (!r->fullscreen) {
        mm_renderer_stderr(r, "\033""8");
    }
    if (!r->show_cursor) mm_renderer_csi(r, "?25h");
    disable_modes(r);
    mm_renderer_flush(r);
    if (g_termios_saved && isatty(r->ttyin))
        tcsetattr(r->ttyin, TCSANOW, &g_orig_termios);
    free(r->queued);
    r->queued = NULL;
    r->queued_len = r->queued_cap = 0;
}

void mm_renderer_clear(mm_renderer_t *r) {
    if (r->fullscreen) mm_renderer_csi(r, "H");
    mm_renderer_origin(r);
    mm_renderer_csi(r, "J");
    mm_renderer_flush(r);
}

void mm_renderer_bell(mm_renderer_t *r) {
    flush_raw(r, "\a", 1);
}

void mm_renderer_hide_cursor(mm_renderer_t *r) { r->show_cursor = false; mm_renderer_csi(r, "?25l"); }
void mm_renderer_show_cursor(mm_renderer_t *r) { r->show_cursor = true; mm_renderer_csi(r, "?25h"); }

/* ---- color codes (port of attrCodes/colorCodes/csiColor) ---- */
static bool color_is_24(mm_color_t c) { return c > 0 && (c & (1 << 24)) != 0; }

static void append_codes(char *buf, size_t cap, size_t *len, const char *code) {
    size_t n = strlen(code);
    if (*len + n + 2 >= cap) return;
    if (*len > 0) buf[(*len)++] = ';';
    memcpy(buf + *len, code, n);
    *len += n;
    buf[*len] = 0;
}

/* Build the SGR sequence for fg/bg/ul/attr. Returns false + reset when empty. */
static bool csi_color(char *out, size_t cap, mm_color_t fg, mm_color_t bg,
                      mm_color_t ul, mm_attr_t attr) {
    char codes[256];
    size_t clen = 0;
    codes[0] = 0;
    if (!(attr & MM_ATTR_CLEAR)) {
        if ((attr & MM_BOLD) || (attr & MM_BOLDFORCE)) append_codes(codes, sizeof(codes), &clen, "1");
        if (attr & MM_DIM)    append_codes(codes, sizeof(codes), &clen, "2");
        if (attr & MM_ITALIC) append_codes(codes, sizeof(codes), &clen, "3");
        if (attr & MM_UNDERLINE) {
            switch (mm_attr_underline_style(attr)) {
            case MM_ULSTYLE_DOUBLE: append_codes(codes, sizeof(codes), &clen, "4:2"); break;
            case MM_ULSTYLE_CURLY:  append_codes(codes, sizeof(codes), &clen, "4:3"); break;
            case MM_ULSTYLE_DOTTED: append_codes(codes, sizeof(codes), &clen, "4:4"); break;
            case MM_ULSTYLE_DASHED: append_codes(codes, sizeof(codes), &clen, "4:5"); break;
            default:                append_codes(codes, sizeof(codes), &clen, "4"); break;
            }
        }
        if (attr & MM_BLINK)         append_codes(codes, sizeof(codes), &clen, "5");
        if (attr & MM_REVERSE)       append_codes(codes, sizeof(codes), &clen, "7");
        if (attr & MM_STRIKETHROUGH) append_codes(codes, sizeof(codes), &clen, "9");
    }
    char tmp[32];
    for (int pass = 0; pass < 2; pass++) {
        mm_color_t c = pass == 0 ? fg : bg;
        int offset = pass == 0 ? 0 : 10;
        if (c == MM_COL_DEFAULT) continue;
        if (color_is_24(c))
            snprintf(tmp, sizeof(tmp), "%d;2;%d;%d;%d", 38 + offset,
                     (int)((c >> 16) & 0xff), (int)((c >> 8) & 0xff), (int)(c & 0xff));
        else if (c >= MM_COL_BLACK && c <= MM_COL_WHITE)
            snprintf(tmp, sizeof(tmp), "%d", (int)c + 30 + offset);
        else if (c > MM_COL_WHITE && c < 16)
            snprintf(tmp, sizeof(tmp), "%d", (int)c + 90 + offset - 8);
        else if (c >= 16 && c < 256)
            snprintf(tmp, sizeof(tmp), "%d;5;%d", 38 + offset, (int)c);
        else
            continue;
        append_codes(codes, sizeof(codes), &clen, tmp);
    }
    if (ul != MM_COL_DEFAULT) {
        if (color_is_24(ul))
            snprintf(tmp, sizeof(tmp), "58;2;%d;%d;%d",
                     (int)((ul >> 16) & 0xff), (int)((ul >> 8) & 0xff), (int)(ul & 0xff));
        else if (ul >= 0 && ul < 256)
            snprintf(tmp, sizeof(tmp), "58;5;%d", (int)ul);
        else
            tmp[0] = 0;
        if (tmp[0]) append_codes(codes, sizeof(codes), &clen, tmp);
    }
    if (clen == 0) { snprintf(out, cap, "\x1b[0m"); return false; }
    snprintf(out, cap, "\x1b[;%sm", codes);
    return true;
}

static void cleanse(mm_renderer_t *r, const char *text) {
    /* Strip ESC from text before printing, as fzf does. */
    if (!text) return;
    const char *p = text;
    size_t start = 0, i = 0;
    while (p[i]) {
        if (p[i] == '\x1b') {
            if (i > start) queued_append(r, p + start, i - start);
            start = i + 1;
        }
        i++;
    }
    if (i > start) queued_append(r, p + start, i - start);
}

/* ---- border shapes (fzf src/tui/tui.go) ---- */
static const mm_border_glyphs_t border_rounded = {
    "\u2500", "\u2500", "\u2502", "\u2502",
    "\u256d", "\u256e", "\u2570", "\u256f"
};
static const mm_border_glyphs_t border_sharp = {
    "\u2500", "\u2500", "\u2502", "\u2502",
    "\u250c", "\u2510", "\u2514", "\u2518"
};
static const mm_border_glyphs_t border_bold = {
    "\u2501", "\u2501", "\u2503", "\u2503",
    "\u250f", "\u2513", "\u2517", "\u251b"
};
static const mm_border_glyphs_t border_block = {
    "\u2580", "\u2584", "\u258c", "\u2590",
    "\u259b", "\u259c", "\u2599", "\u259f"
};
static const mm_border_glyphs_t border_thinblock = {
    "\u2594", "\u2581", "\u258f", "\u2595",
    "\xf0\x9f\xad\xbd", "\xf0\x9f\xad\xbe",
    "\xf0\x9f\xad\xbc", "\xf0\x9f\xad\xbf"
};
static const mm_border_glyphs_t border_double = {
    "\u2550", "\u2550", "\u2551", "\u2551",
    "\u2554", "\u2557", "\u255a", "\u255d"
};
static const mm_border_glyphs_t border_dashed = {
    "\u2576", "\u2576", "\u2506", "\u2506",
    "\u256d", "\u256e", "\u2570", "\u256f"
};
static const mm_border_glyphs_t border_horizontal = {
    "\u2500", "\u2500", " ", " ",
    "\u250c", "\u2510", "\u2514", "\u2518"
};
static const mm_border_glyphs_t border_vertical = {
    " ", " ", "\u2502", "\u2502",
    "\u250c", "\u2510", "\u2514", "\u2518"
};

/* Which edges a shape draws. */
void mm_border_sides(int shape, bool *top, bool *bottom, bool *left, bool *right) {
    *top = *bottom = *left = *right = true;
    switch (shape) {
    case MM_BORDER_NONE:       *top = *bottom = *left = *right = false; break;
    case MM_BORDER_HORIZONTAL: *left = *right = false; break;
    case MM_BORDER_VERTICAL:   *top = *bottom = false; break;
    case MM_BORDER_TOP:        *bottom = *left = *right = false; break;
    case MM_BORDER_BOTTOM:     *top = *left = *right = false; break;
    case MM_BORDER_LEFT:       *top = *bottom = *right = false; break;
    case MM_BORDER_RIGHT:      *top = *bottom = *left = false; break;
    default: break;
    }
}

/* The ASCII border style (--no-unicode). */
static const mm_border_glyphs_t border_ascii = {
    "-", "-", "|", "|", "+", "+", "+", "+"
};

const mm_border_glyphs_t *mm_border_glyphs_ascii(int shape) {
    if (shape == MM_BORDER_NONE) return NULL;
    return &border_ascii;
}

const mm_border_glyphs_t *mm_border_glyphs(int shape) {
    switch (shape) {
    case MM_BORDER_ROUNDED:    return &border_rounded;
    case MM_BORDER_SHARP:      return &border_sharp;
    case MM_BORDER_BOLD:       return &border_bold;
    case MM_BORDER_BLOCK:      return &border_block;
    case MM_BORDER_THINBLOCK:  return &border_thinblock;
    case MM_BORDER_DOUBLE:     return &border_double;
    case MM_BORDER_DASHED:     return &border_dashed;
    case MM_BORDER_HORIZONTAL: return &border_horizontal;
    case MM_BORDER_VERTICAL:   return &border_vertical;
    case MM_BORDER_TOP:
    case MM_BORDER_BOTTOM:
    case MM_BORDER_LEFT:
    case MM_BORDER_RIGHT:      return &border_sharp;
    default:                   return NULL;
    }
}

int mm_border_parse(const char *spec) {
    if (!spec || !*spec) return MM_BORDER_ROUNDED;
    if (!strcmp(spec, "rounded"))    return MM_BORDER_ROUNDED;
    if (!strcmp(spec, "sharp"))      return MM_BORDER_SHARP;
    if (!strcmp(spec, "bold"))       return MM_BORDER_BOLD;
    if (!strcmp(spec, "block"))      return MM_BORDER_BLOCK;
    if (!strcmp(spec, "thinblock"))  return MM_BORDER_THINBLOCK;
    if (!strcmp(spec, "double"))     return MM_BORDER_DOUBLE;
    if (!strcmp(spec, "dashed"))     return MM_BORDER_DASHED;
    if (!strcmp(spec, "horizontal")) return MM_BORDER_HORIZONTAL;
    if (!strcmp(spec, "vertical"))   return MM_BORDER_VERTICAL;
    if (!strcmp(spec, "top"))        return MM_BORDER_TOP;
    if (!strcmp(spec, "bottom"))     return MM_BORDER_BOTTOM;
    if (!strcmp(spec, "left"))       return MM_BORDER_LEFT;
    if (!strcmp(spec, "right"))      return MM_BORDER_RIGHT;
    if (!strcmp(spec, "line"))       return MM_BORDER_TOP;
    if (!strcmp(spec, "inline"))     return MM_BORDER_NONE;
    if (!strcmp(spec, "none"))       return MM_BORDER_NONE;
    return -1;
}

/* ---- window ---- */
void mm_window_init(mm_window_t *w, mm_renderer_t *r, int top, int left,
                    int width, int height, int window_type, int border,
                    bool erase) {
    memset(w, 0, sizeof(*w));
    w->r = r;
    w->colored = r->colored;
    w->window_type = window_type;
    w->border = border;
    w->top = top;
    w->left = left;
    w->width = width < 0 ? 0 : width;
    w->height = height < 0 ? 0 : height;
    w->tabstop = r->tabstop;
    w->fg = MM_COL_DEFAULT;
    w->bg = MM_COL_DEFAULT;
    (void)erase;
}

void mm_window_move(mm_window_t *w, int y, int x) {
    w->posx = x;
    w->posy = y;
    mm_renderer_move(w->r, w->top + y, w->left + x);
}

void mm_window_print(mm_window_t *w, const char *text) {
    mm_window_cprint(w, MM_COL_DEFAULT, w->bg, MM_ATTR_REGULAR, text);
}

void mm_window_cprint(mm_window_t *w, mm_color_t fg, mm_color_t bg,
                      mm_attr_t attr, const char *text) {
    char code[256];
    csi_color(code, sizeof(code), fg, bg, MM_COL_DEFAULT, attr);
    mm_renderer_set_sgr(w->r, code);
    cleanse(w->r, text);
}

void mm_window_move_and_clear(mm_window_t *w, int y, int x) {
    mm_window_move(w, y, x);
    char *buf = malloc((size_t)(w->width - x) * 2 + 2);
    int n = w->width - x;
    if (n < 0) n = 0;
    if (buf) {
        for (int i = 0; i < n; i++) buf[i] = ' ';
        buf[n] = 0;
        mm_window_print(w, buf);
        free(buf);
    }
    mm_window_move(w, y, x);
}

void mm_window_erase(mm_window_t *w) {
    mm_renderer_t *r = w->r;
    for (int y = 0; y < w->height; y++) {
        mm_window_move(w, y, 0);
        for (int x = 0; x < w->width; x++) queued_append(r, " ", 1);
    }
    mm_renderer_set_sgr(r, "\x1b[0m");
}

void mm_window_fill(mm_window_t *w, const char *text) {
    /* Simple non-wrapping fill: split on '\n', respect width. Full WrapLine
       port follows. */
    mm_renderer_t *r = w->r;
    const char *p = text;
    while (*p) {
        const char *nl = strchr(p, '\n');
        size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
        if (w->posx < w->width) {
            mm_renderer_t tmp; /* print via cleanse into queued */
            (void)tmp;
            cleanse(r, "");
            /* print segment */
            char *seg = malloc(seglen + 1);
            memcpy(seg, p, seglen);
            seg[seglen] = 0;
            cleanse(r, seg);
            w->posx += mm_utf8_count(seg);
            free(seg);
        }
        if (nl) {
            if (w->posy + 1 >= w->height) return;
            mm_window_move(w, w->posy + 1, 0);
            p = nl + 1;
        } else break;
    }
}

void mm_window_cfill(mm_window_t *w, mm_color_t fg, mm_color_t bg,
                     mm_color_t ul, mm_attr_t attr, const char *text) {
    char code[256];
    csi_color(code, sizeof(code), fg, bg, ul, attr);
    mm_renderer_set_sgr(w->r, code);
    mm_window_fill(w, text);
}

void mm_window_finish_fill(mm_window_t *w) {
    mm_renderer_set_sgr(w->r, "\x1b[0m");
}
