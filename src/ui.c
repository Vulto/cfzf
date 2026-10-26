/* ui.c - interactive fuzzy finder loop.
   Renders a prompt plus a scrollable list of matches and handles the
   core key bindings (movement, editing, selection, multi-select). */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "terminal.h"
#include "event.h"
#include "util.h"

#include <errno.h>
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <locale.h>
#include <limits.h>
#include <stdint.h>

#define MM_MIN_HEIGHT 1

/* Forward declarations. */
static mm_color_t ui_theme_color(const mm_ui_t *ui, mm_color_attr_t entry,
                                 mm_color_t fallback);
static void ui_draw_preview(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *win,
                            int x, int y, int w, int h);

/* --- query buffer helpers --- */
static void query_set(mm_ui_t *ui, const char *s) {
    size_t n = strlen(s);
    if (n > INT_MAX) return;
    if (n + 1 > ui->query_cap) {
        size_t nc = ui->query_cap ? ui->query_cap : 64;
        while (n + 1 > nc) {
            if (nc > SIZE_MAX / 2) {
                nc = n + 1;
                break;
            }
            nc *= 2;
        }
        char *buf = realloc(ui->query_buf, nc);
        if (!buf) return;
        ui->query_buf = buf;
        ui->query_cap = nc;
    }
    memcpy(ui->query_buf, s, n + 1);
    ui->query_len = n;
    ui->query_pos = (int)n;
}

static void query_insert(mm_ui_t *ui, const char *s, size_t n) {
    if (n > SIZE_MAX - ui->query_len - 1 ||
        ui->query_len + n > INT_MAX)
        return;
    size_t required = ui->query_len + n + 1;
    if (required > ui->query_cap) {
        size_t nc = ui->query_cap ? ui->query_cap : 64;
        while (required > nc) {
            if (nc > SIZE_MAX / 2) {
                nc = required;
                break;
            }
            nc *= 2;
        }
        char *buf = realloc(ui->query_buf, nc);
        if (!buf) return;
        ui->query_buf = buf;
        ui->query_cap = nc;
    }
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    memmove(ui->query_buf + pos + n, ui->query_buf + pos,
            ui->query_len - pos + 1);
    memcpy(ui->query_buf + pos, s, n);
    ui->query_len += n;
    ui->query_pos = (int)(pos + n);
    ui->query_buf[ui->query_len] = 0;
}

static void query_backspace(mm_ui_t *ui) {
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    if (pos == 0) return;
    size_t start = pos - 1;
    while (start > 0 && ((unsigned char)ui->query_buf[start] & 0xC0) == 0x80)
        start--;
    memmove(ui->query_buf + start, ui->query_buf + pos, ui->query_len - pos + 1);
    ui->query_len -= pos - start;
    ui->query_pos = (int)start;
}

static void query_delete(mm_ui_t *ui) {
    size_t pos = (size_t)ui->query_pos;
    if (pos >= ui->query_len) return;
    size_t end = pos + 1;
    while (end < ui->query_len &&
           ((unsigned char)ui->query_buf[end] & 0xC0) == 0x80)
        end++;
    memmove(ui->query_buf + pos, ui->query_buf + end, ui->query_len - end + 1);
    ui->query_len -= end - pos;
}

static void query_kill_before_cursor(mm_ui_t *ui) {
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    if (pos == 0) return;
    memmove(ui->query_buf, ui->query_buf + pos, ui->query_len - pos + 1);
    ui->query_len -= pos;
    ui->query_pos = 0;
}

static void query_kill_after_cursor(mm_ui_t *ui) {
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    ui->query_len = pos;
    ui->query_buf[pos] = '\0';
}

/* --- searching --- */
static void ui_search(mm_ui_t *ui);
static bool ui_execute_action(mm_ui_t *ui, const mm_binding_t *b);
static void ui_exec_event(mm_ui_t *ui, const char *name);

static void ui_search(mm_ui_t *ui) {
    const char *prev_query = ui->query_buf;
    int prev_nresults = ui->nresults;

    if (ui->phony) {
        /* The query does not filter the items: every item is a result, in
           the order of the input. */
        int n = ui->count;
        int *res = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
        if (res) {
            for (int i = 0; i < n; i++) res[i] = i;
            free(ui->results);
            ui->results = res;
            ui->nresults = n;
        } else {
            ui->nresults = 0;
        }
        mm_match_pos_free(ui->match_pos, ui->match_pos_n);
        ui->match_pos = NULL;
        ui->match_pos_n = 0;
        if (ui->cursor >= ui->nresults) ui->cursor = ui->nresults ? ui->nresults - 1 : 0;
        if (ui->offset > ui->cursor) ui->offset = ui->cursor;
        /* Trigger events. */
        if (strcmp(prev_query, ui->query_buf) != 0)
            ui_exec_event(ui, "change");
        if (prev_nresults != ui->nresults)
            ui_exec_event(ui, "result");
        return;
    }
    bool forward = mm_criteria_forward(&ui->crit);
    mm_pattern_t *pat = mm_pattern_build(ui->query_buf, ui->fuzzy, ui->extended,
                                         ui->case_mode, !ui->literal, forward, true);
    if (!pat) { ui->nresults = 0; /* Trigger events. */
        if (strcmp(prev_query, ui->query_buf) != 0) ui_exec_event(ui, "change");
        if (prev_nresults != ui->nresults) ui_exec_event(ui, "result");
        return; }
    if (ui->no_sort) mm_pattern_set_sortable(pat, false);

    /* Incremental search in streaming mode not fully implemented yet */

    /* The items are decoded once, when the UI starts, so every search can
       reuse them instead of decoding the whole list again. */
    mm_input_t input = { ui->lines, ui->count, ui->items, 0, ui->delim, ui->nth,
                         ui->nth_n, ui->length_from_transform };
    int n = 0;
    mm_match_pos_t *mp = NULL;
    int *res = mm_batch_search(&input, pat, &ui->crit, ui->tac, &n, &mp);
    free(ui->results);
    ui->results = res;
    ui->nresults = n;
    mm_match_pos_free(ui->match_pos, ui->match_pos_n);
    ui->match_pos = mp;
    ui->match_pos_n = n;
    if (ui->input_fd >= 0) {
        ui->search_count = ui->count;
        if (ui->search_results) free(ui->search_results);
        ui->search_results = res ? malloc((size_t)(n > 0 ? n : 1) * sizeof(int)) : NULL;
        if (ui->search_results && res) memcpy(ui->search_results, res, (size_t)n * sizeof(int));
    }
    if (ui->cursor >= n) ui->cursor = n ? n - 1 : 0;
    if (ui->offset > ui->cursor) ui->offset = ui->cursor;
    mm_pattern_free(pat);

    /* Trigger events. */
    if (strcmp(prev_query, ui->query_buf) != 0)
        ui_exec_event(ui, "change");
    if (prev_nresults != ui->nresults)
        ui_exec_event(ui, "result");
}

/* Execute the actions bound to an event. */
static void ui_exec_event(mm_ui_t *ui, const char *name) {
    const mm_event_binding_t *found[16];
    int nf = mm_event_lookup(&ui->events, name, found, 16);
    for (int i = 0; i < nf; i++)
        ui_execute_action(ui, &(const mm_binding_t){
            .type = MM_EV_INVALID, .rune = 0, .action = found[i]->action,
            .arg = found[i]->arg });
}

/* --- rendering --- */
/* fzf 0.74 default (fullscreen) layout, from bottom to top:
     - prompt on the last line
     - info line ("  N/M ----") above it
     - the match list above that, drawn bottom-up: the first match sits on
       the line directly above the info line.
   Every list row carries a pointer glyph; the current row's pointer is
   highlighted. */
/* Default (dark) theme values, matching fzf's Dark256 theme. They are
   overridden by --color. */
#define MM_THEME_DARK_BG   236
#define MM_THEME_PROMPT    110
#define MM_THEME_CURRENT   254
#define MM_THEME_INFO      144
#define MM_THEME_POINTER   161
#define MM_THEME_BORDER    59
#define MM_THEME_CURRENT_MATCH 151
#define MM_THEME_MATCH      108
#define MM_THEME_MARKER     168
#define MM_THEME_SPINNER    148
#define MM_THEME_HEADER    109

/* Display width of a UTF-8 string in terminal cells. */
static int ui_str_width(const char *s) {
    int w = 0;
    size_t len = strlen(s);
    size_t i = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(s + i, len - i, &cp);
        if (used == 0) used = 1;
        w += mm_runewidth(cp);
        i += used;
    }
    return w;
}

static int ui_query_cursor_width(const mm_ui_t *ui) {
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    int width = 0;
    size_t i = 0;
    while (i < pos) {
        wchar_t cp;
        size_t used = mm_utf8_decode(ui->query_buf + i, pos - i, &cp);
        if (used == 0) used = 1;
        width += mm_runewidth(cp);
        i += used;
    }
    return width;
}

static void ui_draw_border_box(mm_window_t *win,
                                const mm_border_glyphs_t *g,
                                mm_color_t color, int left, int right,
                                int top, int bottom, int total) {
    if (!g || left < 0 || right <= left || top < 0 || bottom >= total ||
        bottom <= top) return;
    mm_window_move(win, top, left);
    mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top_left);
    for (int x = left + 1; x < right; x++)
        mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top);
    mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top_right);

    mm_window_move(win, bottom, left);
    mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom_left);
    for (int x = left + 1; x < right; x++)
        mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom);
    mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom_right);

    for (int y = top + 1; y < bottom; y++) {
        mm_window_move(win, y, left);
        mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->left);
        mm_window_move(win, y, right);
        mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->right);
    }
}

/* Blank out the rest of the current line. */
/* Fill the row with spaces up to the given column.  `used` is the number
   of columns that are already filled from the start of the row. */
static void ui_clear_to_eol(mm_window_t *w, int width, int used, mm_color_t bg) {
    if (used >= width) return;
    int n = width - used;
    char *buf = malloc((size_t)n + 1);
    if (!buf) return;
    memset(buf, ' ', (size_t)n);
    buf[n] = 0;
    mm_window_cprint(w, MM_COL_DEFAULT, bg, 0, buf);
    free(buf);
}

/* fzf's default ellipsis (src/terminal.go: t.ellipsis = "··"). */
#define MM_ELLIPSIS "\u00b7\u00b7"
#define MM_ELLIPSIS_WIDTH 2

/* Trim `text` to `limit` display columns, appending the ellipsis when it
   does not fit. Returns a malloc'd string; *out_width receives the display
   width of the result. */
static char *ui_fit_text(const char *text, int limit, int *out_width,
                        const char *ellipsis) {
    int width = ui_str_width(text);
    if (limit < 1) limit = 1;
    if (width <= limit) {
        *out_width = width;
        return strdup(text);
    }
    if (!ellipsis || !*ellipsis) ellipsis = MM_ELLIPSIS;
    int ellipsis_width = ui_str_width(ellipsis);
    int keep = limit - ellipsis_width;
    if (keep < 0) keep = 0;
    /* Walk the string, keeping whole code points that fit. */
    size_t len = strlen(text);
    size_t i = 0;
    int acc = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        int cw = mm_runewidth(cp);
        if (acc + cw > keep) break;
        acc += cw;
        i += used;
    }
    char *out = malloc(i + strlen(ellipsis) + 1);
    if (!out) { *out_width = width; return strdup(text); }
    memcpy(out, text, i);
    strcpy(out + i, ellipsis);
    *out_width = acc + ellipsis_width;
    return out;
}

/* fzf's scrollbar geometry (src/terminal.go getScrollbar). */
static void ui_scrollbar_range(int total, int height, int offset,
                               int *bar_len, int *bar_start) {
    if (total <= 0 || total <= height) { *bar_len = 0; *bar_start = 0; return; }
    int len = height * height / total;
    if (len < 1) len = 1;
    int start;
    if (total == height) {
        start = 0;
    } else {
        start = (height - len) * offset / (total - height);
        if (start > height - len) start = height - len;
    }
    *bar_len = len;
    *bar_start = start;
}

/* Resolve a theme color, falling back to the built-in default when the
   theme entry was not overridden. */
static mm_color_t ui_color(mm_color_attr_t entry, mm_color_t fallback) {
    if (entry.color == MM_COL_UNDEFINED || entry.color == MM_COL_DEFAULT)
        return fallback;
    return entry.color;
}

/* The colour of a theme entry, honouring --no-color. */

/* Print an item, highlighting the matched characters. */
static void ui_print_item(mm_window_t *win, mm_ui_t *ui, const char *text,
                          const mm_match_pos_t *matched,
                          mm_color_t fg, mm_color_t bg, mm_attr_t attr,
                          bool cur);


/* The display width of the text between the two byte offsets. */
static int ui_width_range(const char *text, size_t from, size_t to) {
    int w = 0;
    size_t i = from;
    while (i < to) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, to - i, &cp);
        if (used == 0) used = 1;
        w += mm_runewidth(cp);
        i += used;
    }
    return w;
}

/* Split `text` into the wrapped lines of the list.  The first line holds
   `first` display columns, the continuations `cont` columns each.  With
   `word` the lines break at the last space that still fits.  The pieces are
   returned as malloc'd strings and `*off` holds the rune offset of every
   piece, so that the matched offsets can be adjusted. */
static int ui_wrap_text(const char *text, int first, int cont, bool word,
                         char ***out_pieces, int **out_off) {
    size_t len = strlen(text);
    size_t i = 0;            /* the current byte offset */
    int rune = 0;            /* the current rune offset */
    int np = 0;
    int cap = 0;
    char **pieces = NULL;
    int *off = NULL;
    int limit = first;
    size_t start = 0;        /* the byte offset of the current piece */
    int start_rune = 0;
    int acc = 0;             /* the display width of the current piece */

    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        int cw = mm_runewidth(cp);
        if (acc + cw > limit && acc > 0) {
            /* The line is full.  In the word mode it breaks at the last
               space that still fits. */
            size_t brk = i;      /* the byte offset of the break */
            size_t nstart = i;   /* the start of the next piece */
            int nstart_rune = rune;
            int nacc = 0;
            if (word) {
                size_t sp = 0;
                int sr = 0;
                bool found = false;
                size_t j = start;
                int jr = start_rune;
                while (j < i) {
                    wchar_t c2;
                    size_t u2 = mm_utf8_decode(text + j, i - j, &c2);
                    if (u2 == 0) u2 = 1;
                    if (c2 == ' ') { sp = j; sr = jr; found = true; }
                    j += u2;
                    jr++;
                }
                if (found && sp > start) {
                    /* The space belongs to the line that is finished. */
                    brk = sp + 1;
                    nstart = sp + 1;
                    nstart_rune = sr + 1;
                    nacc = ui_width_range(text, nstart, i);
                }
            }
            if (np + 1 > cap) {
                int ncap = cap ? cap * 2 : 4;
                char **np_ = realloc(pieces, sizeof(char *) * (size_t)ncap);
                if (!np_) { free(pieces); free(off); return 0; }
                pieces = np_;
                int *no_ = realloc(off, sizeof(int) * (size_t)ncap);
                if (!no_) { free(pieces); free(off); return 0; }
                off = no_;
                cap = ncap;
            }
            char *piece = malloc(brk - start + 1);
            if (!piece) break;
            memcpy(piece, text + start, brk - start);
            piece[brk - start] = 0;
            pieces[np] = piece;
            off[np] = start_rune;
            np++;
            if (brk != i) {
                /* The word mode: the rest of the line starts after the
                   space, the rune at `i` is not consumed yet. */
                start = nstart;
                start_rune = nstart_rune;
                acc = nacc;
                limit = cont;
                continue;
            }
            start = i;
            start_rune = rune;
            acc = 0;
            limit = cont;
        }
        acc += cw;
        rune++;
        i += used;
    }
    if (np + 1 > cap) {
        int ncap = cap ? cap * 2 : 4;
        char **np_ = realloc(pieces, sizeof(char *) * (size_t)ncap);
        if (!np_) { free(pieces); free(off); return 0; }
        pieces = np_;
        int *no_ = realloc(off, sizeof(int) * (size_t)ncap);
        if (!no_) { free(pieces); free(off); return 0; }
        off = no_;
        cap = ncap;
    }
    char *piece = malloc(i - start + 1);
    if (!piece) { for (int k = 0; k < np; k++) free(pieces[k]); free(pieces); free(off); return 0; }
    memcpy(piece, text + start, i - start);
    piece[i - start] = 0;
    pieces[np] = piece;
    off[np] = start_rune;
    np++;
    *out_pieces = pieces;
    *out_off = off;
    return np;
}

/* Number of display rows occupied by a (possibly multiline) text block. */
static int ui_text_rows(const char *text) {
    if (!text || !*text) return 0;
    int rows = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') rows++;
    return rows;
}

static void ui_render(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *listwin,
                      mm_window_t *promptwin, mm_window_t *infowin) {
    int total = mm_renderer_max_y(r);
    int width = mm_renderer_max_x(r);

    /* A border box takes the first and the last line, and one column on
       each side; the content is then laid out inside it. */
    bool b_top, b_bottom, b_left, b_right;
    mm_border_sides(ui->border, &b_top, &b_bottom, &b_left, &b_right);

    /* A preview window takes one side of the screen; the list gets the rest.
       The preview is placed inside the border, so its share is computed from
       the area between the border edges. */
    int pv_x = 0, pv_y = 0, pv_w = 0, pv_h = 0;
    bool has_preview = ui->preview != NULL;
    /* A left border costs two columns (the border and the gap after it). */
    bool full_component_style = ui->style_full && !ui->header_first;
    bool draw_list_box = full_component_style ||
                         (ui->list_border_set && ui->list_border != MM_BORDER_NONE);
    int list_border_inset = draw_list_box && !full_component_style ? 2 : 0;
    int content_x = b_left ? 2 : 0;        /* pointer column */
    if (has_preview && ui->preview_win.pos == MM_PREVIEW_LEFT) content_x = 1;
    /* The padding shifts the whole list to the right, the margin shifts it
       away from the edge of the screen. */
    content_x += ui->padding + ui->margin;
    int info_x    = content_x;             /* info line */
    /* The area between the border edges; the preview takes a share of it. */
    int inner_w = width - (b_left ? 1 : 0) - (b_right ? 1 : 0);
    int inner_h = total - (b_top ? 1 : 0) - (b_bottom ? 1 : 0);
    int pv_total = 0;
    if (has_preview) {
        int size = ui->preview_win.size;
        if (size < 1) size = 1;
        if (size > 100) size = 100;
        switch (ui->preview_win.pos) {
        case MM_PREVIEW_LEFT:
        case MM_PREVIEW_RIGHT: pv_total = inner_w * size / 100; break;
        default:               pv_total = inner_h * size / 100; break;
        }
        if (pv_total < 2) pv_total = 2;
    }

    /* The preview box is inset from the screen edge by the border, and the
       list gets whatever is left.  The area already excludes the border
       edges, so the content offsets do not have to account for them. */
    int area_x = 0, area_y = 0;
    int area_w = width - (b_right ? 1 : 0) - ui->margin;
    int area_h = total - (b_bottom ? 1 : 0) - ui->margin;
    /* With --height the UI is confined to the top rows of the terminal. */
    if (ui->height > 0 && ui->height < area_h) area_h = ui->height;
    /* The bottom border is already excluded from the area. */
    if (has_preview) {
        switch (ui->preview_win.pos) {
        case MM_PREVIEW_LEFT:
            pv_w = pv_total - (b_left ? 1 : 0);
            pv_x = area_x + (b_left ? 2 : 0);
            pv_y = area_y + (b_top ? 1 : 0);
            pv_h = area_h - (b_top ? 1 : 0) - (b_bottom ? 1 : 0);
            area_x = pv_x + pv_w;
            area_w = width - area_x - (b_right ? 1 : 0);
            break;
        case MM_PREVIEW_TOP:
            pv_h = pv_total - (b_top ? 1 : 0);
            pv_x = area_x + (b_left ? 1 : 0);
            pv_w = area_w - (b_left ? 1 : 0);
            pv_y = area_y + (b_top ? 1 : 0);
            area_y = pv_y + pv_h;
            area_h = total - area_y - (b_bottom ? 1 : 0);
            break;
        case MM_PREVIEW_BOTTOM:
            pv_h = pv_total - (b_bottom ? 1 : 0);
            pv_x = area_x + (b_left ? 1 : 0);
            pv_w = area_w - (b_left ? 1 : 0);
            pv_y = area_y + area_h - (b_bottom ? 1 : 0) - pv_h;
            area_h = pv_y - area_y;
            break;
        default:
            pv_w = pv_total - (b_right ? 1 : 0);
            pv_x = area_x + area_w - 1 - (b_right ? 1 : 0) - pv_w + 1;
            pv_y = area_y + (b_top ? 1 : 0);
            pv_h = area_h - (b_top ? 1 : 0);
            area_w = pv_x - area_x;
            break;
        }
    }
    /* Right edge of the content area: the scrollbar always takes the last
       column, a right border takes one more. */
    int right_edge = area_x + area_w - 2;
    /* Columns available for a list line, measured from content_x. */
    int list_w    = right_edge - (area_x + content_x) + 1;
    /* Columns available for the info line, measured from info_x.  The
       padding takes away a column at the right as well. */
    int info_w    = list_w - ui->padding;

    int header_rows  = ui_text_rows(ui->header);
    int hlines_rows  = ui_text_rows(ui->header_lines);
    bool has_header  = header_rows > 0;
    bool has_hlines  = hlines_rows > 0;
    /* --header-first moves the "first" block to the opposite edge.  When
       there is no explicit --header, the header lines take that role. */
    bool first_is_explicit = ui->header_first && has_header;
    bool first_is_hlines   = ui->header_first && !has_header && has_hlines;
    bool has_first_block   = first_is_explicit || first_is_hlines;
    int  first_rows        = first_is_explicit ? header_rows
                            : (first_is_hlines ? hlines_rows : 0);
    bool rest_hlines       = has_hlines && !first_is_hlines;
    int  rest_rows         = rest_hlines ? hlines_rows : 0;

    /* The rows are relative to the list area, which is what is left of the
       screen after the preview window. */
    /* The padding takes away a row of the list area. */
    int content_h = area_h - (b_top ? 1 : 0) - ui->padding;
    if (content_h < 1) content_h = 1;

    int content_top = area_y + (b_top ? 1 : 0);
    int content_bottom = content_top + content_h - 1;
    int prompt_row, info_row, header_row, header_lines_row;
    int block_row, list_first, list_last;
    /* The footer is drawn at the top of the list area, followed by a
       horizontal line.  With --reverse it sits at the bottom instead. */
    bool has_footer = ui->footer && *ui->footer;
    header_row = header_lines_row = -1;
    bool draw_header_box = has_header && !ui->header_first &&
                           (full_component_style ||
                            (ui->header_border_set && ui->header_border != MM_BORDER_NONE));
    bool draw_input_box = full_component_style ||
                          (ui->input_border_set && ui->input_border != MM_BORDER_NONE);
    bool draw_component_boxes = full_component_style || draw_list_box ||
                                draw_header_box || draw_input_box;
    if (ui->reverse) {
        /* prompt on top, info below it, list grows downwards. */
        prompt_row = content_top;
        if (has_first_block) {
            if (first_is_explicit) header_row = content_top;
            else                   header_lines_row = content_top;
            prompt_row = content_top + first_rows;
        }
        block_row = prompt_row + (ui->info && ui->info_inline_right ? 0 : 1);
        info_row  = ui->info ? block_row : -1;
        int row = block_row + 1;
        if (!ui->header_first && has_header) {
            header_row = row;
            row += header_rows;
        }
        if (rest_hlines) {
            header_lines_row = row;
            row += rest_rows;
        }
        list_first = row;
        list_last  = content_bottom;
        if (has_footer) list_last -= 2;
    } else {
        /* The list grows upwards from the info line at the bottom. */
        prompt_row = content_bottom;
        if (has_first_block) {
            if (first_is_explicit) header_row = content_bottom - first_rows + 1;
            else                   header_lines_row = content_bottom;
            prompt_row = content_bottom - first_rows;
        }
        block_row = prompt_row - (ui->info && ui->info_inline_right ? 0 : 1);
        info_row  = ui->info ? block_row : -1;
        int row = block_row - 1;
        if (!ui->header_first && has_header) {
            /* The explicit header sits directly above the info line. */
            header_row = row - header_rows + 1;
            row -= header_rows;
        }
        if (rest_hlines) {
            /* Header lines are stacked above the explicit header, with the
               first input line closest to it. */
            header_lines_row = row;
            row -= rest_rows;
        }
        list_last  = row;
        list_first = content_top + (has_footer ? 2 : 0);
    }
    if (full_component_style) {
        /* Reserve input, list, and optional header borders; adjacent boxes
           share their divider rows. */
        if (ui->reverse) {
            prompt_row++;
            if (info_row >= 0) info_row++;
            if (draw_header_box) {
                header_row += 3;
                list_first += 4;
            } else {
                list_first += 2;
            }
            list_last--;
        } else {
            if (draw_header_box) {
                header_row -= 2;
                list_last -= 3;
            } else {
                list_last--;
            }
            list_first++;
        }
    } else if (draw_input_box || draw_list_box || draw_header_box) {
        if (ui->reverse) {
            if (draw_input_box) {
                prompt_row++;
                if (info_row >= 0) info_row++;
                list_first += 2;
                list_last--;
            }
            if (draw_header_box) {
                header_row++;
                list_first += 2;
            }
            if (draw_list_box) {
                list_first += 2;
                list_last--;
            }
        } else {
            if (draw_input_box) list_last--;
            if (draw_header_box) list_last--;
            if (draw_list_box) {
                list_first++;
                list_last--;
            }
        }
    }

    int list_rows = list_last - list_first + 1;
    if (list_rows < 1) list_rows = 1;

    /* Keep the cursor inside the visible window and apply the scroll
       offset (fzf's constrain(), src/terminal.go). */
    if (ui->cursor < 0) ui->cursor = 0;
    if (ui->nresults > 0 && ui->cursor >= ui->nresults) ui->cursor = ui->nresults - 1;
    {
        int count = ui->nresults;
        int min_offset = ui->cursor - list_rows + 1;
        if (min_offset < 0) min_offset = 0;
        int max_offset = count - list_rows;
        if (max_offset > ui->cursor) max_offset = ui->cursor;
        if (max_offset < 0) max_offset = 0;
        if (ui->offset < min_offset) ui->offset = min_offset;
        if (ui->offset > max_offset) ui->offset = max_offset;
        if (ui->offset < 0) ui->offset = 0;

        /* The scroll offset keeps the cursor away from the edges of the
           list, so that the context stays visible. */
        if (ui->scroll_off > 0) {
            int scroll_off = list_rows / 2;
            if (scroll_off > ui->scroll_off) scroll_off = ui->scroll_off;
            int new_offset = ui->offset;
            for (int phase = 0; phase < 2; phase++) {
                for (;;) {
                    int prev = new_offset;
                    int item_lines = 1 + ui->gap;
                    int lines_before = ui->cursor - new_offset;
                    int lines_after = list_rows - (lines_before + item_lines);
                    if (lines_before < scroll_off && lines_after < scroll_off) break;
                    if (phase == 0 && lines_before < scroll_off) {
                        new_offset--;
                        if (new_offset < min_offset) new_offset = min_offset;
                    } else if (phase == 1 && lines_after < scroll_off) {
                        new_offset++;
                        if (new_offset > max_offset) new_offset = max_offset;
                    }
                    if (new_offset == prev) break;
                }
                ui->offset = new_offset;
            }
        }
    }

    /* Footer: at the top of the list area in the default layout, and at the
       bottom with --reverse.  A horizontal line separates it from the list. */
    if (has_footer) {
        int frow, lrow;
        if (ui->reverse) {
            lrow = content_bottom - 1;
            frow = content_bottom;
        } else {
            frow = list_first - 2;
            lrow = frow + 1;
        }
        mm_window_move(listwin, frow, area_x + content_x + 2);
        mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.footer, MM_THEME_HEADER),
                         MM_COL_DEFAULT, 0, ui->footer);
        ui_clear_to_eol(listwin, area_x + area_w - 1,
                        area_x + content_x + 2 + ui_str_width(ui->footer), MM_COL_DEFAULT);
        /* The line next to the footer. */
        mm_window_move(listwin, lrow, area_x);
        {
            int fill = area_w;
            if (fill < 1) fill = 1;
            char *line = malloc((size_t)fill * 4 + 8);
            if (line) {
                size_t o = 0;
                const char *dash = ui->no_unicode ? "-" : "\u2500";
                size_t dl = strlen(dash);
                for (int i = 0; i < fill; i++) {
                    memcpy(line + o, dash, dl);
                    o += dl;
                }
                line[o] = 0;
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                 MM_COL_DEFAULT, 0, line);
                free(line);
            }
        }
    }

    /* Draw a (possibly multiline) header block.  In the default layout the
       explicit header is printed top-down, while the --header-lines block
       is stacked upwards (its first line is closest to the info line). */
    if (has_header) {
        const char *p = ui->header;
        int row = header_row;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            char *seg = malloc(seglen + 1);
            if (seg) {
                memcpy(seg, p, seglen);
                seg[seglen] = 0;
                mm_window_move(listwin, row, area_x + content_x + 2);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.header, MM_THEME_HEADER),
                                 MM_COL_DEFAULT, 0, seg);
                ui_clear_to_eol(listwin, list_w - 2, ui_str_width(seg), MM_COL_DEFAULT);
                free(seg);
            }
            row++;
            if (!nl) break;
            p = nl + 1;
        }
    }
    if (has_hlines) {
        const char *p = ui->header_lines;
        int row = header_lines_row;
        int step = ui->reverse ? 1 : -1;
        while (*p) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            char *seg = malloc(seglen + 1);
            if (seg) {
                memcpy(seg, p, seglen);
                seg[seglen] = 0;
                mm_window_move(listwin, row, area_x + content_x + 2);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.header, MM_THEME_HEADER),
                                 MM_COL_DEFAULT, 0, seg);
                ui_clear_to_eol(listwin, list_w - 2, ui_str_width(seg), MM_COL_DEFAULT);
                free(seg);
            }
            row += step;
            if (!nl) break;
            p = nl + 1;
        }
    }

    /* Prompt. */
    mm_window_move(promptwin, prompt_row, area_x + content_x);
    const char *pr = ui->prompt ? ui->prompt : "> ";
    mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.prompt, MM_THEME_PROMPT),
                     MM_COL_DEFAULT, ui->no_bold ? 0 : MM_BOLD, pr);
    /* The query is drawn in the prompt colour, like the reference does. */
    mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.prompt, MM_THEME_PROMPT),
                     MM_COL_DEFAULT, 0, ui->query_buf);
    int used = ui_str_width(pr) + ui_str_width(ui->query_buf);
    if (ui->ghost && *ui->ghost && !ui->query_len) {
        mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.ghost, MM_COL_DEFAULT),
                         MM_COL_DEFAULT, MM_DIM, ui->ghost);
        used += ui_str_width(ui->ghost);
    }
    ui_clear_to_eol(promptwin, list_w, used, MM_COL_DEFAULT);
    /* Keep the terminal cursor aligned with the UTF-8 query insertion point. */
    int query_cursor_width = ui_query_cursor_width(ui);
    mm_window_move(promptwin, prompt_row,
                   area_x + content_x + ui_str_width(pr) + query_cursor_width);

    /* When the info line is hidden, fzf draws a separator in its place. */
    if (!ui->info) {
        int sep_len = list_w;
        if (sep_len > 0) {
            char *sep = malloc((size_t)sep_len * 3 + 1);
            if (sep) {
                for (int i = 0; i < sep_len; i++) {
                    sep[i * 3 + 0] = (char)0xE2;
                    sep[i * 3 + 1] = (char)0x94;
                    sep[i * 3 + 2] = (char)0x80;
                }
                sep[sep_len * 3] = 0;
                mm_window_move(listwin, block_row, area_x + content_x);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                MM_COL_DEFAULT, 0, sep);
                free(sep);
            }
        }
    }

    /* Info line. */
    if (ui->info) {
        char info[64];
        const char *info_text = info;
        if (ui->info_command && *ui->info_command) {
            /* Run the info command and use its output as the info line. */
            static char info_cmd_buf[256];
            FILE *fp = popen(ui->info_command, "r");
            if (fp) {
                size_t o = 0;
                int c;
                while ((c = fgetc(fp)) != EOF && o < sizeof(info_cmd_buf) - 1) {
                    if (c == '\n') break;
                    info_cmd_buf[o++] = (char)c;
                }
                info_cmd_buf[o] = 0;
                pclose(fp);
                info_text = info_cmd_buf;
            }
        }
        int total_items = ui->nresults > ui->count ? ui->nresults : ui->count;
        int n;
        if (ui->info_command && *ui->info_command) {
            n = ui_str_width(info_text);
        } else {
            n = snprintf(info, sizeof(info), "%d/%d", ui->nresults, total_items);
            if (ui->multi) {
                /* fzf always shows the selection count in multi mode. */
                n += snprintf(info + n, sizeof(info) - (size_t)n, " (%d)", ui->nselected);
            }
        }
        int info_start_x = area_x + info_x;
        if (ui->info_inline_right) {
            info_start_x = area_x + area_w - n - 2;
            if (info_start_x < area_x + content_x)
                info_start_x = area_x + content_x;
        }
        mm_window_move(infowin, info_row, info_start_x);
        /* The indent is drawn with the default colour, the counts with the
           info colour and the separator with the border colour. */
        /* The spinner is shown while the input is still being read. */
        if (ui->input_fd >= 0 && !ui->input_done) {
            static const char *frames[] = {
                "\u280b", "\u2819", "\u2839", "\u2838", "\u283c",
                "\u2834", "\u2826", "\u2827", "\u2807", "\u280f"
            };
            static const char *frames_ascii[] = { "-", "\\", "|", "/" };
            int nf = ui->no_unicode ? 4 : 10;
            /* The frame follows the wall clock, the way the reference
               picks it (src/terminal.go printSpinner). */
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            long long ns = (long long)ts.tv_sec * 1000000000LL + ts.tv_nsec;
            int idx = (int)((ns % (100000000LL * nf)) / 100000000LL);
            const char *sp = ui->no_unicode ? frames_ascii[idx]
                                            : frames[idx % 10];
            mm_window_cprint(infowin, ui_theme_color(ui, ui->theme.spinner, MM_THEME_SPINNER),
                             MM_COL_DEFAULT, 0, sp);
            mm_window_cprint(infowin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, " ");
        } else {
            mm_window_cprint(infowin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, "  ");
        }
        mm_window_cprint(infowin, ui_theme_color(ui, ui->theme.info, MM_THEME_INFO),
                         MM_COL_DEFAULT, 0, info_text);
        int used = n + 2;
        if (!ui->info_inline_right && used < info_w) {
            /* The separator is U+2500 repeated; each glyph is three bytes.
               Size the buffer from the glyph count, not the other way round. */
            /* One column goes to the space that separates the counts from
               the separator. */
            int seplen = info_w - used - 1;
            if (seplen < 0) seplen = 0;
            /* The separator is repeated until the end of the line. */
            const char *sepchar = (ui->separator && *ui->separator) ? ui->separator
                                     : (ui->no_unicode ? "-" : "\u2500");
            size_t seplen_bytes = strlen(sepchar);
            if (seplen_bytes == 0) seplen_bytes = 1;
            /* The separator is repeated until the line is full; a separator
               that is wider than one column is repeated less often. */
            int sepw = ui_str_width(sepchar);
            if (sepw < 1) sepw = 1;
            /* The separator is repeated until the line is full; the last
               repetition is cut when it does not fit completely. */
            int reps = (seplen + sepw - 1) / sepw;
            size_t bytes = (size_t)seplen * seplen_bytes + 1;
            char *sep = malloc(bytes);
            if (sep) {
                size_t o = 0;
                for (int i = 0; i < reps; i++) {
                    int w = sepw;
                    if ((int)(o / seplen_bytes + 1) * sepw > seplen) {
                        w = seplen - (int)(o / seplen_bytes) * sepw;
                        if (w < 0) w = 0;
                    }
                    /* Copy the separator, but only as many columns as are
                       left on the line. */
                    int cols = 0;
                    size_t k = 0;
                    while (k < seplen_bytes && cols < w) {
                        wchar_t cp;
                        size_t used = mm_utf8_decode(sepchar + k, seplen_bytes - k, &cp);
                        if (used == 0) used = 1;
                        memcpy(sep + o, sepchar + k, used);
                        o += used;
                        cols += mm_runewidth(cp);
                        k += used;
                    }
                }
                sep[o] = 0;
                mm_window_cprint(infowin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                 MM_COL_DEFAULT, ui->no_color ? MM_DIM : 0, " ");
                mm_window_cprint(infowin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                 MM_COL_DEFAULT, ui->no_color ? MM_DIM : 0, sep);
                free(sep);
            }
        }
    }

    /* The text area excludes the pointer, the space after it and the
       scrollbar column (fzf: t.wrapCols). */
    int text_limit = list_w - 2;
    if (text_limit < 1) text_limit = 1;
    int bar_len = 0, bar_start = 0;
    ui_scrollbar_range(ui->nresults, list_rows, ui->offset, &bar_len, &bar_start);
    /* Default layout is bottom-up: the scrollbar starts at the bottom and
       moves up; with --reverse it starts at the top. */
    if (!ui->reverse && bar_len > 0) {
        bar_start = (list_rows - bar_len) - bar_start;
        if (bar_start < 0) bar_start = 0;
    }

    /* List. In the default layout the list is drawn bottom-up: the first
       match sits on the line directly above the info line. With --reverse
       the list starts right below the info line and grows downwards. */
    /* The items are laid out from the bottom of the list area upwards (or
       downwards in the reverse layout).  A wrapped item occupies one line
       per wrapped line, and the gap adds blank lines between the items. */
    int nitems = ui->nresults - ui->offset;
    if (nitems < 0) nitems = 0;
    int dir = (ui->reverse || ui->layout == 1) ? 1 : -1;
    int y = (dir > 0) ? list_first : list_last;
    int block = 0;
    while (block < nitems) {
        int idx = ui->offset + block;
        /* Only the items that are (partly) visible are drawn; the rest of
           the list is skipped, which matters for large inputs. */
        if (dir < 0) {
            if (y < list_first) break;
        } else {
            if (y > list_last) break;
        }
        {
        int item_idx = ui->results[idx];
        const char *text = ui->lines[item_idx];
        const mm_match_pos_t *matched = (ui->match_pos && idx < ui->match_pos_n)
                                          ? &ui->match_pos[idx] : NULL;
        bool cur = (idx == ui->cursor);
        bool sel = ui->selected && ui->selected[item_idx];
        int text_width = 0;
        char *fitted = NULL;
        char **pieces = NULL;
        int *piece_off = NULL;
        int n_pieces = 0;
        int sign_width = 0;
        const char *sign = ui->wrap_sign ? ui->wrap_sign : "";

        /* --hscroll-off: horizontal scroll offset for lines wider than the
           display area. Computed from the current item's match position so
           that the match stays visible with hscroll_off columns of margin. */
        int hscroll = 0;
        if (ui->hscroll_off > 0 && cur && matched && matched->npos > 0 &&
            !ui->wrap && text_limit > 0) {
            size_t len = strlen(text);
            size_t i = 0;
            int rune = 0;
            int match_col = 0;
            /* Find the display column of the first match position. */
            while (i < len && rune < matched->pos[0]) {
                wchar_t cp;
                size_t used = mm_utf8_decode(text + i, len - i, &cp);
                if (used == 0) used = 1;
                match_col += mm_runewidth(cp);
                i += used;
                rune++;
            }
            if (match_col > text_limit - ui->hscroll_off) {
                hscroll = match_col - text_limit + ui->hscroll_off;
            }
        }

        if (ui->wrap) {
            sign_width = ui_str_width(sign);
            int cont = text_limit - sign_width;
            if (cont < 1) cont = 1;
            n_pieces = ui_wrap_text(text, text_limit, cont, ui->wrap_word, &pieces, &piece_off);
        } else {
            /* --hscroll-off: skip hscroll display columns at the start of
               the text, then fit the remainder. */
            if (hscroll > 0) {
                size_t len = strlen(text);
                size_t i = 0;
                int acc = 0;
                while (i < len && acc < hscroll) {
                    wchar_t cp;
                    size_t used = mm_utf8_decode(text + i, len - i, &cp);
                    if (used == 0) used = 1;
                    int cw = mm_runewidth(cp);
                    if (acc + cw > hscroll) break;
                    acc += cw;
                    i += used;
                }
                char *sub = strdup(text + i);
                if (sub) {
                    fitted = ui_fit_text(sub, text_limit - hscroll, &text_width,
                                         ui->ellipsis);
                    text_width += hscroll;
                    free(sub);
                } else {
                    fitted = ui_fit_text(text, text_limit, &text_width, ui->ellipsis);
                }
            } else {
                fitted = ui_fit_text(text, text_limit, &text_width, ui->ellipsis);
            }
        }

        /* The block of the item: the first line of a wrapped item is at the
           top of the block, the continuations are below it. */
        int n_cont = (n_pieces > 0) ? n_pieces - 1 : 0;
        int screen_y = (dir < 0) ? y - n_cont : y;
        int block_last = screen_y + n_cont;
        mm_window_move(listwin, screen_y, area_x + content_x + list_border_inset);

        /* The attributes of the current line and of the gutter. */
        mm_attr_t line_attr = cur ? (ui->no_color ? (MM_BOLD | MM_REVERSE)
                                             : (ui->no_bold ? 0 : MM_BOLD))
                                  : 0;
        mm_attr_t pointer_attr = cur ? ((ui->no_bold || ui->no_color) ? 0 : MM_BOLD)
                                    : (ui->no_color ? MM_DIM : 0);
        /* Without unicode the gutter column is blank and uses the default
           colours (fzf's behaviour). */
        mm_color_t pfg = cur ? ui_theme_color(ui, ui->theme.pointer, MM_THEME_POINTER)
                             : (ui->no_unicode
                                    ? MM_COL_DEFAULT
                                    : ui_theme_color(ui, ui->theme.gutter, MM_THEME_DARK_BG));
        mm_color_t fg = cur ? ui_theme_color(ui, ui->theme.current, MM_THEME_CURRENT)
                            : MM_COL_DEFAULT;
        mm_color_t bg = cur ? ui_theme_color(ui, ui->theme.dark_bg, MM_THEME_DARK_BG)
                            : MM_COL_DEFAULT;
        mm_attr_t attr = line_attr;
        if (sel && !cur) {
            /* A selected item that is not the current one uses the theme's
               selected colours (the default ones in the dark theme). */
            fg = ui_theme_color(ui, ui->theme.selected_fg, MM_COL_DEFAULT);
            bg = ui_theme_color(ui, ui->theme.selected_bg, MM_COL_DEFAULT);
        }

        /* The pointer marks the current line; the other lines keep the
           default gutter character (fzf's behaviour).  Items that are
           outside of the list area are not drawn at all. */
        bool visible = (screen_y >= list_first && screen_y <= list_last);
        /* Without unicode the gutter is a blank column drawn in reverse
           video on the current line (fzf's behaviour). */
        const char *gutter = ui->gutter ? ui->gutter
                                        : (ui->no_unicode ? " " : "\u258c");
        if (ui->no_unicode && cur) pointer_attr |= MM_REVERSE;
        /* The column after the pointer holds the multi-select marker.  The
           default marker is U+2503 (fzf's default). */
        static const char *default_marker = "\u2503";
        static const char *default_marker_ascii = ">";
        const char *mark = (sel && ui->multi)
                               ? (ui->marker ? ui->marker
                                             : (ui->no_unicode ? default_marker_ascii
                                                               : default_marker))
                               : " ";
        /* The marker of a selected item uses the theme's marker colour. */
        mm_color_t mfg = (sel && ui->multi)
                             ? ui_theme_color(ui, ui->theme.marker, MM_THEME_MARKER)
                             : (cur ? pfg : MM_COL_DEFAULT);
        if (visible) {
            /* The current line shows the pointer, the other lines the
               gutter character. */
            const char *ptr = cur ? ((ui->pointer) ? ui->pointer
                                                    : (ui->no_unicode ? " " : "\u258c"))
                                      : gutter;
            mm_window_cprint(listwin, pfg, bg, pointer_attr, ptr);
            mm_window_cprint(listwin, mfg, bg, 0, mark);
        }
        if (visible && ui->ansi) {
            mm_renderer_stderr(listwin->r, fitted ? fitted : text);
        } else if (visible && n_pieces > 0) {
            /* The wrapped lines: the first one is here, the continuations
               are drawn on the lines below. */
            for (int c = 0; c < n_pieces; c++) {
                if (screen_y + c < list_first || screen_y + c > list_last) continue;
                if (c > 0) {
                    mm_window_move(listwin, screen_y + c, area_x + content_x + list_border_inset);
                    mm_window_cprint(listwin, pfg, bg, pointer_attr,
                                     (cur && ui->pointer) ? ui->pointer : "\u258c");
                    mm_window_cprint(listwin, mfg, bg, 0, mark);
                    /* The wrap sign uses the style of the line with the
                       dim attribute added (fzf's behaviour). */
                    mm_window_cprint(listwin, fg, bg, attr | MM_DIM, sign);
                }
                /* The matched offsets of this piece. */
                mm_match_pos_t sub = { NULL, 0 };
                int *subpos = NULL;
                int nsub = 0;
                if (matched && matched->npos > 0 && matched->pos) {
                    subpos = malloc(sizeof(int) * (size_t)matched->npos);
                    if (subpos) {
                        for (int k = 0; k < matched->npos; k++) {
                            int p = matched->pos[k] - piece_off[c];
                            if (p >= 0) subpos[nsub++] = p;
                        }
                    }
                }
                sub.pos = subpos;
                sub.npos = nsub;
                ui_print_item(listwin, ui, pieces[c], &sub, fg, bg, attr, cur);
                free(subpos);
                /* Clear the rest of this line.  Every line is cleared on its
                   own, so that the spaces never run past the edge of the
                   row. */
                int used = 2 + (c > 0 ? sign_width : 0) + ui_str_width(pieces[c]);
                mm_color_t clear_bg = (cur && ui->highlight_line) ? bg : MM_COL_DEFAULT;
                ui_clear_to_eol(listwin, area_x + area_w - 1,
                                area_x + content_x + list_border_inset + used, clear_bg);
            }
        } else if (visible) {
            ui_print_item(listwin, ui, fitted, matched, fg, bg, attr, cur);
            mm_color_t clear_bg = (cur && ui->highlight_line) ? bg : MM_COL_DEFAULT;
            ui_clear_to_eol(listwin, area_x + area_w - 1,
                            area_x + content_x + list_border_inset + 2 + text_width, clear_bg);
        }
        free(fitted);
        if (pieces) {
            for (int c = 0; c < n_pieces; c++) free(pieces[c]);
            free(pieces);
        }
        free(piece_off);

        /* Scrollbar, drawn in the last column of the list area. */
        if (!ui->no_scrollbar && bar_len > 0) {
            int screen_line = screen_y;
            int row_in_list = screen_line - list_first;
            if (row_in_list >= bar_start && row_in_list < bar_start + bar_len) {
                mm_window_move(listwin, screen_line,
                              b_right ? area_x + area_w - 2 : area_x + area_w - 1);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.scrollbar, MM_THEME_BORDER),
                                 MM_COL_DEFAULT, 0,
                                 ui->no_unicode ? "|" : "\u2502");
            } else {
                mm_window_move(listwin, screen_line,
                              b_right ? area_x + area_w - 2 : area_x + area_w - 1);
                mm_window_cprint(listwin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, " ");
            }
        }

        /* The gap follows the item in the drawing order: in the default
           layout the items are drawn bottom-up, so the gap is drawn above
           the item, in the reverse layout below it.  The gap line is drawn
           on the row next to the following item. */
        if (ui->gap > 0) {
            for (int g = 0; g < ui->gap; g++) {
                int gr = (dir < 0) ? (screen_y - 1 - g) : (block_last + 1 + g);
                if (gr < list_first || gr > list_last) continue;
                mm_window_move(listwin, gr, area_x + content_x + list_border_inset);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.gutter, MM_THEME_DARK_BG),
                                 MM_COL_DEFAULT, ui->no_color ? MM_DIM : 0, "\u258c");
                mm_window_cprint(listwin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, " ");
                /* The gap line stops before the scrollbar column. */
                int fill = area_w - 3 - list_border_inset;
                if (g == ui->gap - 1 && ui->gap_line && *ui->gap_line && fill > 0) {
                    size_t gl = strlen(ui->gap_line);
                    char *rep = malloc(gl * (size_t)fill + 1);
                    if (rep) {
                        for (int i = 0; i < fill; i++)
                            memcpy(rep + (size_t)i * gl, ui->gap_line, gl);
                        rep[gl * (size_t)fill] = 0;
                        mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                         MM_COL_DEFAULT, 0, rep);
                        free(rep);
                    }
                } else {
                    ui_clear_to_eol(listwin, area_x + area_w - 1, area_x + content_x + list_border_inset + 2, MM_COL_DEFAULT);
                }
            }
        }

        /* --separator: draw the separator line between items, replacing the
           first gap row when --gap is set. Without --gap there is no spare
           row, so the separator is skipped to avoid corrupting the next item. */
        if (ui->separator && *ui->separator && ui->gap > 0 &&
            block + 1 < nitems) {
            int sep_row = (dir < 0) ? (screen_y - 1) : (block_last + 1);
            if (sep_row >= list_first && sep_row <= list_last) {
                mm_window_move(listwin, sep_row, area_x + content_x + list_border_inset + 2);
                int fill = list_w - 2;
                if (fill > 0) {
                    size_t sl = strlen(ui->separator);
                    char *rep = malloc((size_t)fill + 1);
                    if (rep) {
                        for (int i = 0; i < fill; i++)
                            rep[i] = ui->separator[i % sl];
                        rep[fill] = 0;
                        mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                         MM_COL_DEFAULT, 0, rep);
                        free(rep);
                    } else {
                        ui_clear_to_eol(listwin, area_x + area_w - 1, area_x + content_x + list_border_inset + 2, MM_COL_DEFAULT);
                    }
                }
            }
        }

        /* The next item. */
        y = (dir < 0) ? (screen_y - ui->gap - 1) : (block_last + ui->gap + 1);
        block++;
        }
    }

    /* The rows that are left over are empty. */
    for (int gr = y; gr >= list_first && gr <= list_last; gr += dir) {
        mm_window_move(listwin, gr, area_x + content_x + list_border_inset);
        mm_window_cprint(listwin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, "");
        ui_clear_to_eol(listwin, area_x + area_w - 1, area_x + content_x + list_border_inset, MM_COL_DEFAULT);
    }

    /* Border box around the whole UI. */
    if (ui->border != MM_BORDER_NONE) {
        const mm_border_glyphs_t *g = ui->no_unicode ? mm_border_glyphs_ascii(ui->border)
                                                    : mm_border_glyphs(ui->border);
        if (g) {
            mm_color_t bcol = ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER);
            int box_w = width;        /* the border spans the whole width */
            size_t cap = (size_t)box_w * 4 + 16;
            char *p = malloc(cap);
            if (p) {
                /* The horizontal line runs between the corners; without a
                   corner on one side it spans the full width. */
                int dash_count = (b_left && b_right) ? box_w - 2 : box_w;
                if (b_top) {
                    size_t o = 0;
                    mm_window_move(listwin, 0, 0);
                    if (b_left) {
                        memcpy(p + o, g->top_left, strlen(g->top_left));
                        o += strlen(g->top_left);
                    }
                    for (int i = 0; i < dash_count; i++) {
                        memcpy(p + o, g->top, strlen(g->top));
                        o += strlen(g->top);
                    }
                    if (b_right) {
                        memcpy(p + o, g->top_right, strlen(g->top_right));
                        o += strlen(g->top_right);
                    }
                    p[o] = 0;
                    mm_window_cprint(listwin, bcol, MM_COL_DEFAULT, 0, p);                }
                if (b_bottom) {
                    size_t o = 0;
                    mm_window_move(listwin, total - 1, 0);
                    if (b_left) {
                        memcpy(p + o, g->bottom_left, strlen(g->bottom_left));
                        o += strlen(g->bottom_left);
                    }
                    for (int i = 0; i < dash_count; i++) {
                        memcpy(p + o, g->bottom, strlen(g->bottom));
                        o += strlen(g->bottom);
                    }
                    if (b_right) {
                        memcpy(p + o, g->bottom_right, strlen(g->bottom_right));
                        o += strlen(g->bottom_right);
                    }
                    p[o] = 0;
                    mm_window_cprint(listwin, bcol, MM_COL_DEFAULT, 0, p);                }
                if (ui->border_label && *ui->border_label) {
                    int row = -1;
                    if (!ui->border_label_bottom && b_top) row = 0;
                    else if (b_bottom && (ui->border_label_bottom || !b_top))
                        row = total - 1;
                    if (row >= 0) {
                        int lw = ui_str_width(ui->border_label);
                        int col;
                        if (ui->border_label_pos == 0) col = (width - lw) / 2;
                        else if (ui->border_label_pos > 0)
                            col = ui->border_label_pos - 1;
                        else col = width - lw + ui->border_label_pos + 1;
                        if (col < 0) col = 0;
                        mm_window_move(listwin, row, col);
                        mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.border_label, 145), MM_COL_DEFAULT, 0, ui->border_label);
                    }
                }
                if (b_left || b_right) {
                    int first = b_top ? 1 : 0;
                    int last = b_bottom ? total - 2 : total - 1;
                    for (int y = first; y <= last; y++) {
                        /* The vertical edges are two columns wide: the
                           border character and a padding space. */
                        if (b_left) {
                            mm_window_move(listwin, y, 0);
                            mm_window_cprint(listwin, bcol, MM_COL_DEFAULT, 0, g->left);
                            mm_window_cprint(listwin, bcol, MM_COL_DEFAULT, 0, " ");
                        }
                        if (b_right) {
                            /* The padding space in front of the right edge
                               belongs to the content area, which is drawn
                               over it. */
                            mm_window_move(listwin, y, box_w - 1);
                            mm_window_cprint(listwin, bcol, MM_COL_DEFAULT, 0, g->right);
                        }
                    }
                }
                free(p);
            }
        }
    }

    if (draw_component_boxes) {
        int list_shape = ui->list_border_set ? ui->list_border : ui->border;
        const mm_border_glyphs_t *lg = ui->no_unicode
            ? mm_border_glyphs_ascii(list_shape) : mm_border_glyphs(list_shape);
        int header_shape = ui->header_border_set ? ui->header_border : ui->border;
        const mm_border_glyphs_t *hg = ui->no_unicode
            ? mm_border_glyphs_ascii(header_shape) : mm_border_glyphs(header_shape);
        int input_shape = ui->input_border_set ? ui->input_border : ui->border;
        const mm_border_glyphs_t *ig = ui->no_unicode
            ? mm_border_glyphs_ascii(input_shape) : mm_border_glyphs(input_shape);
        mm_color_t bcol = ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER);
        int left = area_x + (b_left ? 1 : 0);
        int right = area_x + area_w - 1;
        if (draw_input_box)
            ui_draw_border_box(listwin, ig, bcol, left, right,
                               prompt_row - 1, prompt_row + 1, total);
        if (draw_input_box && ui->input_label && *ui->input_label) {
            int label_width = ui_str_width(ui->input_label);
            int label_row = ui->input_label_bottom ? prompt_row + 1 : prompt_row - 1;
            if (label_width > 0 && label_width <= right - left - 1 &&
                label_row >= 0 && label_row < total) {
                int label_x;
                if (ui->input_label_pos == 0)
                    label_x = left + (right - left - label_width) / 2;
                else if (ui->input_label_pos > 0)
                    label_x = left + ui->input_label_pos;
                else
                    label_x = right - label_width + ui->input_label_pos;
                if (label_x < left + 1) label_x = left + 1;
                if (label_x + label_width > right)
                    label_x = right - label_width;
                mm_window_move(listwin, label_row, label_x);
                mm_window_cprint(listwin,
                    ui_theme_color(ui, ui->theme.border_label, 145),
                    MM_COL_DEFAULT, 0, ui->input_label);
            }
        }
        if (draw_list_box)
            ui_draw_border_box(listwin, lg, bcol, left, right,
                               list_first - 1, list_last + 1, total);
        if (draw_list_box && ui->list_label && *ui->list_label) {
            int label_width = ui_str_width(ui->list_label);
            int label_row = ui->list_label_bottom ? list_last + 1 : list_first - 1;
            if (label_width > 0 && label_width <= right - left - 1 &&
                label_row >= 0 && label_row < total) {
                int label_x;
                if (ui->list_label_pos == 0)
                    label_x = left + (right - left - label_width) / 2;
                else if (ui->list_label_pos > 0)
                    label_x = left + ui->list_label_pos;
                else
                    label_x = right - label_width + ui->list_label_pos;
                if (label_x < left + 1) label_x = left + 1;
                if (label_x + label_width > right)
                    label_x = right - label_width;
                mm_window_move(listwin, label_row, label_x);
                mm_window_cprint(listwin,
                    ui_theme_color(ui, ui->theme.border_label, 145),
                    MM_COL_DEFAULT, 0, ui->list_label);
            }
        }
        if (draw_header_box)
            ui_draw_border_box(listwin, hg, bcol, left, right,
                               header_row - 1, header_row + header_rows, total);
        if (draw_header_box && ui->header_label && *ui->header_label) {
            int label_width = ui_str_width(ui->header_label);
            int label_row = ui->header_label_bottom
                ? header_row + header_rows : header_row - 1;
            if (label_width > 0 && label_width <= right - left - 1 &&
                label_row >= 0 && label_row < total) {
                int label_x;
                if (ui->header_label_pos == 0)
                    label_x = left + (right - left - label_width) / 2;
                else if (ui->header_label_pos > 0)
                    label_x = left + ui->header_label_pos;
                else
                    label_x = right - label_width + ui->header_label_pos;
                if (label_x < left + 1) label_x = left + 1;
                if (label_x + label_width > right)
                    label_x = right - label_width;
                mm_window_move(listwin, label_row, label_x);
                mm_window_cprint(listwin,
                    ui_theme_color(ui, ui->theme.border_label, 145),
                    MM_COL_DEFAULT, 0, ui->header_label);
            }
        }
    }

    /* Preview window. */
    if (has_preview) {
        ui_draw_preview(ui, r, listwin, pv_x, pv_y, pv_w, pv_h);
    }

    /* Leave the terminal cursor at the query insertion point. */
    const char *pr2 = ui->prompt ? ui->prompt : "> ";
    mm_renderer_move(r, prompt_row,
                     area_x + content_x + ui_str_width(pr2) +
                     ui_query_cursor_width(ui));

    mm_renderer_flush(r);
}

/* Return the expect key matching the event, or NULL. */
static const char *ui_expect_key(mm_ui_t *ui, const mm_event_t *ev) {
    for (int i = 0; i < ui->expect_n; i++) {
        const char *name = ui->expect_keys[i];
        mm_event_type_t t;
        if (!mm_event_from_name(name, &t)) continue;
        if (t == ev->type) return name;
        if (t == MM_EV_RUNE && ev->type == MM_EV_RUNE) {
            /* Compare the literal character. */
            if (name[0] && (wchar_t)name[0] == ev->rune) return name;
        }
    }
    return NULL;
}

/* --- preview window --- */
/* The preview box occupies one side of the screen. Returns the rectangle it
   covers: x, y, width, height. */
/* Draw the preview box and its content. */
static mm_color_t ui_theme_color(const mm_ui_t *ui, mm_color_attr_t entry,
                                 mm_color_t fallback) {
    if (ui->no_color) return MM_COL_DEFAULT;
    return ui_color(entry, fallback);
}

/* Print the padding of a preview row.  With a gap the reference draws the
   first and the last column of the content area in the border colour. */
static void ui_preview_pad(mm_window_t *win, mm_ui_t *ui, int content_w, int used) {
    int fill = content_w - used;
    if (fill <= 0) return;
    mm_color_t col = ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER);
    char *pad = malloc((size_t)fill + 1);
    if (!pad) return;
    memset(pad, ' ', (size_t)fill);
    pad[fill] = 0;
    (void)col;
    mm_window_cprint(win, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, pad);
    free(pad);
}

/* The scrollbar and the line indicator of the preview window; both are
   shown when the output does not fit into the window. */
static void ui_preview_scrollbar(mm_ui_t *ui, mm_window_t *win, int x, int y, int w,
                                  int content_x, int content_y, int content_w,
                                  int content_h, int nlines) {
    (void)x;
    (void)y;
    (void)w;
    if (nlines <= content_h || content_h <= 0) return;
    int bar_len = content_h * content_h / nlines;
    if (bar_len < 1) bar_len = 1;
    for (int i = 0; i < bar_len; i++) {
        int r = content_y + i;
        if (r >= content_y + content_h) break;
        mm_window_move(win, r, content_x + content_w);
        mm_window_cprint(win, ui_theme_color(ui, ui->theme.scrollbar, MM_THEME_BORDER),
                         MM_COL_DEFAULT, 0,
                         ui->no_unicode ? "|" : "\u2502");
    }
    char info[32];
    int n = snprintf(info, sizeof(info), "1/%d", nlines);
    if (n > 0 && content_w - 2 - n >= 0) {
        mm_window_move(win, content_y, content_x + content_w - n);
        mm_window_cprint(win, ui_theme_color(ui, ui->theme.info, MM_THEME_INFO),
                         MM_COL_DEFAULT, 0, info);
    }
}

static void ui_draw_preview(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *win,
                            int x, int y, int w, int h) {
    (void)r;
    if (w <= 0 || h <= 0) return;
    const mm_border_glyphs_t *g = ui->no_unicode
        ? mm_border_glyphs_ascii(ui->preview_border)
        : mm_border_glyphs(ui->preview_border);
    mm_color_t bcol = ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER);
    bool bordered = ui->preview_win.border && g;
    bool full_border = bordered && !ui->preview_win.border_left_only;

    int content_x = x + (bordered ? 2 : 0);
    int content_y = y + (full_border ? 1 : 0);
    /* A left-only border reserves its glyph and padding but no top, bottom,
       or right edge. */
    int content_w = w - (full_border ? 4 : (bordered ? 2 : 0));
    int content_h = h - (full_border ? 2 : 0);

    /* With a gap the reference pads the preview content with the border
       colour. */
    if (bordered) {
        /* The reference draws the space between the border and the content
           in the border colour. */
        mm_window_move(win, content_y, x + 1);
        mm_window_cprint(win, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                         MM_COL_DEFAULT, 0, " ");
    }

    if (full_border) {
        /* top and bottom lines */
        for (int which = 0; which < 2; which++) {
            int row = which == 0 ? y : (y + h - 1);
            char *line = malloc((size_t)w * 4 + 8);
            if (!line) break;
            size_t o = 0;
            const char *lc = which == 0 ? g->top_left : g->bottom_left;
            const char *rc = which == 0 ? g->top_right : g->bottom_right;
            const char *mid = which == 0 ? g->top : g->bottom;
            memcpy(line + o, lc, strlen(lc)); o += strlen(lc);
            for (int i = 0; i < w - 2; i++) {
                memcpy(line + o, mid, strlen(mid)); o += strlen(mid);
            }
            memcpy(line + o, rc, strlen(rc)); o += strlen(rc);
            line[o] = 0;
            mm_window_move(win, row, x);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, line);
            free(line);
        }
        if (ui->preview_label && *ui->preview_label) {
            int label_width = ui_str_width(ui->preview_label);
            if (label_width > 0 && label_width <= w - 2) {
                int label_row = ui->preview_label_bottom ? y + h - 1 : y;
                int label_x;
                if (ui->preview_label_pos == 0)
                    label_x = x + (w - label_width) / 2;
                else if (ui->preview_label_pos > 0)
                    label_x = x + ui->preview_label_pos - 1;
                else
                    label_x = x + w - label_width + ui->preview_label_pos + 1;
                if (label_x < x + 1) label_x = x + 1;
                if (label_x + label_width > x + w - 1)
                    label_x = x + w - label_width - 1;
                mm_window_move(win, label_row, label_x);
                mm_window_cprint(win,
                    ui_theme_color(ui, ui->theme.border_label, 145),
                    MM_COL_DEFAULT, 0, ui->preview_label);
            }
        }
        for (int i = 1; i < h - 1; i++) {
            mm_window_move(win, y + i, x);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, g->left);
            /* The padding between the border and the content is drawn in
               the border colour on every line. */
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, " ");
            mm_window_move(win, y + i, x + w - 2);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, " ");
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, g->right);
        }
    } else if (bordered) {
        /* border-left: draw the divider without top/bottom corners or a
           border on the far side of the preview. */
        for (int i = 0; i < h; i++) {
            mm_window_move(win, y + i, x);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, g->left);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, " ");
        }
    }

    /* The content area is cleared, so that the leftovers of the previous
       frame do not show through. */
    if (bordered) {
        for (int i = 0; i < content_h; i++) {
            mm_window_move(win, content_y + i, content_x);
            ui_clear_to_eol(win, content_x + content_w, content_x, MM_COL_DEFAULT);
        }
    }

    /* The command output, one line per row. */
    const char *out = mm_preview_output(ui->preview);
    int row = content_y;
    if (out && *out && ui->preview_win.wrap) {
        /* Soft wrap: the first line holds the text, the continuation lines
           are prefixed with the wrap sign, the way fzf does it. */
        const char *sign = ui->preview_wrap_sign
            ? ui->preview_wrap_sign : "↳ ";
        int sign_w = ui_str_width(sign);
        int width = content_w;
        if (width < 1) width = 1;
        const char *p = out;
        int first = 1;
        while (*p && row < content_y + content_h) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            size_t off = 0;
            do {
                /* The available width depends on whether this is the first
                   line of the wrapped text or a continuation. */
                int avail = first ? width : width - sign_w;
                if (avail < 1) avail = 1;
                int cols = 0;
                size_t cut = off;
                while (cut < len) {
                    wchar_t cp;
                    size_t used = mm_utf8_decode(p + cut, len - cut, &cp);
                    if (used == 0) used = 1;
                    int cw = mm_runewidth(cp);
                    if (cols + cw > avail) break;
                    cols += cw;
                    cut += used;
                }
                if (cut == off && off < len) {
                    /* A single character does not fit: take it anyway. */
                    wchar_t cp;
                    size_t used = mm_utf8_decode(p + off, len - off, &cp);
                    cut = off + (used ? used : 1);
                }
                char *chunk = malloc(cut - off + 1);
                if (!chunk) break;
                memcpy(chunk, p + off, cut - off);
                chunk[cut - off] = 0;
                mm_window_move(win, row, content_x);
                if (!first) mm_window_cprint(win, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, sign);
                mm_window_cprint(win, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, chunk);
                ui_preview_pad(win, ui, content_w, ui_str_width(chunk));
                free(chunk);
                row++;
                off = cut;
                first = 0;
                if (row >= content_y + content_h) break;
                if (off >= len) break;
            } while (off < len);
            if (!nl) break;
            p = nl + 1;
            first = 1;
        }
        /* The scrollbar and the line indicator, shown when the output does
           not fit. */
        {
            int nlines = 1;
            for (const char *q = out; *q; q++) if (*q == '\n') nlines++;
            size_t olen = strlen(out);
            if (olen > 0 && out[olen - 1] == '\n') nlines--;
            ui_preview_scrollbar(ui, win, x, y, w, content_x, content_y, content_w,
                                 content_h, nlines);
        }
        return;
    }
    if (out && *out) {
        /* Count the lines so that the scrollbar and the "N/M" indicator can
           be drawn, the way fzf does (src/terminal.go). */
        int nlines = 1;
        for (const char *q = out; *q; q++) if (*q == '\n') nlines++;
        /* A trailing newline does not start a new line. */
        size_t olen = strlen(out);
        if (olen > 0 && out[olen - 1] == '\n') nlines--;
        const char *p = out;
        while (*p && row < content_y + content_h) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char *line = malloc(len + 1);
            if (!line) break;
            memcpy(line, p, len);
            line[len] = 0;
            mm_window_move(win, row, content_x);
            /* Clip to the available width. */
            int wchars = mm_utf8_count(line);
            if (wchars > content_w) {
                int cut = 0, i = 0;
                while (i < (int)len && cut < content_w) {
                    int clen = 1;
                    unsigned char c = (unsigned char)line[i];
                    if (c >= 0xF0) clen = 4; else if (c >= 0xE0) clen = 3;
                    else if (c >= 0xC0) clen = 2;
                    i += clen; cut++;
                }
                line[i] = 0;
            }
            mm_window_cprint(win, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, line);
            ui_preview_pad(win, ui, content_w, ui_str_width(line));
            free(line);
            row++;
            if (!nl) break;
            p = nl + 1;
        }

        ui_preview_scrollbar(ui, win, x, y, w, content_x, content_y, content_w,
                             content_h, nlines);
    }
}

/* Print an item, highlighting the matched characters in the "current match"
   colour the way the reference does. */
static void ui_print_item(mm_window_t *win, mm_ui_t *ui, const char *text,
                          const mm_match_pos_t *matched,
                          mm_color_t fg, mm_color_t bg, mm_attr_t attr,
                          bool cur) {
    if (!matched || matched->npos <= 0 || !matched->pos) {
        mm_window_cprint(win, fg, bg, attr, text);
        return;
    }
    /* The current line uses the "current match" colour, the other lines the
       plain "match" colour. */
    mm_color_t mfg = ui_color(cur ? ui->theme.current_match : ui->theme.match,
                             cur ? MM_THEME_CURRENT_MATCH : MM_THEME_MATCH);
    size_t len = strlen(text);
    size_t i = 0;
    int rune = 0;
    int mi = 0;
    bool in_match = false;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        /* Advance the match cursor to this rune. */
        while (mi < matched->npos && matched->pos[mi] < rune) mi++;
        bool hit = (mi < matched->npos && matched->pos[mi] == rune);
        if (hit != in_match) {
            /* The colour changes: flush the current run. */
            in_match = hit;
        }
        /* Print the run up to the next colour change. */
        size_t start = i;
        while (i < len) {
            wchar_t c2;
            size_t u2 = mm_utf8_decode(text + i, len - i, &c2);
            if (u2 == 0) u2 = 1;
            while (mi < matched->npos && matched->pos[mi] < rune) mi++;
            bool h2 = (mi < matched->npos && matched->pos[mi] == rune);
            if (h2 != in_match) break;
            i += u2;
            rune++;
        }
        char *run = malloc(i - start + 1);
        if (!run) { mm_window_cprint(win, fg, bg, attr, text + start); break; }
        memcpy(run, text + start, i - start);
        run[i - start] = 0;
        mm_window_cprint(win, in_match ? mfg : fg, bg, attr, run);
        free(run);
    }
}

/* Replace the item list with the output of a command. */
static void ui_reload(mm_ui_t *ui, const char *cmd) {
    FILE *fp = popen(cmd, "r");
    if (!fp) return;
    char **lines = NULL;
    int n = 0, cap = 0;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    while ((len = getline(&line, &lcap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (n == cap) { cap = cap ? cap * 2 : 64; lines = realloc(lines, (size_t)cap * sizeof(char *)); }
        if (!lines) break;
        lines[n] = malloc((size_t)len + 1);
        if (!lines[n]) break;
        memcpy(lines[n], line, (size_t)len + 1);
        n++;
    }
    free(line);
    pclose(fp);
    if (!lines) return;
    /* Replace the items with the new list. */
    for (int i = 0; i < ui->count; i++) free(ui->lines[i]);
    free(ui->lines);
    ui->lines = lines;
    ui->count = n;
    /* The item array has to grow with the list. */
    mm_item_t *items = realloc(ui->items, (size_t)(n ? n : 1) * sizeof(mm_item_t));
    if (!items) return;
    ui->items = items;
    for (int i = 0; i < n; i++) {
        mm_item_init(&ui->items[i], lines[i], i);
        mchars_decode(&ui->items[i].text);
    }
    free(ui->selected);
    ui->selected = calloc((size_t)(n ? n : 1), sizeof(bool));
    ui->nselected = 0;
    ui->cursor = 0;
    ui->offset = 0;
    ui_search(ui);
}

/* Evaluate a --height specification.  Returns the number of rows the inline
   window may use, or 0 for the full screen.  A percentage is relative to the
   terminal height and never drops below fzf's minimum height of 10 rows for
   the list, plus the info line and the prompt. */
int mm_ui_height(const char *spec, int rows, int min_height) {
    if (!spec || !*spec) return 0;
    const char *p = spec;
    long value = strtol(p, (char **)&p, 10);
    bool percent = (p && *p == '%');
    if (value <= 0 || rows <= 0) return 0;
    if (!percent) {
        int h = (int)value;
        if (min_height > h) h = min_height;
        if (h > rows) h = rows;
        return h;
    }
    int h = (int)((double)value * (double)rows / 100.0);
    if (min_height > 0) {
        /* An explicit --min-height is the lower bound of the window. */
        if (h < min_height) h = min_height;
    } else {
        /* Without --min-height a percentage never drops below ten rows for
           the list, plus the info line and the prompt. */
        if (h < 10) h = 10;
        h += 2;
    }
    if (h > rows) h = rows;
    return h;
}

/* Start (or restart) the preview command for the current item. */
static void ui_preview_restart(mm_ui_t *ui) {
    if (!ui->preview) return;
    int idx = ui->nresults > 0 ? ui->results[ui->cursor] : -1;
    if (idx < 0) return;
    /* The selected items, space separated, for the {+} and {f} placeholders. */
    char *sel = NULL;
    size_t sel_len = 0;
    if (ui->selected) {
        for (int i = 0; i < ui->count; i++) {
            if (!ui->selected[i]) continue;
            size_t l = strlen(ui->lines[i]);
            char *ns = realloc(sel, sel_len + l + 2);
            if (!ns) break;
            sel = ns;
            if (sel_len) sel[sel_len++] = ' ';
            memcpy(sel + sel_len, ui->lines[i], l);
            sel_len += l;
            sel[sel_len] = 0;
        }
    }
    mm_preview_set_selected(ui->preview, sel ? sel : "");
    free(sel);
    mm_preview_start(ui->preview, ui->lines[idx], ui->query_buf, idx);
}


/* --- selection output --- */
/* Mark an item as selected and remember the order of the selection. */
static void ui_select_item(mm_ui_t *ui, int item) {
    if (!ui->selected || ui->selected[item]) return;
    ui->selected[item] = true;
    ui->nselected++;
    if (ui->sel_order_n + 1 > ui->sel_order_cap) {
        int ncap = ui->sel_order_cap ? ui->sel_order_cap * 2 : 16;
        int *n = realloc(ui->sel_order, sizeof(int) * (size_t)ncap);
        if (!n) return;
        ui->sel_order = n;
        ui->sel_order_cap = ncap;
    }
    ui->sel_order[ui->sel_order_n++] = item;
}

/* Unmark an item as selected. */
static void ui_deselect_item(mm_ui_t *ui, int item) {
    if (!ui->selected || !ui->selected[item]) return;
    ui->selected[item] = false;
    ui->nselected--;
    for (int i = 0; i < ui->sel_order_n; i++) {
        if (ui->sel_order[i] == item) {
            memmove(ui->sel_order + i, ui->sel_order + i + 1,
                    sizeof(int) * (size_t)(ui->sel_order_n - i - 1));
            ui->sel_order_n--;
            break;
        }
    }
}

/* Unmark every item. */
static void ui_clear_selection(mm_ui_t *ui) {
    if (ui->selected) memset(ui->selected, 0, (size_t)ui->count * sizeof(bool));
    ui->nselected = 0;
    ui->sel_order_n = 0;
}

/* The direction of the list navigation.  fzf moves the cursor towards the
   end of the list with "up" and towards its beginning with "down" in the
   default layout; the reverse layouts flip the direction. */
static int ui_nav_step(const mm_ui_t *ui, int up) {
    int step = up ? 1 : -1;
    if (ui->reverse || ui->layout == 1) step = -step;
    return step;
}

static bool ui_print_selection(mm_ui_t *ui, FILE *out) {
    char sep = ui->print0 ? '\0' : '\n';
    if (ui->nselected > 0 && ui->sel_order) {
        /* The selected items are printed in the order they were selected. */
        for (int i = 0; i < ui->sel_order_n; i++) {
            int item = ui->sel_order[i];
            if (item < 0 || item >= ui->count) continue;
            fputs(ui->lines[item], out);
            fputc(sep, out);
        }
        return true;
    } else if (ui->nresults > 0) {
        /* Nothing is selected: the current item is printed. */
        int item_idx = ui->results[ui->cursor];
        fputs(ui->lines[item_idx], out);
        fputc(sep, out);
        return true;
    }
    return false;
}

/* Replace the fzf placeholders in a command string: {} (current item),
   {q} (query), {n} (index of the current item). Returns a malloc'd string. */
static char *ui_expand_command(mm_ui_t *ui, const char *cmd) {
    size_t cap = strlen(cmd) * 2 + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    int idx = ui->nresults > 0 ? ui->results[ui->cursor] : -1;
    for (size_t i = 0; cmd[i]; i++) {
        if (cmd[i] == '{' && cmd[i + 1] == '}') {
            const char *rep = idx >= 0 ? ui->lines[idx] : "";
            size_t rl = strlen(rep);
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, rep, rl);
            o += rl;
            i++;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'q' && cmd[i + 2] == '}') {
            size_t rl = ui->query_len;
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, ui->query_buf, rl);
            o += rl;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'n' && cmd[i + 2] == '}') {
            char num[32];
            int n = idx >= 0 ? idx : 0;
            int nl = snprintf(num, sizeof(num), "%d", n);
            while (o + (size_t)nl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, num, (size_t)nl);
            o += (size_t)nl;
            i += 2;
            continue;
        }
        if (o + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[o++] = cmd[i];
    }
    out[o] = 0;
    return out;
}

/* Run a command with the system shell, waiting for it to finish. */
static void ui_run_command(mm_ui_t *ui, const char *cmd, bool silent) {
    if (!cmd || !*cmd) return;
    char *expanded = ui_expand_command(ui, cmd);
    if (!expanded) return;
    if (!silent) {
        /* Leave the alternate screen so the command can interact with the
           terminal, then come back. */
        mm_renderer_close(ui->renderer);
    }
    pid_t pid = fork();
    if (pid == 0) {
        if (ui->shell && *ui->shell)
            execl(ui->shell, ui->shell, "-c", expanded, (char *)NULL);
        else
            execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    } else if (pid > 0) {
        int status = 0;
        while (waitpid(pid, &status, 0) < 0) { }
    }
    free(expanded);
    if (!silent) {
        /* Re-enter the alternate screen and repaint. */
        mm_renderer_init(ui->renderer, ui->renderer->ttyin, ui->renderer->ttyout,
                         false, 8, true, true);
    }
}

/* Execute one action. Returns false when the action should end the loop. */
static bool ui_execute_action(mm_ui_t *ui, const mm_binding_t *b) {
    switch (b->action) {
    case MM_ACT_ACCEPT:
        return false;   /* leave the loop */
    case MM_ACT_ACCEPT_NON_EMPTY:
        /* Accept only if the query is non-empty. */
        if (ui->query_len > 0) return false;
        break;
    case MM_ACT_ACCEPT_OR_PRINT_QUERY:
        /* Accept, or print the query when there is no selection. */
        if (ui->nresults > 0) return false;
        ui->print_query = true;
        return false;
    case MM_ACT_ABORT:
        ui->exit_code = 130;
        ui->aborted = true;
        return false;
    case MM_ACT_CANCEL:
        /* Clear the query, or abort when it is already empty. */
        if (ui->query_len > 0) {
            query_set(ui, "");
            ui_search(ui);
        } else {
            ui->exit_code = 130;
            ui->aborted = true;
            return false;
        }
        break;
    case MM_ACT_CLEAR_SELECTION:
        ui_clear_selection(ui);
        break;
    case MM_ACT_CLEAR_SCREEN:
        if (ui->renderer) mm_renderer_clear(ui->renderer);
        break;
    case MM_ACT_CLOSE:
        /* Close the preview window. */
        if (ui->preview) {
            mm_preview_free(ui->preview);
            ui->preview = NULL;
        }
        break;
    case MM_ACT_UP_MATCH:
    case MM_ACT_DOWN_MATCH:
        /* Only the matching items are listed, so this is a plain move. */
        if (ui->nresults > 0) {
            int step = ui_nav_step(ui, b->action == MM_ACT_UP_MATCH);
            if (ui->cycle) {
                int n = ui->nresults;
                ui->cursor = ((ui->cursor + step) % n + n) % n;
            } else {
                int next = ui->cursor + step;
                if (next >= 0 && next < ui->nresults) ui->cursor = next;
            }
        }
        break;
    case MM_ACT_UP_SELECTED:
    case MM_ACT_DOWN_SELECTED:
        /* Move to the previous or next selected item. */
        if (ui->selected && ui->nselected > 0) {
            /* "up-selected" moves to the previous selected item,
               "down-selected" to the next one. */
            int step = ui_nav_step(ui, b->action == MM_ACT_UP_SELECTED ? 0 : 1);
            int i = ui->cursor + step;
            while (i >= 0 && i < ui->nresults) {
                if (ui->selected[ui->results[i]]) { ui->cursor = i; break; }
                i += step;
            }
        }
        break;
    case MM_ACT_TOGGLE_DOWN:
    case MM_ACT_TOGGLE_UP: {
        /* Toggle the current item and move on. */
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            int limit = ui->multi ? ui->multi_max : 1;
            if (ui->selected[item]) {
                ui_deselect_item(ui, item);
            } else if (limit <= 0 || ui->nselected < limit) {
                if (limit == 1) ui_clear_selection(ui);
                ui_select_item(ui, item);
            }
            int step = ui_nav_step(ui, b->action == MM_ACT_TOGGLE_UP);
            int next = ui->cursor + step;
            if (next >= 0 && next < ui->nresults) ui->cursor = next;
        }
        break;
    }
    case MM_ACT_DESELECT_DOWN:
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            ui_deselect_item(ui, item);
            int next = ui->cursor - 1;
            if (next >= 0) ui->cursor = next;
        }
        break;
    case MM_ACT_REFRESH_PREVIEW:
        if (ui->preview) ui_preview_restart(ui);
        break;
    case MM_ACT_CHANGE_POINTER:
        if (b->arg) ui->pointer = b->arg;
        break;
    case MM_ACT_CLEAR_QUERY:
        query_set(ui, "");
        ui_search(ui);
        break;
    /* The list is drawn bottom-up: the first match sits on the bottom line.
       "up" therefore moves towards the end of the list and "down" towards
       its beginning, which is what the reference does. */
    case MM_ACT_UP:
    case MM_ACT_DOWN: {
        /* "down" moves towards the beginning of the list, "up" towards its
           end; the reverse layouts flip the direction. */
        int step = ui_nav_step(ui, b->action == MM_ACT_UP);
        if (ui->nresults > 0) {
            if (ui->cycle) {
                ui->cursor = ((ui->cursor + step) % ui->nresults + ui->nresults) % ui->nresults;
            } else {
                int next = ui->cursor + step;
                if (next >= 0 && next < ui->nresults) ui->cursor = next;
            }
        }
        break;
    }
    case MM_ACT_FIRST:
        ui->cursor = 0;
        break;
    case MM_ACT_LAST:
        if (ui->nresults > 0) ui->cursor = ui->nresults - 1;
        break;
    case MM_ACT_PAGE_UP:
    case MM_ACT_PAGE_DOWN:
    case MM_ACT_HALF_PAGE_UP:
    case MM_ACT_HALF_PAGE_DOWN: {
        /* "page-up" moves towards the end of the list, "page-down" towards
           its beginning; the reverse layouts flip the direction. */
        int lines = (b->action == MM_ACT_HALF_PAGE_UP ||
                     b->action == MM_ACT_HALF_PAGE_DOWN)
                        ? ui->page_size / 2 : ui->page_size;
        if (lines < 1) lines = 1;
        int step = ui_nav_step(ui, b->action == MM_ACT_PAGE_UP ||
                                     b->action == MM_ACT_HALF_PAGE_UP);
        int next = ui->cursor + step * lines;
        if (next < 0) next = 0;
        if (ui->nresults > 0 && next >= ui->nresults) next = ui->nresults - 1;
        ui->cursor = next;
        break;
    }
    case MM_ACT_TOGGLE:
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            /* Without --multi only one item can be selected at a time, but
               the toggle still works (fzf's behaviour). */
            int limit = ui->multi ? ui->multi_max : 1;
            if (ui->selected[item]) {
                ui_deselect_item(ui, item);
            } else if (limit <= 0 || ui->nselected < limit) {
                if (limit == 1) ui_clear_selection(ui);
                ui_select_item(ui, item);
            }
        }
        break;
    case MM_ACT_TOGGLE_ALL:
        if (ui->multi) {
            for (int i = 0; i < ui->nresults; i++) {
                int item = ui->results[i];
                if (ui->selected[item]) ui_deselect_item(ui, item);
                else if (ui->multi_max <= 0 || ui->nselected < ui->multi_max)
                    ui_select_item(ui, item);
            }
        }
        break;
    case MM_ACT_SELECT_ALL:
        if (ui->multi) {
            for (int i = 0; i < ui->nresults; i++) {
                int item = ui->results[i];
                if (!ui->selected[item] &&
                    (ui->multi_max <= 0 || ui->nselected < ui->multi_max)) {
                    ui_select_item(ui, item);
                }
            }
        }
        break;
    case MM_ACT_DESELECT_ALL:
        ui_clear_selection(ui);
        break;
    case MM_ACT_DESELECT:
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            ui_deselect_item(ui, item);
        }
        break;
    case MM_ACT_REFRESH:
        ui_search(ui);
        break;
    case MM_ACT_BELL:
        mm_renderer_bell(ui->renderer);
        break;
    case MM_ACT_PRINT_QUERY:
        ui->print_query = true;
        break;
    case MM_ACT_BACKWARD_DELETE_CHAR:
        query_backspace(ui);
        ui_search(ui);
        break;
    case MM_ACT_DELETE_CHAR:
        query_delete(ui);
        ui_search(ui);
        break;
    case MM_ACT_KILL_LINE:
        query_kill_after_cursor(ui);
        ui_search(ui);
        break;
    case MM_ACT_KILL_WORD:
        while ((size_t)ui->query_pos < ui->query_len &&
               ui->query_buf[ui->query_pos] == ' ')
            query_delete(ui);
        while ((size_t)ui->query_pos < ui->query_len &&
               ui->query_buf[ui->query_pos] != ' ')
            query_delete(ui);
        ui_search(ui);
        break;
    case MM_ACT_BACKWARD_KILL_WORD:
    case MM_ACT_UNIX_WORD_RUBOUT:
        while (ui->query_pos > 0 &&
               ui->query_buf[ui->query_pos - 1] == ' ')
            query_backspace(ui);
        while (ui->query_pos > 0 &&
               ui->query_buf[ui->query_pos - 1] != ' ')
            query_backspace(ui);
        ui_search(ui);
        break;
    case MM_ACT_EXECUTE:
    case MM_ACT_EXECUTE_MULTI:
        if (b->arg) ui_run_command(ui, b->arg, false);
        break;
    case MM_ACT_EXECUTE_SILENT:
        if (b->arg) ui_run_command(ui, b->arg, true);
        break;
    case MM_ACT_CHANGE_QUERY:
        if (b->arg) { query_set(ui, b->arg); ui_search(ui); }
        break;
    case MM_ACT_CHANGE_PROMPT:
        if (b->arg) ui->prompt = b->arg;
        break;
    case MM_ACT_CHANGE_HEADER:
        if (b->arg) ui->header = b->arg;
        break;
    case MM_ACT_CHANGE_MULTI:
        if (b->arg) {
            int m = atoi(b->arg);
            ui->multi = m != 0;
            ui->multi_max = m > 0 ? m : 0;
        }
        break;
    case MM_ACT_PREV_HISTORY:
        if (ui->history) {
            const char *q = mm_history_prev(ui->history);
            if (q) { query_set(ui, q); ui_search(ui); }
        }
        break;
    case MM_ACT_NEXT_HISTORY:
        if (ui->history) {
            const char *q = mm_history_next(ui->history);
            if (q) { query_set(ui, q); ui_search(ui); }
        }
        break;
    case MM_ACT_RELOAD:
        /* Reload the list from the output of the given command. */
        if (b->arg && *b->arg) ui_reload(ui, b->arg);
        break;
    case MM_ACT_PRINT:
        /* Print the given string to stdout. */
        if (b->arg) {
            fputs(b->arg, stdout);
            fputc('\n', stdout);
            fflush(stdout);
        }
        break;
    case MM_ACT_BECOME:
        /* Replace this process with the given command. */
        if (b->arg && *b->arg) {
            fflush(stdout);
            fflush(stderr);
            execl("/bin/sh", "sh", "-c", b->arg, (char *)NULL);
            /* The exec failed: report it and keep running. */
            fprintf(stderr, "fzf: failed to execute %s\n", b->arg);
        }
        break;
    case MM_ACT_IGNORE:
        break;
    case MM_ACT_CLEAR_MULTI:
        memset(ui->selected, 0, (size_t)ui->count * sizeof(bool));
        ui->nselected = 0;
        break;
    case MM_ACT_HIDE_HEADER:
        ui->header = NULL;
        break;
    case MM_ACT_HIDE_PREVIEW:
        if (ui->preview) {
            mm_preview_free(ui->preview);
            ui->preview = NULL;
        }
        break;
    case MM_ACT_OFFSET_DOWN:
        if (ui->nresults > 0) {
            int step = b->arg ? atoi(b->arg) : 1;
            ui->cursor += step;
            if (ui->cursor >= ui->nresults) ui->cursor = ui->nresults - 1;
        }
        break;
    case MM_ACT_OFFSET_UP:
        if (ui->nresults > 0) {
            int step = b->arg ? atoi(b->arg) : 1;
            ui->cursor -= step;
            if (ui->cursor < 0) ui->cursor = 0;
        }
        break;
    case MM_ACT_OFFSET_MIDDLE:
        if (ui->nresults > 0) ui->cursor = ui->nresults / 2;
        break;
    case MM_ACT_POS:
        if (b->arg) {
            int p = atoi(b->arg) - 1;
            if (p >= 0 && p < ui->nresults) ui->cursor = p;
        }
        break;
    case MM_ACT_REPLACE_QUERY:
        if (b->arg) { query_set(ui, b->arg); ui_search(ui); }
        break;
    case MM_ACT_TOGGLE_HEADER:
        ui->header = ui->header ? NULL : "> ";
        break;
    case MM_ACT_TOGGLE_HSCROLL:
        ui->hscroll_off = ui->hscroll_off ? 0 : 1;
        break;
    case MM_ACT_TOGGLE_PREVIEW:
        if (ui->preview) {
            mm_preview_free(ui->preview);
            ui->preview = NULL;
        }
        break;
    case MM_ACT_TOGGLE_PREVIEW_WRAP:
        if (ui->preview) ui->preview_win.wrap = !ui->preview_win.wrap;
        break;
    case MM_ACT_TOGGLE_PREVIEW_WRAP_WORD:
        if (ui->preview) ui->preview_win.wrap = !ui->preview_win.wrap;
        break;
    case MM_ACT_CHANGE_PREVIEW:
        if (b->arg && ui->preview) {
            mm_preview_set_command(ui->preview, b->arg);
            ui_preview_restart(ui);
        }
        break;
    case MM_ACT_CHANGE_PREVIEW_WINDOW:
        if (b->arg && ui->preview) {
            /* Change the preview window options. */
            mm_preview_window_parse(b->arg, &ui->preview_win);
        }
        break;
    case MM_ACT_PREVIEW:
        /* Toggle the preview window for the current item. */
        if (ui->preview) {
            mm_preview_free(ui->preview);
            ui->preview = NULL;
        } else {
            /* Preview would need a command; no-op without one. */
        }
        break;
    case MM_ACT_TOGGLE_RAW:
        ui->ansi = !ui->ansi;
        break;
    case MM_ACT_TOGGLE_SEARCH:
        break;
    case MM_ACT_TOGGLE_SORT:
        ui->no_sort = !ui->no_sort;
        ui_search(ui);
        break;
    case MM_ACT_TOGGLE_TRACK:
        break;
    case MM_ACT_TOGGLE_TRACK_CURRENT:
        break;
    case MM_ACT_TOGGLE_WRAP:
        ui->wrap = !ui->wrap;
        break;
    case MM_ACT_TOGGLE_WRAP_WORD:
        ui->wrap_word = !ui->wrap_word;
        break;
    case MM_ACT_TRACK:
        break;
    case MM_ACT_UNBIND:
        break;
    case MM_ACT_UNIX_LINE_DISCARD:
        query_set(ui, "");
        ui_search(ui);
        break;
    case MM_ACT_UNTRACK_CURRENT:
        break;
    case MM_ACT_WAIT:
        break;
    case MM_ACT_YANK:
        break;
    default:
        /* Actions that need features this port does not implement yet are
           accepted and ignored rather than silently mis-executed. */
        break;
    }
    return true;
}
int mm_ui_run(mm_ui_t *ui) {
    int ttyin = open("/dev/tty", O_RDONLY);
    int ttyout = open("/dev/tty", O_WRONLY);
    if (ttyin < 0 || ttyout < 0) {
        fprintf(stderr, "fzf: failed to open /dev/tty\n");
        if (ttyin >= 0) close(ttyin);
        if (ttyout >= 0) close(ttyout);
        return 2;
    }

    mm_renderer_t r;
    /* With --height the UI is drawn inside the terminal instead of using the
       alternate screen. */
    int total_rows = 0;
    {
        struct winsize ws;
        if (ioctl(ttyout, TIOCGWINSZ, &ws) == 0 && ws.ws_row > 0) total_rows = ws.ws_row;
        else total_rows = 24;
    }
    int win_height = ui->height_spec ? mm_ui_height(ui->height_spec, total_rows,
                                                ui->min_height) : 0;
    if (!mm_renderer_init(&r, ttyin, ttyout, false, ui->tabstop, !ui->no_clear, win_height == 0)) {
        fprintf(stderr, "fzf: failed to initialize terminal\n");
        close(ttyin); close(ttyout);
        return 2;
    }
    r.colored = true;
    if (win_height > 0) {
        r.inline_mode = true;
        r.inline_height = win_height;
        ui->height = win_height;
    }

    /* Trigger the "start" event. */
    ui_exec_event(ui, "start");

    mm_keybuf_t input;
    mm_keybuf_init(&input);
    ui->renderer = &r;
    ui->page_size = mm_renderer_max_y(&r) > 3 ? mm_renderer_max_y(&r) - 3 : 1;

    int total = mm_renderer_max_y(&r);
    mm_window_t listwin, promptwin, infowin;
    mm_window_init(&listwin, &r, 0, 0, mm_renderer_max_x(&r),
                   total > 0 ? total : 1, MM_WIN_LIST, MM_BORDER_NONE, false);
    mm_window_init(&promptwin, &r, 0, 0, mm_renderer_max_x(&r), 1,
                   MM_WIN_INPUT, MM_BORDER_NONE, false);
    mm_window_init(&infowin, &r, 0, 0, mm_renderer_max_x(&r), 1,
                   MM_WIN_INPUT, MM_BORDER_NONE, false);

    /* The items that are already available are shown right away; the rest
       arrives while the program is running (a live stream). */
    if (mm_ui_read_items(ui, ttyin)) ui_search(ui);
    ui_search(ui);
    ui_render(ui, &r, &listwin, &promptwin, &infowin);

    /* Trigger the "load" event after the initial search. */
    ui_exec_event(ui, "load");

    bool running = true;
    if (ui->select_1 && ui->nresults == 1) running = false;
    int exit_code = 0;
    if (ui->preview) ui_preview_restart(ui);

    while (running) {
        mm_event_t ev;
        /* A cancellable read lets us poll the preview command while waiting
           for the next key press. */
        if (!mm_read_event(ttyin, &input, &ev, true)) break;
        if (ev.type == MM_EV_INVALID) {
            /* Timed out: read the items that arrived in the meantime and
               give the preview a chance to produce output. */
            bool more = mm_ui_read_items(ui, ttyin);
            if (more) { ui_search(ui); }
            if ((more || (ui->preview && mm_preview_poll(ui->preview))) && running) {
                ui_render(ui, &r, &listwin, &promptwin, &infowin);
            }
            continue;
        }
        {
            const char *exp = ui_expect_key(ui, &ev);
            if (exp) {
                ui->expected = exp;
                running = false;
                break;
            }
        }
        /* User bindings take precedence over the built-in behaviour. */
        {
            const mm_binding_t *found[16];
            int nf = mm_bind_lookup(&ui->keymap, &ev, found, 16);
            if (nf > 0) {
                bool cont = true;
                for (int i = 0; i < nf && cont; i++)
                    cont = ui_execute_action(ui, found[i]);
                if (!cont) { running = false; break; }
                if (ui->preview) ui_preview_restart(ui);
                if (running) ui_render(ui, &r, &listwin, &promptwin, &infowin);
                continue;
            }
        }
        switch (ev.type) {
        case MM_EV_CTRL_C:
        case MM_EV_CTRL_G:
        case MM_EV_CTRL_Q:
        case MM_EV_ESC:
            running = false;
            break;
        case MM_EV_ENTER:
            running = false;
            break;
        case MM_EV_TAB:
            /* The tab toggles the current item and moves on (fzf's
               default binding). */
            if (ui->multi && ui->nresults > 0) {
                int item_idx = ui->results[ui->cursor];
                if (!ui->selected[item_idx]) {
                    if (ui->multi_max <= 0 || ui->nselected < ui->multi_max) {
                        ui_select_item(ui, item_idx);
                    }
                } else {
                    ui_deselect_item(ui, item_idx);
                }
                int step = ui_nav_step(ui, 0);
                int next = ui->cursor + step;
                if (next >= 0 && next < ui->nresults) ui->cursor = next;
            }
            break;
        case MM_EV_DOWN:
        case MM_EV_CTRL_N:
            if (ui->nresults > 0) {
                int step = ui_nav_step(ui, 0);
                if (ui->cycle) {
                    ui->cursor = ((ui->cursor + step) % ui->nresults + ui->nresults) % ui->nresults;
                } else {
                    int next = ui->cursor + step;
                    if (next >= 0 && next < ui->nresults) ui->cursor = next;
                }
            }
            break;
        case MM_EV_UP:
        case MM_EV_CTRL_P:
            if (ui->nresults > 0) {
                int step = ui_nav_step(ui, 1);
                if (ui->cycle) {
                    ui->cursor = ((ui->cursor + step) % ui->nresults + ui->nresults) % ui->nresults;
                } else {
                    int next = ui->cursor + step;
                    if (next >= 0 && next < ui->nresults) ui->cursor = next;
                }
            }
            break;
        case MM_EV_CTRL_LEFT:
        case MM_EV_ALT_LEFT:
            while (ui->query_pos > 0 &&
                   ui->query_buf[ui->query_pos - 1] == ' ')
                ui->query_pos--;
            while (ui->query_pos > 0 &&
                   ui->query_buf[ui->query_pos - 1] != ' ')
                ui->query_pos--;
            while (ui->query_pos > 0 &&
                   ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                ui->query_pos--;
            break;
        case MM_EV_CTRL_RIGHT:
        case MM_EV_ALT_RIGHT:
            while ((size_t)ui->query_pos < ui->query_len &&
                   ui->query_buf[ui->query_pos] != ' ')
                ui->query_pos++;
            while ((size_t)ui->query_pos < ui->query_len &&
                   ui->query_buf[ui->query_pos] == ' ')
                ui->query_pos++;
            while ((size_t)ui->query_pos < ui->query_len &&
                   ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                ui->query_pos++;
            break;
        case MM_EV_LEFT:
        case MM_EV_CTRL_B:
            if (ui->query_pos > 0) {
                ui->query_pos--;
                while (ui->query_pos > 0 &&
                       ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                    ui->query_pos--;
            }
            break;
        case MM_EV_RIGHT:
        case MM_EV_CTRL_F:
            if ((size_t)ui->query_pos < ui->query_len) {
                ui->query_pos++;
                while ((size_t)ui->query_pos < ui->query_len &&
                       ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                    ui->query_pos++;
            }
            break;
        case MM_EV_HOME:
        case MM_EV_CTRL_A:
            ui->query_pos = 0;
            break;
        case MM_EV_END:
        case MM_EV_CTRL_E:
            ui->query_pos = (int)ui->query_len;
            break;
        case MM_EV_CTRL_BACKSPACE:
        case MM_EV_ALT_BACKSPACE:
            while (ui->query_pos > 0 &&
                   ui->query_buf[ui->query_pos - 1] == ' ')
                query_backspace(ui);
            while (ui->query_pos > 0 &&
                   ui->query_buf[ui->query_pos - 1] != ' ')
                query_backspace(ui);
            ui_search(ui);
            break;
        case MM_EV_CTRL_DELETE:
        case MM_EV_ALT_DELETE:
            while ((size_t)ui->query_pos < ui->query_len &&
                   ui->query_buf[ui->query_pos] == ' ')
                query_delete(ui);
            while ((size_t)ui->query_pos < ui->query_len &&
                   ui->query_buf[ui->query_pos] != ' ')
                query_delete(ui);
            ui_search(ui);
            break;
        case MM_EV_BACKSPACE:
        case MM_EV_CTRL_H:
            query_backspace(ui);
            ui_search(ui);
            break;
        case MM_EV_DELETE:
            query_delete(ui);
            ui_search(ui);
            break;
        case MM_EV_CTRL_U:
            query_kill_before_cursor(ui);
            ui_search(ui);
            break;
        case MM_EV_CTRL_K:
            query_kill_after_cursor(ui);
            ui_search(ui);
            break;
        case MM_EV_CTRL_W:
            /* Delete the previous word. */
            while (ui->query_pos > 0 && ui->query_buf[ui->query_pos - 1] == ' ')
                query_backspace(ui);
            while (ui->query_pos > 0 && ui->query_buf[ui->query_pos - 1] != ' ')
                query_backspace(ui);
            ui_search(ui);
            break;
        case MM_EV_RUNE: {
            char buf[8];
            size_t n = 0;
            wchar_t w = ev.rune;
            if (w < 0x80) { buf[n++] = (char)w; }
            else if (w < 0x800) {
                buf[n++] = (char)(0xC0 | (w >> 6));
                buf[n++] = (char)(0x80 | (w & 0x3F));
            } else if (w < 0x10000) {
                buf[n++] = (char)(0xE0 | (w >> 12));
                buf[n++] = (char)(0x80 | ((w >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (w & 0x3F));
            } else {
                buf[n++] = (char)(0xF0 | (w >> 18));
                buf[n++] = (char)(0x80 | ((w >> 12) & 0x3F));
                buf[n++] = (char)(0x80 | ((w >> 6) & 0x3F));
                buf[n++] = (char)(0x80 | (w & 0x3F));
            }
            query_insert(ui, buf, n);
            ui_search(ui);
            break;
        }
        default:
            break;
        }
        if (ui->preview) ui_preview_restart(ui);
        if (running) ui_render(ui, &r, &listwin, &promptwin, &infowin);
    }

    /* Record the query in the history file. */
    if (ui->history) mm_history_append(ui->history, ui->query_buf);

    /* Leave the alternate screen before writing the selection so that the
       output lands on the user's terminal, not in the discarded buffer. */
    mm_keybuf_free(&input);
    mm_renderer_close(&r);
    close(ttyin);
    close(ttyout);

    if (ui->aborted) {
        /* An aborted session prints nothing and exits with 130. */
        exit_code = 130;
        return exit_code;
    }
    if (ui->print_query) {
        char sep = ui->print0 ? '\0' : '\n';
        fputs(ui->query_buf, stdout);
        fputc(sep, stdout);
    }
    if (ui->expected) {
        char sep = ui->print0 ? '\0' : '\n';
        fputs(ui->expected, stdout);
        fputc(sep, stdout);
    }
    bool found = ui_print_selection(ui, stdout);
    fflush(stdout);

    /* fzf exits with 1 when nothing was printed (no match), unless
       --exit-0 is given. */
    if (ui->exit_0 && !found) exit_code = 0;
    else exit_code = found ? 0 : 1;
    return exit_code;
}

/* Make room for more items in the list. */
static bool ui_grow(mm_ui_t *ui, int need) {
    if (need < 0 || ui->count > INT_MAX - need) return false;
    int required = ui->count + need;
    if (required <= ui->lines_cap) return true;
    int ncap = ui->lines_cap > 0 ? ui->lines_cap : 1024;
    while (ncap < required) {
        if (ncap > INT_MAX / 2) {
            ncap = required;
            break;
        }
        ncap *= 2;
    }
    if ((size_t)ncap > SIZE_MAX / sizeof(char *) ||
        (size_t)ncap > SIZE_MAX / sizeof(mm_item_t) ||
        (size_t)ncap > SIZE_MAX / sizeof(bool))
        return false;
    char **nl = realloc(ui->lines, sizeof(*nl) * (size_t)ncap);
    if (!nl) return false;
    ui->lines = nl;
    mm_item_t *ni = realloc(ui->items, sizeof(*ni) * (size_t)ncap);
    if (!ni) return false;
    ui->items = ni;
    bool *nsel = realloc(ui->selected, sizeof(*nsel) * (size_t)ncap);
    if (!nsel) return false;
    ui->selected = nsel;
    ui->lines_cap = ncap;
    return true;
}

/* Append one item to the list. */
static void ui_add_line(mm_ui_t *ui, char *line) {
    if (!ui_grow(ui, 1)) { free(line); return; }
    int idx = ui->count;
    ui->lines[idx] = line;
    mm_item_init(&ui->items[idx], line, idx);
    if (!mchars_decode(&ui->items[idx].text)) { free(line); return; }
    ui->selected[idx] = false;
    ui->count++;
}

/* Read the items that are available on the input and append them to the
   list.  The reference reads the input in the background, so that a live
   stream shows up while the program is running. */
bool mm_ui_read_items(mm_ui_t *ui, int ttyin) {
    if (ui->input_fd < 0 || ui->input_done) return false;
    bool added = false;
    bool finished = false;
    char buf[131072];
    for (;;) {
        ssize_t n = read(ui->input_fd, buf, sizeof(buf));
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            ui->input_done = true;
            finished = true;
            break;
        }
        if (n == 0) { ui->input_done = true; finished = true; break; }
        if ((size_t)n > SIZE_MAX - ui->pending_len - 1) {
            ui->input_done = true;
            break;
        }
        size_t required = ui->pending_len + (size_t)n + 1;
        if (required > ui->pending_cap) {
            size_t ncap = ui->pending_cap ? ui->pending_cap : 65536;
            while (ncap < required) {
                if (ncap > SIZE_MAX / 2) {
                    ncap = required;
                    break;
                }
                ncap *= 2;
            }
            char *np = realloc(ui->pending, ncap);
            if (!np) { ui->input_done = true; break; }
            ui->pending = np;
            ui->pending_cap = ncap;
        }
        memcpy(ui->pending + ui->pending_len, buf, (size_t)n);
        ui->pending_len += (size_t)n;
        /* Split the complete lines. */
        size_t start = 0;
        for (size_t i = 0; i < ui->pending_len; i++) {
            if (ui->pending[i] != ui->input_sep) continue;
            size_t llen = i - start;
            if (ui->input_sep == '\n' && llen > 0 && ui->pending[start + llen - 1] == '\r')
                llen--;
            char *line = malloc(llen + 1);
            if (!line) break;
            memcpy(line, ui->pending + start, llen);
            line[llen] = 0;
            ui_add_line(ui, line);
            added = true;
            start = i + 1;
        }
        if (start > 0) {
            memmove(ui->pending, ui->pending + start, ui->pending_len - start);
            ui->pending_len -= start;
        }
        /* A continuously-writing producer can keep the pipe readable forever.
           Stop draining when a key is pending so the event loop can handle it
           before consuming the entire stream. With no key pending, continue
           draining normally and avoid repeated search/render passes. */
        if (ttyin >= 0) {
            struct pollfd pfd = { .fd = ttyin, .events = POLLIN };
            int ready;
            do {
                ready = poll(&pfd, 1, 0);
            } while (ready < 0 && errno == EINTR);
            if (ready > 0 && (pfd.revents & POLLIN)) break;
        }
    }
    /* The last line of the stream is not terminated. */
    if (ui->input_done && ui->pending_len > 0) {
        size_t llen = ui->pending_len;
        if (ui->input_sep == '\n' && llen > 0 && ui->pending[llen - 1] == '\r') llen--;
        char *line = malloc(llen + 1);
        if (line) {
            memcpy(line, ui->pending, llen);
            line[llen] = 0;
            ui_add_line(ui, line);
            added = true;
        }
        ui->pending_len = 0;
    }
    /* Report the end of the stream as a change so that the spinner is
       cleared even when the last read did not add an item. */
    return added || finished;
}

bool mm_ui_init(mm_ui_t *ui, char **lines, int count, int cap) {
    ui->lines = lines;
    ui->count = 0;
    ui->lines_cap = cap;
    /* The arrays are allocated with the full capacity, so that the items
       can be appended while the program is running. */
    size_t alloc = (size_t)(cap ? cap : (count ? count : 1));
    ui->items = calloc(alloc, sizeof(mm_item_t));
    if (!ui->items) return false;
    for (int i = 0; i < count; i++) {
        mm_item_init(&ui->items[i], lines[i], i);
        if (!mchars_decode(&ui->items[i].text)) {
            ui->count = i;
            return false;
        }
    }
    ui->selected = calloc(alloc, sizeof(bool));
    if (!ui->selected) return false;
    ui->count = count;
    query_set(ui, ui->query ? ui->query : "");
    ui->cursor = 0;
    ui->offset = 0;
    ui->nresults = 0;
    ui->results = NULL;
    return true;
}

void mm_ui_free(mm_ui_t *ui) {
    if (ui->items)
        for (int i = 0; i < ui->count; i++) mm_item_free(&ui->items[i]);
    if (ui->lines) {
        for (int i = 0; i < ui->count; i++) free(ui->lines[i]);
        free(ui->lines);
        ui->lines = NULL;
    }
    free(ui->items);
    free(ui->selected);
    free(ui->results);
    free(ui->search_results);
    free(ui->query_buf);
    mm_delimiter_free(&ui->delim);
    mm_keymap_free(&ui->keymap);
    mm_event_map_free(&ui->events);
    mm_preview_free(ui->preview);
    ui->preview = NULL;
    mm_history_free(ui->history);
    ui->history = NULL;
    for (int i = 0; i < ui->expect_n; i++) free(ui->expect_keys[i]);
    free(ui->expect_keys);
    ui->expect_keys = NULL;
    ui->expect_n = 0;
    ui->items = NULL;
    ui->selected = NULL;
    ui->results = NULL;
    ui->query_buf = NULL;
}
