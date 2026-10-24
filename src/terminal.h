/* terminal.h - raw-termios terminal renderer and input decoder.
   Port of the mechanics of fzf's src/tui/light.go (the default "light"
   renderer) plus input decoding from src/terminal.go. No ncurses. */
#ifndef MM_TERMINAL_H
#define MM_TERMINAL_H

#include "tui.h"
#include <stdbool.h>
#include <stddef.h>

enum {
    MM_WIN_BASE,
    MM_WIN_LIST,
    MM_WIN_INPUT,
    MM_WIN_HEADER,
    MM_WIN_FOOTER,
    MM_WIN_PREVIEW,
};

enum {
    MM_BORDER_NONE,
    MM_BORDER_ROUNDED,
    MM_BORDER_SHARP,
    MM_BORDER_BLOCK,
    MM_BORDER_THINBLOCK,
    MM_BORDER_BOLD,
    MM_BORDER_DOUBLE,
    MM_BORDER_HORIZONTAL,
    MM_BORDER_VERTICAL,
    MM_BORDER_DASHED,
    MM_BORDER_TOP,
    MM_BORDER_BOTTOM,
    MM_BORDER_LEFT,
    MM_BORDER_RIGHT,
};

/* The glyphs that make up a border shape (fzf's tui.BorderStyle). */
typedef struct {
    const char *top, *bottom, *left, *right;
    const char *top_left, *top_right, *bottom_left, *bottom_right;
} mm_border_glyphs_t;

/* Parse a --border value. Returns the shape id, or -1 on an unknown shape. */
int mm_border_parse(const char *spec);

/* Glyphs for a shape. */
const mm_border_glyphs_t *mm_border_glyphs(int shape);

/* The ASCII border style used by --no-unicode. */
const mm_border_glyphs_t *mm_border_glyphs_ascii(int shape);

/* Which edges of the screen a shape draws. */
void mm_border_sides(int shape, bool *top, bool *bottom, bool *left, bool *right);

typedef struct mm_renderer_t {
    int   ttyin;              /* input fd */
    int   ttyout;             /* output fd */
    int   width, height;
    int   y, x;               /* current cursor (window-relative origin) */
    int   yoffset;
    int   tabstop;
    int   esc_delay;
    bool  mouse;
    bool  fullscreen;
    bool  inline_mode;   /* --height: draw inside the terminal, no alt screen */
    int   inline_height; /* the number of rows the inline UI may use */
    bool  clear_on_exit;
    bool  up_one_line;
    bool  show_cursor;
    bool  force_black;
    bool  colored;
    char *queued;             /* output buffer */
    size_t queued_len, queued_cap;
    char  sgr[64];            /* last emitted SGR sequence */
    char  buffer[4096];       /* pending input bytes */
    size_t buffer_len;
    bool  paste_was_set;
    bool  have_paste_was_set;
} mm_renderer_t;

typedef struct mm_window_t {
    mm_renderer_t *r;
    bool           colored;
    int            window_type;
    int            border;
    int            top, left, width, height;
    int            posx, posy;
    int            tabstop;
    mm_color_t     fg, bg;
    const char    *wrap_sign;
    int            wrap_sign_width;
} mm_window_t;

/* ---- renderer lifecycle ---- */
/* ttyin/ttyout are file descriptors (e.g. from open("/dev/tty")). Returns
   false on failure. */
bool mm_renderer_init(mm_renderer_t *r, int ttyin, int ttyout, bool mouse,
                      int tabstop, bool clear_on_exit, bool fullscreen);
void mm_renderer_close(mm_renderer_t *r);
void mm_renderer_update_size(mm_renderer_t *r);
int  mm_renderer_max_x(mm_renderer_t *r);
int  mm_renderer_max_y(mm_renderer_t *r);
int  mm_renderer_top(mm_renderer_t *r);
void mm_renderer_flush(mm_renderer_t *r);
void mm_renderer_clear(mm_renderer_t *r);
void mm_renderer_bell(mm_renderer_t *r);
void mm_renderer_hide_cursor(mm_renderer_t *r);
void mm_renderer_show_cursor(mm_renderer_t *r);

/* Low-level output helpers. */
void mm_renderer_move(mm_renderer_t *r, int y, int x);
void mm_renderer_origin(mm_renderer_t *r);
void mm_renderer_csi(mm_renderer_t *r, const char *code);
void mm_renderer_set_sgr(mm_renderer_t *r, const char *code);
void mm_renderer_stderr(mm_renderer_t *r, const char *str);

/* ---- window ---- */
void mm_window_init(mm_window_t *w, mm_renderer_t *r, int top, int left,
                    int width, int height, int window_type, int border,
                    bool erase);
void mm_window_print(mm_window_t *w, const char *text);
void mm_window_cprint(mm_window_t *w, mm_color_t fg, mm_color_t bg,
                      mm_attr_t attr, const char *text);
void mm_window_move(mm_window_t *w, int y, int x);
void mm_window_move_and_clear(mm_window_t *w, int y, int x);
void mm_window_erase(mm_window_t *w);
void mm_window_fill(mm_window_t *w, const char *text);
void mm_window_cfill(mm_window_t *w, mm_color_t fg, mm_color_t bg,
                     mm_color_t ul, mm_attr_t attr, const char *text);
void mm_window_finish_fill(mm_window_t *w);

#endif /* MM_TERMINAL_H */
