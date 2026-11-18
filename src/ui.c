/* ui.c - interactive fuzzy finder loop.
   Renders a prompt plus a scrollable list of matches and handles the
   core key bindings (movement, editing, selection, multi-select). */
#define _POSIX_C_SOURCE 200809L
#include "ui.h"
#include "terminal.h"
#include "tokenizer.h"
#include "event.h"
#include "util.h"

#include <errno.h>
#include <ctype.h>
#include <regex.h>
#include <wctype.h>
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
#define MM_DEFAULT_JUMP_LABELS "asdfghjklqwertyuiopzxcvbnm1234567890ASDFGHJKLQWERTYUIOPZXCVBNM`~;:,<.>/?'\"!@#$%^&*()[{]}-_=+"

static volatile sig_atomic_t ui_resize_pending;

/* Paint the first items immediately, then at most once per frame while
   more of the list is still arriving. */
#define MM_PAINT_MS 16

typedef struct mm_line_slab {
    struct mm_line_slab *next;
    size_t used;
    size_t cap;
    char data[];
} mm_line_slab;

static char *ui_slab_alloc(mm_ui_t *ui, size_t n) {
    mm_line_slab *slab = ui->line_slabs;
    if (!slab || n > slab->cap || slab->used > slab->cap - n) {
        size_t cap = (size_t)1 << 20;
        if (n > cap) cap = n;
        if (cap > SIZE_MAX - sizeof(mm_line_slab)) return NULL;
        slab = malloc(sizeof(mm_line_slab) + cap);
        if (!slab) return NULL;
        slab->next = ui->line_slabs;
        slab->used = 0;
        slab->cap = cap;
        ui->line_slabs = slab;
    }
    char *out = slab->data + slab->used;
    slab->used += n;
    return out;
}

static void ui_free_slabs(mm_ui_t *ui) {
    mm_line_slab *slab = ui->line_slabs;
    while (slab) {
        mm_line_slab *next = slab->next;
        free(slab);
        slab = next;
    }
    ui->line_slabs = NULL;
}

typedef struct mm_rune_slab {
    struct mm_rune_slab *next;
    size_t used;
    size_t cap;
    wchar_t data[];
} mm_rune_slab;

static wchar_t *ui_rune_alloc(mm_ui_t *ui, size_t n) {
    mm_rune_slab *slab = ui->rune_slabs;
    if (!slab || n > slab->cap || slab->used > slab->cap - n) {
        size_t cap = (size_t)1 << 18;
        if (n > cap) cap = n;
        if (cap > (SIZE_MAX - sizeof(mm_rune_slab)) / sizeof(wchar_t)) return NULL;
        slab = malloc(sizeof(mm_rune_slab) + cap * sizeof(wchar_t));
        if (!slab) return NULL;
        slab->next = ui->rune_slabs;
        slab->used = 0;
        slab->cap = cap;
        ui->rune_slabs = slab;
    }
    wchar_t *out = slab->data + slab->used;
    slab->used += n;
    return out;
}

static void ui_free_rune_slabs(mm_ui_t *ui) {
    mm_rune_slab *slab = ui->rune_slabs;
    while (slab) {
        mm_rune_slab *next = slab->next;
        free(slab);
        slab = next;
    }
    ui->rune_slabs = NULL;
    ui->runes_ready = 0;
}

/* Non-ASCII lines are decoded once. ASCII is widened per match and is
   not stored. An all-ASCII list does not walk the items. */
static void ui_prepare_runes(mm_ui_t *ui) {
    if (!ui->has_non_ascii) {
        ui->runes_ready = ui->count;
        return;
    }
    int i = ui->runes_ready;
    if (i < 0) i = 0;
    for (; i < ui->count; i++) {
        mchars_t *c = &ui->items[i].text;
        if (c->ascii || c->decoded) continue;
        wchar_t *buf = ui_rune_alloc(ui, (size_t)c->length + 1);
        if (!buf || !mchars_decode_into(c, buf, c->length + 1)) continue;
    }
    ui->runes_ready = ui->count;
}

static void ui_qcache_clear(mm_ui_t *ui) {
    for (int i = 0; i < MM_UI_QCACHE; i++) {
        free(ui->qcache[i].query);
        free(ui->qcache[i].idx);
        ui->qcache[i].query = NULL;
        ui->qcache[i].idx = NULL;
        ui->qcache[i].n = 0;
        ui->qcache[i].item_count = 0;
    }
    ui->qcache_next = 0;
}

static void ui_qcache_store(mm_ui_t *ui, const char *query, const int *idx, int n) {
    if (!query || !query[0] || n < 0 || !idx) return;
    for (int i = 0; i < MM_UI_QCACHE; i++) {
        if (!ui->qcache[i].query || strcmp(ui->qcache[i].query, query) != 0)
            continue;
        int *copy = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
        if (!copy) return;
        if (n > 0) memcpy(copy, idx, (size_t)n * sizeof(int));
        free(ui->qcache[i].idx);
        ui->qcache[i].idx = copy;
        ui->qcache[i].n = n;
        ui->qcache[i].item_count = ui->count;
        return;
    }
    int slot = ui->qcache_next % MM_UI_QCACHE;
    if (ui->qcache_next < INT_MAX) ui->qcache_next++;
    char *q = strdup(query);
    int *copy = malloc((size_t)(n > 0 ? n : 1) * sizeof(int));
    if (!q || !copy) { free(q); free(copy); return; }
    if (n > 0) memcpy(copy, idx, (size_t)n * sizeof(int));
    free(ui->qcache[slot].query);
    free(ui->qcache[slot].idx);
    ui->qcache[slot].query = q;
    ui->qcache[slot].idx = copy;
    ui->qcache[slot].n = n;
    ui->qcache[slot].item_count = ui->count;
}

static bool ui_qcache_find(const mm_ui_t *ui, const char *query, int **out, int *n) {
    if (!query || !query[0]) return false;
    for (int i = 0; i < MM_UI_QCACHE; i++) {
        if (!ui->qcache[i].query || ui->qcache[i].item_count != ui->count)
            continue;
        if (strcmp(ui->qcache[i].query, query) != 0) continue;
        int cn = ui->qcache[i].n;
        int *copy = malloc((size_t)(cn > 0 ? cn : 1) * sizeof(int));
        if (!copy) return false;
        if (cn > 0) memcpy(copy, ui->qcache[i].idx, (size_t)cn * sizeof(int));
        *out = copy;
        *n = cn;
        return true;
    }
    return false;
}

/* Release line text. The pointer array itself is left to the caller. */
static void ui_free_line_storage(mm_ui_t *ui) {
    if (!ui->lines_pooled && ui->lines) {
        for (int i = 0; i < ui->count; i++) free(ui->lines[i]);
    }
    ui_free_slabs(ui);
    ui->lines_pooled = false;
}

static void ui_discard_line(mm_ui_t *ui, char *line) {
    if (!ui->lines_pooled) free(line);
}

static char *ui_store_line(mm_ui_t *ui, const char *src, size_t len) {
    char *line;
    if (ui->lines_pooled || ui->count == 0) {
        line = ui_slab_alloc(ui, len + 1);
        if (line) {
            ui->lines_pooled = true;
            memcpy(line, src, len);
            line[len] = '\0';
            return line;
        }
        if (ui->lines_pooled) return NULL;
    }
    line = malloc(len + 1);
    if (!line) return NULL;
    memcpy(line, src, len);
    line[len] = '\0';
    return line;
}

static void ui_handle_sigwinch(int signal_number) {
    (void)signal_number;
    ui_resize_pending = 1;
}



/* Forward declarations. */
static mm_color_t ui_theme_color(const mm_ui_t *ui, mm_color_attr_t entry,
                                 mm_color_t fallback);
static void ui_rebuild_header_lines(mm_ui_t *ui);
static void ui_deselect_item(mm_ui_t *ui, int item);
static void ui_draw_preview(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *win,
                            int x, int y, int w, int h);

/* Keep dynamic action arguments alive after temporary action maps are freed. */
static bool ui_set_runtime_string(mm_ui_t *ui, const char **target, const char *value) {
    if (!value) return false;
    char *copy = strdup(value);
    if (!copy) return false;
    if (ui->runtime_strings_n == ui->runtime_strings_cap) {
        size_t cap = ui->runtime_strings_cap ? ui->runtime_strings_cap * 2 : 8;
        if (cap < ui->runtime_strings_cap ||
            cap > SIZE_MAX / sizeof(*ui->runtime_strings)) {
            free(copy);
            return false;
        }
        char **grown = realloc(ui->runtime_strings, cap * sizeof(*grown));
        if (!grown) {
            free(copy);
            return false;
        }
        ui->runtime_strings = grown;
        ui->runtime_strings_cap = cap;
    }
    ui->runtime_strings[ui->runtime_strings_n++] = copy;
    *target = copy;
    return true;
}

/* Join the first header_line_count input lines into the header block.
   Lines past the input are blank rows, matching fzf's padded header. */
static void ui_rebuild_header_lines(mm_ui_t *ui) {
    free(ui->header_lines_buf);
    ui->header_lines_buf = NULL;
    ui->header_lines = NULL;
    int n = ui->header_line_count;
    if (n <= 0 || !ui->lines || ui->count <= 0) return;
    int shown = n > 4096 ? 4096 : n;
    int have = shown < ui->count ? shown : ui->count;
    int pad = shown - have;
    size_t hlen = 1;
    for (int i = 0; i < have; i++) {
        size_t len = ui->lines[i] ? strlen(ui->lines[i]) : 0;
        if (hlen > SIZE_MAX - len - 1) return;
        hlen += len + 1;
    }
    if ((size_t)pad > SIZE_MAX - hlen) return;
    hlen += (size_t)pad;
    char *hbuf = malloc(hlen);
    if (!hbuf) return;
    size_t o = 0;
    for (int i = 0; i < have; i++) {
        if (i) hbuf[o++] = '\n';
        const char *line = ui->lines[i] ? ui->lines[i] : "";
        size_t len = strlen(line);
        memcpy(hbuf + o, line, len);
        o += len;
    }
    for (int i = 0; i < pad; i++) hbuf[o++] = '\n';
    hbuf[o] = 0;
    ui->header_lines_buf = hbuf;
    ui->header_lines = hbuf;
}

/* strconv.Atoi, then require a non-negative value. "-0" is zero. */
static bool ui_parse_nonneg_int(const char *s, int *out) {
    if (!s || !*s) return false;
    const char *p = s;
    bool neg = false;
    if (*p == '+' || *p == '-') {
        neg = *p == '-';
        p++;
        if (!*p) return false;
    }
    long n = 0;
    bool any = false;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return false;
        any = true;
        if (n > (LONG_MAX - (*p - '0')) / 10) return false;
        n = n * 10 + (*p - '0');
    }
    if (!any || n > INT_MAX) return false;
    if (neg && n != 0) return false;
    *out = (int)n;
    return true;
}

/* --- query buffer helpers --- */
static void ui_clear_search_override(mm_ui_t *ui) {
    free(ui->search_override);
    ui->search_override = NULL;
}

/* A real query edit drops the search() override, matching fzf: the override
   lives only until the displayed input changes. Hidden input discards the
   edit afterwards, so the override stays until that restore. */
static void query_note_edit(mm_ui_t *ui) {
    if (!ui->no_input) ui_clear_search_override(ui);
}

static void query_restore(mm_ui_t *ui, const char *s) {
    size_t n = s ? strlen(s) : 0;
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
    memcpy(ui->query_buf, s ? s : "", n + 1);
    ui->query_len = n;
    ui->query_pos = (int)n;
}

static void query_set(mm_ui_t *ui, const char *s) {
    if (ui->no_input) return;
    query_note_edit(ui);
    query_restore(ui, s);
}

static void query_insert(mm_ui_t *ui, const char *s, size_t n) {
    if (ui->no_input || n == 0) return;
    if (n > SIZE_MAX - ui->query_len - 1 ||
        ui->query_len + n > INT_MAX)
        return;
    query_note_edit(ui);
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
    if (ui->no_input) return;
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    if (pos == 0) return;
    query_note_edit(ui);
    size_t start = pos - 1;
    while (start > 0 && ((unsigned char)ui->query_buf[start] & 0xC0) == 0x80)
        start--;
    memmove(ui->query_buf + start, ui->query_buf + pos, ui->query_len - pos + 1);
    ui->query_len -= pos - start;
    ui->query_pos = (int)start;
}

static bool query_delete(mm_ui_t *ui) {
    if (ui->no_input) return false;
    size_t pos = (size_t)ui->query_pos;
    if (pos >= ui->query_len) return false;
    query_note_edit(ui);
    size_t end = pos + 1;
    while (end < ui->query_len &&
           ((unsigned char)ui->query_buf[end] & 0xC0) == 0x80)
        end++;
    memmove(ui->query_buf + pos, ui->query_buf + end, ui->query_len - end + 1);
    ui->query_len -= end - pos;
    return true;
}

static void query_kill_before_cursor(mm_ui_t *ui) {
    if (ui->no_input) return;
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    if (pos == 0) return;
    query_note_edit(ui);
    memmove(ui->query_buf, ui->query_buf + pos, ui->query_len - pos + 1);
    ui->query_len -= pos;
    ui->query_pos = 0;
}

static void query_kill_after_cursor(mm_ui_t *ui) {
    if (ui->no_input) return;
    size_t pos = (size_t)ui->query_pos;
    if (pos > ui->query_len) pos = ui->query_len;
    if (pos == ui->query_len) {
        ui->query_buf[pos] = '\0';
        return;
    }
    query_note_edit(ui);
    ui->query_len = pos;
    ui->query_buf[pos] = '\0';
}

/* Subword movement treats punctuation/underscores as separators and splits
   lower-to-upper transitions (for example, "fooBar" -> "foo" + "Bar").
   UTF-8 continuation bytes remain attached to their leading byte. */
static size_t query_prev_codepoint(const char *s, size_t pos) {
    if (pos == 0) return 0;
    pos--;
    while (pos > 0 && ((unsigned char)s[pos] & 0xC0) == 0x80) pos--;
    return pos;
}

static size_t query_next_codepoint(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    pos++;
    while (pos < len && ((unsigned char)s[pos] & 0xC0) == 0x80) pos++;
    return pos;
}

static bool query_subword_char(unsigned char c) {
    return c >= 0x80 || (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9');
}

static size_t query_subword_start(const char *s, size_t pos) {
    if (pos == 0) return 0;
    size_t cur = query_prev_codepoint(s, pos);
    while (cur > 0) {
        size_t prev = query_prev_codepoint(s, cur);
        unsigned char a = (unsigned char)s[prev];
        unsigned char b = (unsigned char)s[cur];
        if (!query_subword_char(a) || !query_subword_char(b)) break;
        if (a >= 'a' && a <= 'z' && b >= 'A' && b <= 'Z') break;
        cur = prev;
    }
    return cur;
}

static size_t query_subword_end(const char *s, size_t len, size_t pos) {
    if (pos >= len) return len;
    size_t cur = pos;
    size_t next = query_next_codepoint(s, len, cur);
    while (next < len) {
        unsigned char a = (unsigned char)s[cur];
        unsigned char b = (unsigned char)s[next];
        if (!query_subword_char(a) || !query_subword_char(b)) break;
        if (a >= 'a' && a <= 'z' && b >= 'A' && b <= 'Z') break;
        cur = next;
        next = query_next_codepoint(s, len, cur);
    }
    return next;
}

/* fzf kill-subword uses subWordNext: delete through the first of
   [a-z][A-Z], a word char followed by a non-word char, or the last char. */
static size_t query_kill_subword_end(const char *s, size_t len, size_t pos) {
    if (pos >= len) return pos;
    size_t i = pos;
    while (i < len) {
        size_t next = query_next_codepoint(s, len, i);
        unsigned char a = (unsigned char)s[i];
        bool last = next >= len;
        unsigned char b = last ? 0 : (unsigned char)s[next];
        bool boundary = last ||
            (a >= 'a' && a <= 'z' && b >= 'A' && b <= 'Z') ||
            (query_subword_char(a) && !query_subword_char(b));
        if (boundary) return next;
        i = next;
    }
    return len;
}

/* --- searching --- */
static void ui_search(mm_ui_t *ui);
static bool ui_execute_action(mm_ui_t *ui, const mm_binding_t *b);
static bool ui_run_action(mm_ui_t *ui, const mm_binding_t *b);
static bool ui_exec_event(mm_ui_t *ui, const char *name);
static void ui_exec_result_events(mm_ui_t *ui, int previous_count);

static char *ui_track_key_for(mm_ui_t *ui, int item) {
    if (!ui->id_nth_spec || item < 0 || item >= ui->count) return NULL;
    char *raw = mm_accept_nth_expand(ui->id_nth_spec, ui->lines[item],
                                     ui->delim, item);
    if (!raw) return NULL;
    char *key = mm_strip_last_delimiter(raw, ui->delim);
    free(raw);
    return key;
}

static bool ui_restore_tracked_cursor(mm_ui_t *ui, int tracked_item,
                                      const char *track_key) {
    if (track_key && ui->id_nth_spec) {
        for (int i = 0; i < ui->nresults; i++) {
            char *candidate = ui_track_key_for(ui, ui->results[i]);
            bool match = candidate && strcmp(candidate, track_key) == 0;
            free(candidate);
            if (match) {
                ui->cursor = i;
                return true;
            }
        }
        return false;
    }
    if (tracked_item >= 0) {
        for (int i = 0; i < ui->nresults; i++) {
            if (ui->results[i] == tracked_item) {
                ui->cursor = i;
                return true;
            }
        }
    }
    return false;
}

static void ui_free_identity_keys(char **keys, int count) {
    if (!keys) return;
    for (int i = 0; i < count; i++) free(keys[i]);
    free(keys);
}

static void ui_restore_selected_keys(mm_ui_t *ui, char **keys, int key_count) {
    ui->nselected = 0;
    ui->sel_order_n = 0;
    if (!ui->selected || !ui->multi || !keys) return;

    for (int k = 0; k < key_count; k++) {
        for (int item = 0; item < ui->count; item++) {
            char *candidate = ui_track_key_for(ui, item);
            bool match = candidate && strcmp(candidate, keys[k]) == 0;
            free(candidate);
            if (!match || ui->selected[item]) continue;

            if (ui->sel_order_n == ui->sel_order_cap) {
                if (ui->sel_order_cap > INT_MAX / 2) return;
                int new_cap = ui->sel_order_cap ? ui->sel_order_cap * 2 : 16;
                if ((size_t)new_cap > SIZE_MAX / sizeof(*ui->sel_order))
                    return;
                int *grown = realloc(ui->sel_order,
                                     (size_t)new_cap * sizeof(*ui->sel_order));
                if (!grown) return;
                ui->sel_order = grown;
                ui->sel_order_cap = new_cap;
            }
            ui->selected[item] = true;
            ui->sel_order[ui->sel_order_n++] = item;
            ui->nselected++;
            break;
        }
    }
}

static const char *ui_match_query(const mm_ui_t *ui) {
    if (ui->search_override) return ui->search_override;
    return ui->query_buf ? ui->query_buf : "";
}

/* An empty query with no transform lists every item in input order. */
static bool ui_list_is_plain(const mm_ui_t *ui) {
    const char *query = ui_match_query(ui);
    return (!query || !query[0]) && !ui->tac && !ui->raw && !ui->phony &&
           ui->search_enabled && !ui->has_excluded &&
           ui->header_line_count == 0 && ui->nth_n == 0 && ui->with_nth_n == 0;
}

static void ui_clear_window_pos(mm_ui_t *ui) {
    if (!ui->window_pos) return;
    mm_match_pos_free(ui->window_pos, ui->window_pos_n);
    ui->window_pos = NULL;
    ui->window_pos_n = 0;
    ui->window_pos_from = 0;
}

static void ui_note_plain_results(mm_ui_t *ui) {
    ui->plain_results = ui_list_is_plain(ui) && ui->nresults == ui->count &&
                        (ui->nresults == 0 ||
                         (ui->results && ui->results[0] == 0 &&
                          ui->results[ui->nresults - 1] == ui->nresults - 1));
}

/* Append items that arrived since the last empty-query refresh. Returns
   false when the full search has to rebuild the list. */
static bool ui_search_append(mm_ui_t *ui, bool query_changed, int prev_nresults,
                             char *track_key, int tracked_item) {
    if (query_changed || !ui_list_is_plain(ui)) return false;
    if (!ui->plain_results && ui->nresults != 0) return false;
    int need = ui->count;
    if (need < ui->nresults) return false;
    if (need > ui->results_cap) {
        int cap = ui->results_cap > 0 ? ui->results_cap : 1024;
        while (cap < need) {
            if (cap > INT_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap < need || (size_t)cap > SIZE_MAX / sizeof(int)) return false;
        int *grown = realloc(ui->results, (size_t)cap * sizeof(int));
        if (!grown) return false;
        ui->results = grown;
        ui->results_cap = cap;
    }
    for (int i = ui->nresults; i < need; i++) ui->results[i] = i;
    ui->nresults = need;
    ui->plain_results = true;
    ui->results_item_count = ui->count;
    ui->lazy_highlight = false;
    mm_match_pos_free(ui->match_pos, ui->match_pos_n);
    ui->match_pos = NULL;
    ui->match_pos_n = 0;
    ui_clear_window_pos(ui);
    bool restored = ui_restore_tracked_cursor(ui, tracked_item, track_key);
    if ((tracked_item >= 0 || track_key) && !restored &&
        ui->track_current && !ui->track)
        ui->track_current = false;
    free(track_key);
    if (!restored && ui->cursor >= ui->nresults)
        ui->cursor = ui->nresults ? ui->nresults - 1 : 0;
    if (ui->offset > ui->cursor) ui->offset = ui->cursor;
    ui_exec_result_events(ui, prev_nresults);
    return true;
}

static void ui_finish_search(mm_ui_t *ui, bool query_changed, int prev_nresults,
                             char *track_key, int tracked_item);

/* The query was cleared. Every plain item matches, in input order. Filling
   the existing buffer avoids building a pattern and allocating a new list. */
static bool ui_search_restore_plain(mm_ui_t *ui, int prev_nresults,
                                    char *track_key, int tracked_item) {
    if (!ui_list_is_plain(ui)) return false;
    int need = ui->count;
    if (need < 0) return false;
    if (need > ui->results_cap) {
        int cap = ui->results_cap > 0 ? ui->results_cap : 1024;
        while (cap < need) {
            if (cap > INT_MAX / 2) {
                cap = need;
                break;
            }
            cap *= 2;
        }
        if (cap < need || (size_t)cap > SIZE_MAX / sizeof(int)) return false;
        int *grown = realloc(ui->results, (size_t)cap * sizeof(int));
        if (!grown) return false;
        ui->results = grown;
        ui->results_cap = cap;
    }
    for (int i = 0; i < need; i++) ui->results[i] = i;
    ui->nresults = need;
    ui->plain_results = true;
    ui->results_item_count = ui->count;
    ui->lazy_highlight = false;
    mm_match_pos_free(ui->match_pos, ui->match_pos_n);
    ui->match_pos = NULL;
    ui->match_pos_n = 0;
    ui_clear_window_pos(ui);
    ui_finish_search(ui, true, prev_nresults, track_key, tracked_item);
    return true;
}

/* True when cur is prev plus a suffix that cannot add matches. A "|" in
   the suffix can introduce an alternative, so that query is not a subset. */
static bool ui_query_is_narrowing(const char *prev, const char *cur) {
    if (!prev || !prev[0] || !cur) return false;
    size_t plen = strlen(prev);
    size_t qlen = strlen(cur);
    if (qlen <= plen || memcmp(cur, prev, plen) != 0) return false;
    return memchr(cur + plen, '|', qlen - plen) == NULL;
}

/* Restore the tracked row and run change/result. The list is already current. */
static void ui_finish_search(mm_ui_t *ui, bool query_changed, int prev_nresults,
                             char *track_key, int tracked_item) {
    bool restored = ui_restore_tracked_cursor(ui, tracked_item, track_key);
    if ((tracked_item >= 0 || track_key) && !restored &&
        ui->track_current && !ui->track)
        ui->track_current = false;
    free(track_key);
    if (!restored && ui->cursor >= ui->nresults)
        ui->cursor = ui->nresults ? ui->nresults - 1 : 0;
    if (ui->offset > ui->cursor) ui->offset = ui->cursor;
    if (query_changed)
        ui_exec_event(ui, "change");
    ui_exec_result_events(ui, prev_nresults);
}

/* Install a new match list and run the change/result events. res is taken
   over. mp is taken over; a NULL mp means highlights are filled on paint.
   The result buffer keeps its high-water size so clearing the query fills
   indexes in place instead of allocating the whole list again. */
static void ui_commit_results(mm_ui_t *ui, int *res, int n, mm_match_pos_t *mp,
                              bool lazy, bool query_changed, int prev_nresults,
                              char *track_key, int tracked_item) {
    if (res && res != ui->results && ui->results && n <= ui->results_cap) {
        if (n > 0) memcpy(ui->results, res, (size_t)n * sizeof(int));
        free(res);
        res = ui->results;
    } else if (res != ui->results) {
        free(ui->results);
        ui->results = res;
        ui->results_cap = res ? (n > 0 ? n : 1) : 0;
    }
    ui->nresults = res ? n : 0;
    if (res) ui->results_item_count = ui->count;
    ui->lazy_highlight = lazy && res != NULL;
    ui_note_plain_results(ui);
    mm_match_pos_free(ui->match_pos, ui->match_pos_n);
    ui->match_pos = mp;
    ui->match_pos_n = mp ? n : 0;
    ui_clear_window_pos(ui);
    ui_finish_search(ui, query_changed, prev_nresults, track_key, tracked_item);
}

static void ui_search(mm_ui_t *ui) {
    /* fzf refilters after the whole action chain, and skips that refilter
       when the chain quits (accept, become, abort). */
    if (ui->defer_search > 0) {
        ui->search_dirty = true;
        return;
    }
    ui->search_dirty = false;
    const char *current_query = ui_match_query(ui);
    bool query_changed = ui->last_search_query &&
                         strcmp(ui->last_search_query, current_query) != 0;
    /* Snapshot the previous query before it is replaced. A longer query
       with no new "|" can be matched against that result list alone. */
    char *prev_query = ui->last_search_query ? strdup(ui->last_search_query)
                                             : NULL;
    char *search_query = strdup(current_query ? current_query : "");
    if (search_query) {
        free(ui->last_search_query);
        ui->last_search_query = search_query;
    }
    int prev_nresults = ui->nresults;
    char *track_key = ui->pending_track_key;
    ui->pending_track_key = NULL;
    int tracked_item = (!track_key && (ui->track || ui->track_current) &&
                        ui->results && ui->cursor >= 0 &&
                        ui->cursor < ui->nresults) ? ui->results[ui->cursor] : -1;

    if (ui_search_append(ui, query_changed, prev_nresults, track_key, tracked_item)) {
        free(prev_query);
        return;
    }
    if (ui_search_restore_plain(ui, prev_nresults, track_key, tracked_item)) {
        free(prev_query);
        return;
    }

    if (ui->phony || !ui->search_enabled) {
        /* The query does not filter the items: every item is a result, in
           the order of the input. */
        int n = ui->count;
        int *res = malloc(sizeof(int) * (size_t)(n > 0 ? n : 1));
        int kept = 0;
        if (res) {
            for (int i = 0; i < n; i++) {
                if (i < ui->header_line_count) continue;
                if (ui->excluded && ui->excluded[i]) continue;
                res[kept++] = i;
            }
        }
        free(prev_query);
        if (res) {
            ui_commit_results(ui, res, kept, NULL, false, query_changed,
                              prev_nresults, track_key, tracked_item);
        } else {
            ui->nresults = 0;
            ui->lazy_highlight = false;
            ui->plain_results = false;
            ui_finish_search(ui, query_changed, prev_nresults, track_key,
                             tracked_item);
        }
        return;
    }

    /* Whole-line queries keep their index lists. --nth, --with-nth and
       --raw still go through the position-building search. */
    bool fast = !ui->raw && ui->nth_n == 0 && ui->with_nth_n == 0 &&
                current_query && current_query[0];
    if (fast) {
        int *cached = NULL;
        int cached_n = 0;
        if (ui_qcache_find(ui, current_query, &cached, &cached_n)) {
            free(prev_query);
            ui_commit_results(ui, cached, cached_n, NULL, true, query_changed,
                              prev_nresults, track_key, tracked_item);
            return;
        }
    }

    bool forward = mm_criteria_forward(&ui->crit);
    mm_pattern_t *pat = mm_pattern_build(current_query, ui->fuzzy, ui->extended,
                                         ui->case_mode, !ui->literal, forward, true);
    if (!pat) {
        ui->nresults = 0;
        ui->plain_results = false;
        ui->lazy_highlight = false;
        ui->results_item_count = 0;
        ui_clear_window_pos(ui);
        free(prev_query);
        ui_finish_search(ui, query_changed, prev_nresults, track_key, tracked_item);
        return;
    }
    if (ui->no_sort) mm_pattern_set_sortable(pat, false);

    if (fast && !mm_pattern_is_empty(pat) && ui->items) {
        ui_prepare_runes(ui);
        bool longer = ui_query_is_narrowing(prev_query, current_query);
        bool same = prev_query && current_query &&
                    strcmp(prev_query, current_query) == 0;
        int covered = ui->results_item_count;
        bool have = ui->results && covered > 0 && covered <= ui->count;
        /* Same query, new tail: the walk appended lines after the last
           scan. A longer query only drops matches, so the old hits plus
           that tail are the whole candidate set. Unsorted order is the
           input order, which a narrowed list is not. */
        bool narrow = pat->sortable && have &&
                      (longer || (same && covered < ui->count));
        const int *cands = NULL;
        int ncand = 0;
        int lo = 0;
        int hi = ui->count;
        if (narrow) {
            cands = ui->results;
            ncand = ui->nresults > 0 ? ui->nresults : 0;
            lo = covered;
            hi = ui->count;
        }
        int n = -1;
        int *res = mm_batch_search_interactive(
            ui->items, ui->count, cands, ncand, lo, hi,
            ui->has_excluded ? ui->excluded : NULL,
            ui->header_line_count, pat, &ui->crit, ui->tac, &n);
        mm_pattern_free(pat);
        free(prev_query);
        if (!res || n < 0) {
            /* Keep the previous matches on screen. Do not narrow from
               them: they belong to an older query than the one just
               attempted. An empty result is a non-NULL pointer. */
            ui->results_item_count = 0;
            ui_finish_search(ui, query_changed, prev_nresults, track_key,
                             tracked_item);
            return;
        }
        ui_qcache_store(ui, current_query, res, n);
        ui_commit_results(ui, res, n, NULL, true, query_changed,
                          prev_nresults, track_key, tracked_item);
        return;
    }
    free(prev_query);

    mm_input_t input = {
        .lines = ui->lines, .count = ui->count, .items = ui->items, .base = 0,
        .delim = ui->delim, .nth_ranges = ui->nth, .nth_n = ui->nth_n,
        .with_nth_ranges = ui->with_nth, .with_nth_n = ui->with_nth_n
    };
    int n = 0;
    mm_match_pos_t *mp = NULL;
    int *res = mm_batch_search(&input, pat, &ui->crit, ui->tac, &n, &mp);
    if (res && (ui->header_line_count > 0 || ui->excluded)) {
        int kept = 0;
        for (int i = 0; i < n; i++) {
            int item = res[i];
            if (item < ui->header_line_count ||
                (item >= 0 && item < ui->count && ui->excluded &&
                 ui->excluded[item])) {
                if (mp) free(mp[i].pos);
                continue;
            }
            if (kept != i) {
                res[kept] = res[i];
                if (mp) {
                    mp[kept] = mp[i];
                    mp[i].pos = NULL;
                    mp[i].npos = 0;
                }
            }
            kept++;
        }
        n = kept;
    }
    if (ui->raw) {
        size_t count = (size_t)(ui->count > 0 ? ui->count : 1);
        bool *seen = calloc(count, sizeof(*seen));
        int *raw_results = count <= SIZE_MAX / sizeof(*raw_results)
                         ? malloc(count * sizeof(*raw_results)) : NULL;
        mm_match_pos_t *raw_positions = calloc(count, sizeof(*raw_positions));
        if (seen && raw_results && raw_positions) {
            for (int i = 0; i < n; i++) {
                int item = res ? res[i] : -1;
                if (item >= 0 && item < ui->count &&
                    item >= ui->header_line_count &&
                    !(ui->excluded && ui->excluded[item]))
                    seen[item] = true;
            }
            int match_cursor = 0, kept = 0;
            for (int item = 0; item < ui->count; item++) {
                if (item < ui->header_line_count) continue;
                if (ui->excluded && ui->excluded[item]) continue;
                if (seen[item]) {
                    int match_item = res[match_cursor];
                    raw_results[kept] = match_item;
                    if (mp) {
                        raw_positions[kept] = mp[match_cursor];
                        mp[match_cursor].pos = NULL;
                        mp[match_cursor].npos = 0;
                    }
                    match_cursor++;
                } else {
                    raw_results[kept] = item;
                }
                kept++;
            }
            free(seen);
            free(res);
            mm_match_pos_free(mp, n);
            res = raw_results;
            mp = raw_positions;
            n = kept;
        } else {
            free(seen);
            free(raw_results);
            free(raw_positions);
        }
    }
    mm_pattern_free(pat);
    ui->lazy_highlight = false;
    ui_commit_results(ui, res, n, mp, false, query_changed, prev_nresults,
                      track_key, tracked_item);
}

/* Execute the actions bound to an event. */
/* Execute the actions bound to an event. */
static bool ui_exec_event(mm_ui_t *ui, const char *name) {
    const mm_event_binding_t *found[16];
    int nf = mm_event_lookup(&ui->events, name, found, 16);
    bool keep = true;
    ui->stop_chain = false;
    ui->defer_search++;
    for (int i = 0; i < nf && keep && !ui->stop_chain; i++)
        keep = ui_run_action(ui, &(const mm_binding_t){
            .type = MM_EV_INVALID, .rune = 0, .action = found[i]->action,
            .arg = found[i]->arg });
    ui->stop_chain = false;
    ui->defer_search--;
    if (keep && ui->defer_search == 0 && ui->search_dirty) ui_search(ui);
    return keep;
}

/* Dispatch focus when the current input item changes, not on every redraw. */
static void ui_exec_focus_if_changed(mm_ui_t *ui, int *last_item) {
    int item = ui->nresults > 0 && ui->cursor >= 0 &&
               ui->cursor < ui->nresults
             ? ui->results[ui->cursor] : -1;
    if (item == *last_item) return;
    *last_item = item;
    if (item >= 0) ui_exec_event(ui, "focus");
}

/* Match fzf's list-update events: result on result-count changes, one/zero
   when a finalized list enters those states, and result-final after a completed
   input list. Mark initialization before invoking actions to avoid recursion
   when an event action itself changes the query. */
static void ui_exec_result_events(mm_ui_t *ui, int previous_count) {
    bool count_changed = previous_count != ui->nresults;
    bool complete = ui->input_fd < 0 || ui->input_done;
    bool initial_final = complete && !ui->result_events_initialized;

    if (complete) ui->result_events_initialized = true;
    if (count_changed) ui_exec_event(ui, "result");

    if (!complete || (!count_changed && !initial_final)) return;

    if (ui->nresults == 0) ui_exec_event(ui, "zero");
    else if (ui->nresults == 1) ui_exec_event(ui, "one");

    const mm_event_binding_t *found[1];
    if (mm_event_lookup(&ui->events, "result", found, 1) > 0 ||
        mm_event_lookup(&ui->events, "result-final", found, 1) > 0)
        ui_exec_event(ui, "result-final");
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

/* fzf prints a raw newline or carriage return as a dim line symbol when
   the item stays on one row (src/tui/light.go LF / CR). */
static char *ui_show_breaks(const char *text) {
    if (!text) return NULL;
    size_t extra = 0;
    for (const char *p = text; *p; p++)
        if (*p == '\n' || *p == '\r') extra += 2;
    char *out = malloc(strlen(text) + extra + 1);
    if (!out) return NULL;
    char *o = out;
    for (const char *p = text; *p; p++) {
        if (*p == '\n') {
            memcpy(o, "\u240a", 3);
            o += 3;
        } else if (*p == '\r') {
            memcpy(o, "\u240d", 3);
            o += 3;
        } else {
            *o++ = *p;
        }
    }
    *o = 0;
    return out;
}

/* Split a --read0 item on newlines. A trailing newline is an empty last
   line, matching util.Chars.Lines. *off[i] is the rune index of line i
   in the original text (the newline itself is not part of the line). */
static int ui_split_item_lines(const char *text, char ***out_pieces, int **out_off) {
    int n = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') n++;
    char **pieces = calloc((size_t)n, sizeof(char *));
    int *off = calloc((size_t)n, sizeof(int));
    if (!pieces || !off) {
        free(pieces);
        free(off);
        return 0;
    }
    int count = 0;
    int rune = 0;
    const char *start = text;
    int start_rune = 0;
    for (const char *p = text; ; p++) {
        if (*p != 0 && *p != '\n') {
            wchar_t cp;
            size_t used = mm_utf8_decode(p, strlen(p), &cp);
            if (used == 0) used = 1;
            rune++;
            p += used - 1;
            continue;
        }
        size_t len = (size_t)(p - start);
        char *line = malloc(len + 1);
        if (!line) break;
        memcpy(line, start, len);
        line[len] = 0;
        pieces[count] = line;
        off[count] = start_rune;
        count++;
        if (*p == 0 || count >= n) break;
        rune++;
        start = p + 1;
        start_rune = rune;
    }
    *out_pieces = pieces;
    *out_off = off;
    return count;
}

static int ui_rune_count(const char *s) {
    int n = 0;
    if (!s) return 0;
    size_t len = strlen(s);
    for (size_t i = 0; i < len; ) {
        wchar_t cp;
        size_t used = mm_utf8_decode(s + i, len - i, &cp);
        if (used == 0) used = 1;
        i += used;
        n++;
    }
    return n;
}

/* Columns [start_col, start_col + content) of `text`, with optional
   ellipses. *skip_runes is how many runes were cut from the left, so match
   positions can be shifted onto the slice. */
static char *ui_slice_text(const char *text, int start_col, int content,
                           bool left_ell, bool right_ell, const char *ellipsis,
                           int *out_width, int *skip_runes) {
    if (!ellipsis || !*ellipsis) ellipsis = MM_ELLIPSIS;
    if (start_col < 0) start_col = 0;
    if (content < 0) content = 0;
    size_t len = strlen(text);
    size_t i = 0;
    int col = 0;
    int rune = 0;
    while (i < len && col < start_col) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        int cw = mm_runewidth(cp);
        if (col + cw > start_col) break;
        col += cw;
        i += used;
        rune++;
    }
    *skip_runes = rune;
    size_t byte_start = i;
    int acc = 0;
    while (i < len && acc < content) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        int cw = mm_runewidth(cp);
        if (acc + cw > content) break;
        acc += cw;
        i += used;
    }
    int ell_w = ui_str_width(ellipsis);
    size_t elen = strlen(ellipsis);
    char *out = malloc((left_ell ? elen : 0) + (i - byte_start) +
                       (right_ell ? elen : 0) + 1);
    if (!out) { *out_width = 0; return NULL; }
    char *p = out;
    if (left_ell) { memcpy(p, ellipsis, elen); p += elen; }
    memcpy(p, text + byte_start, i - byte_start);
    p += i - byte_start;
    if (right_ell) { memcpy(p, ellipsis, elen); p += elen; }
    *p = 0;
    *out_width = (left_ell ? ell_w : 0) + acc + (right_ell ? ell_w : 0);
    return out;
}

static size_t ui_byte_at_rune(const char *text, int rune_index) {
    size_t len = text ? strlen(text) : 0;
    size_t i = 0;
    int rune = 0;
    if (rune_index < 0) rune_index = 0;
    while (i < len && rune < rune_index) {
        wchar_t cp;
        size_t used = mm_utf8_decode(text + i, len - i, &cp);
        if (used == 0) used = 1;
        i += used;
        rune++;
    }
    return i;
}

static char *ui_rune_slice_dup(const char *text, int from_rune, int to_rune) {
    size_t b0 = ui_byte_at_rune(text, from_rune);
    size_t b1 = to_rune < 0 ? strlen(text) : ui_byte_at_rune(text, to_rune);
    if (b1 < b0) b1 = b0;
    char *s = malloc(b1 - b0 + 1);
    if (!s) return NULL;
    memcpy(s, text + b0, b1 - b0);
    s[b1 - b0] = 0;
    return s;
}

static int ui_trailing_space_runes(const char *s) {
    if (!s || !*s) return 0;
    size_t len = strlen(s);
    int runes = 0;
    int last_non = -1;
    size_t i = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(s + i, len - i, &cp);
        if (used == 0) used = 1;
        if (!iswspace(cp)) last_non = runes;
        runes++;
        i += used;
    }
    if (last_non < 0) return runes;
    return runes - last_non - 1;
}

/* Rune length of the delimiter that ends `token`, or 0. AWK has none. */
static int ui_last_delim_runes(const char *token, mm_delimiter_t delim) {
    if (!token) return 0;
    if (delim.is_regex && delim.rx) {
        regex_t *rx = delim.rx;
        size_t len = strlen(token);
        size_t search = 0;
        size_t last_start = 0, last_end = 0;
        bool found = false;
        while (search <= len) {
            regmatch_t match;
            int rc = regexec(rx, token + search, 1, &match,
                             search == 0 ? 0 : REG_NOTBOL);
            if (rc != 0 || match.rm_so < 0 || match.rm_eo < 0) break;
            size_t start = search + (size_t)match.rm_so;
            size_t end = search + (size_t)match.rm_eo;
            if (end < start || end > len) break;
            found = true;
            last_start = start;
            last_end = end;
            if (end == len) break;
            if (end == search) {
                wchar_t cp;
                size_t used = mm_utf8_decode(token + end, len - end, &cp);
                search = end + (used ? used : 1);
            } else {
                search = end;
            }
        }
        if (found && last_end == len) return mm_utf8_count(token + last_start);
        return 0;
    }
    if (delim.str && delim.slen > 0) {
        size_t n = strlen(token);
        size_t d = (size_t)delim.slen;
        if (n >= d && memcmp(token + n - d, delim.str, d) == 0)
            return mm_utf8_count(delim.str);
    }
    return 0;
}

static void ui_free_tokens(char **tokens, int n, int *prefix) {
    if (tokens) {
        for (int i = 0; i < n; i++) free(tokens[i]);
        free(tokens);
    }
    free(prefix);
}

/* fzf splitOffset1 / splitOffset2 (src/terminal.go printHighlighted). */
static void ui_freeze_offsets(const mm_ui_t *ui, const char *text,
                              int *split1, int *split2) {
    *split1 = -1;
    *split2 = -1;
    if (!ui || (ui->freeze_left <= 0 && ui->freeze_right <= 0) || !text) return;
    int *prefix = NULL;
    int n = 0;
    char **tokens = mm_tokenize(text, ui->delim, &prefix, &n);
    if (!tokens || !prefix) {
        ui_free_tokens(tokens, n, prefix);
        return;
    }
    if (ui->freeze_left > 0 && n > 0) {
        int idx = (ui->freeze_left < n ? ui->freeze_left : n) - 1;
        int tlen = mm_utf8_count(tokens[idx]);
        if (tlen < 0) tlen = 0;
        *split1 = prefix[idx] + tlen - ui_trailing_space_runes(tokens[idx]);
    }
    if (ui->freeze_right > 0) {
        int index = ui->freeze_left - 1;
        int alt = n - ui->freeze_right - 1;
        if (alt > index) index = alt;
        if (index < 0) {
            *split2 = 0;
        } else if (index >= ui->freeze_left && index < n) {
            int tlen = mm_utf8_count(tokens[index]);
            if (tlen < 0) tlen = 0;
            *split2 = prefix[index] + tlen - ui_last_delim_runes(tokens[index], ui->delim);
        }
        if (*split2 < *split1) *split2 = *split1;
    }
    ui_free_tokens(tokens, n, prefix);
}

static bool ui_push_pos(int **pos, int *n, int *cap, int value) {
    if (*n >= *cap) {
        int ncap = *cap ? *cap * 2 : 8;
        int *np = realloc(*pos, sizeof(int) * (size_t)ncap);
        if (!np) return false;
        *pos = np;
        *cap = ncap;
    }
    (*pos)[(*n)++] = value;
    return true;
}

/* Keep frozen fields visible and scroll the middle, matching fzf when
   hscroll is on and wrap is off. */
static char *ui_apply_freeze(mm_ui_t *ui, const char *text, int text_limit,
                             const mm_match_pos_t *matched,
                             int *out_width, int **out_pos, int *out_npos) {
    *out_width = 0;
    *out_pos = NULL;
    *out_npos = 0;
    if (!text) return NULL;
    const char *ell = ui->ellipsis ? ui->ellipsis : MM_ELLIPSIS;
    int ell_w = ui_str_width(ell);
    if (ell_w < 1) ell_w = MM_ELLIPSIS_WIDTH;
    int ell_runes = ui_rune_count(ell);

    int split1 = -1, split2 = -1;
    ui_freeze_offsets(ui, text, &split1, &split2);
    int total = mm_utf8_count(text);
    if (total < 0) total = 0;
    int left_end = (split1 > 0 && split1 < total) ? split1 : 0;
    int right_at = -1;
    if (split2 >= 0 && split2 < total && split2 >= left_end) right_at = split2;

    char *left = left_end > 0 ? ui_rune_slice_dup(text, 0, left_end) : NULL;
    char *middle = ui_rune_slice_dup(text, left_end, right_at);
    char *right = right_at >= 0 ? ui_rune_slice_dup(text, right_at, -1) : NULL;

    int budget = text_limit;
    if (budget < 1) budget = 1;
    char *left_fit = NULL;
    char *mid_fit = NULL;
    char *right_fit = NULL;
    int left_w = 0, mid_w = 0, right_w = 0;
    bool left_cut = false, right_cut = false;
    int right_skip = 0;
    int *mid_pos = NULL;
    int mid_npos = 0;
    int mid_cap = 0;
    bool mid_scrolled = false;

    if (left && *left && budget > 0) {
        int lw = ui_str_width(left);
        int allow = budget - ell_w;
        if (allow < 0) allow = 0;
        if (lw > allow) {
            left_fit = ui_fit_text(left, budget, &left_w, ell);
            left_cut = true;
        } else {
            left_fit = strdup(left);
            left_w = lw;
        }
        if (left_w > budget) left_w = budget;
        budget -= left_w;
    }
    if (right && *right && budget > 0) {
        int rw = ui_str_width(right);
        int allow = budget - ell_w;
        if (allow < 0) allow = 0;
        if (rw > allow) {
            int content = budget - ell_w;
            if (content < 1) content = 0;
            int start = rw - content;
            if (start < 0) start = 0;
            right_fit = ui_slice_text(right, start, content, start > 0, false,
                                      ell, &right_w, &right_skip);
            right_cut = start > 0;
        } else {
            right_fit = strdup(right);
            right_w = rw;
        }
        if (right_w > budget) right_w = budget;
        budget -= right_w;
    }
    if (middle && *middle && budget > 0) {
        int full_w = ui_str_width(middle);
        int match_end_col = 0;
        bool scroll = matched && matched->npos > 0 && matched->pos && full_w > budget;
        if (scroll) {
            int last = -1;
            for (int k = 0; k < matched->npos; k++) {
                int p = matched->pos[k];
                if (p < left_end) continue;
                if (right_at >= 0 && p >= right_at) continue;
                int rel = p - left_end;
                if (rel > last) last = rel;
            }
            if (last < 0) scroll = false;
            else {
                size_t len = strlen(middle);
                size_t i = 0;
                int rune = 0;
                int col = 0;
                while (i < len) {
                    wchar_t cp;
                    size_t used = mm_utf8_decode(middle + i, len - i, &cp);
                    if (used == 0) used = 1;
                    col += mm_runewidth(cp);
                    if (rune == last) { match_end_col = col; break; }
                    i += used;
                    rune++;
                }
            }
        }
        int prefix_keep = full_w > budget ? budget - ell_w : budget;
        if (prefix_keep < 0) prefix_keep = 0;
        int off = ui->hscroll_off;
        int half = budget / 2 - ell_w;
        if (off > half) off = half;
        if (off < 0) off = 0;
        if (scroll && match_end_col > prefix_keep) {
            int right_edge = match_end_col + off;
            if (right_edge > full_w) right_edge = full_w;
            bool right_ell = right_edge < full_w;
            bool left_ell = true;
            int content = budget - ell_w - (right_ell ? ell_w : 0);
            if (content < 1) content = 1;
            int start = right_edge - content;
            if (start <= 0) {
                start = 0;
                left_ell = false;
                content = budget - (right_ell ? ell_w : 0);
                if (content < 1) content = 1;
            }
            int skip_runes = 0;
            mid_fit = ui_slice_text(middle, start, content, left_ell, right_ell,
                                    ell, &mid_w, &skip_runes);
            if (mid_fit && matched) {
                int er = left_ell ? ell_runes : 0;
                int content_runes = ui_rune_count(mid_fit) - er -
                                    (right_ell ? ell_runes : 0);
                for (int k = 0; k < matched->npos; k++) {
                    int p = matched->pos[k];
                    if (p < left_end || (right_at >= 0 && p >= right_at)) continue;
                    int rel = (p - left_end) - skip_runes;
                    if (rel < 0 || rel >= content_runes) continue;
                    ui_push_pos(&mid_pos, &mid_npos, &mid_cap, rel + er);
                }
            }
            mid_scrolled = true;
        } else {
            mid_fit = ui_fit_text(middle, budget, &mid_w, ell);
        }
    }

    size_t ln = left_fit ? strlen(left_fit) : 0;
    size_t mn = mid_fit ? strlen(mid_fit) : 0;
    size_t rn = right_fit ? strlen(right_fit) : 0;
    char *out = malloc(ln + mn + rn + 1);
    if (!out) {
        free(left); free(middle); free(right);
        free(left_fit); free(mid_fit); free(right_fit);
        free(mid_pos);
        return NULL;
    }
    memcpy(out, left_fit ? left_fit : "", ln);
    memcpy(out + ln, mid_fit ? mid_fit : "", mn);
    memcpy(out + ln + mn, right_fit ? right_fit : "", rn);
    out[ln + mn + rn] = 0;
    *out_width = ui_str_width(out);

    int cap = 0;
    int shift = left_fit ? ui_rune_count(left_fit) : 0;
    if (matched && matched->npos > 0 && matched->pos) {
        int left_keep = 0;
        if (left_fit) {
            left_keep = ui_rune_count(left_fit);
            if (left_cut) left_keep -= ell_runes;
            if (left_keep < 0) left_keep = 0;
        }
        for (int k = 0; k < matched->npos; k++) {
            int p = matched->pos[k];
            if (p >= 0 && p < left_end && p < left_keep)
                ui_push_pos(out_pos, out_npos, &cap, p);
        }
        for (int k = 0; k < mid_npos; k++)
            ui_push_pos(out_pos, out_npos, &cap, shift + mid_pos[k]);
        if (!mid_scrolled && mid_fit) {
            int keep = ui_rune_count(mid_fit);
            if (middle && ui_str_width(middle) > mid_w) keep -= ell_runes;
            if (keep < 0) keep = 0;
            for (int k = 0; k < matched->npos; k++) {
                int p = matched->pos[k];
                if (p < left_end || (right_at >= 0 && p >= right_at)) continue;
                int rel = p - left_end;
                if (rel < keep) ui_push_pos(out_pos, out_npos, &cap, shift + rel);
            }
        }
        shift += mid_fit ? ui_rune_count(mid_fit) : 0;
        if (right_fit) {
            int content_runes = ui_rune_count(right_fit);
            int add = 0;
            if (right_cut) {
                content_runes -= ell_runes;
                add = ell_runes;
            }
            if (content_runes < 0) content_runes = 0;
            for (int k = 0; k < matched->npos; k++) {
                int p = matched->pos[k];
                if (right_at < 0 || p < right_at) continue;
                int rel = p - right_at;
                int adj = rel - (right_cut ? right_skip : 0);
                if (adj < 0 || adj >= content_runes) continue;
                ui_push_pos(out_pos, out_npos, &cap, shift + add + adj);
            }
        }
    }

    free(left); free(middle); free(right);
    free(left_fit); free(mid_fit); free(right_fit);
    free(mid_pos);
    return out;
}

/* fzf's scrollbar geometry (src/terminal.go getScrollbar). per_line is the
   average number of rows an item occupies. */
static void ui_scrollbar_range(int total, int height, int offset, int per_line,
                               int *bar_len, int *bar_start) {
    if (per_line < 1) per_line = 1;
    if (total <= 0 || total * per_line <= height) {
        *bar_len = 0;
        *bar_start = 0;
        return;
    }
    int span = total * per_line;
    int len = height * height / span;
    if (len < 1) len = 1;
    int start = 0;
    int denom = span - height;
    if (denom > 0)
        start = (height * per_line - len) * offset / denom;
    if (start > height - len) start = height - len;
    if (start < 0) start = 0;
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

/* Rows taken by the horizontal edges of a border shape. */
static int ui_border_line_count(int shape) {
    bool top, bottom, left, right;
    mm_border_sides(shape, &top, &bottom, &left, &right);
    return (top ? 1 : 0) + (bottom ? 1 : 0);
}

/* --footer-border defaults to a single line on the side away from the list. */
static int ui_resolve_footer_shape(const mm_ui_t *ui) {
    int shape = ui->footer_border_set ? ui->footer_border : MM_BORDER_LINE;
    /* Inline borders need a list frame with both horizontal edges. Without
       one, fzf falls back to a plain line. */
    if (shape == MM_BORDER_INLINE) shape = MM_BORDER_LINE;
    if (shape == MM_BORDER_LINE)
        shape = ui->reverse ? MM_BORDER_TOP : MM_BORDER_BOTTOM;
    return shape;
}

/* --header-border=line sits on the side away from the list. Inline falls
   back to that line unless a list frame can hold it, which is not drawn yet. */
static int ui_resolve_header_shape(const mm_ui_t *ui) {
    int shape = ui->header_border < 0 ? MM_BORDER_ROUNDED : ui->header_border;
    if (shape == MM_BORDER_INLINE) shape = MM_BORDER_LINE;
    if (shape == MM_BORDER_LINE)
        shape = ui->reverse ? MM_BORDER_BOTTOM : MM_BORDER_TOP;
    return shape;
}

/* A closed frame keeps a row for each horizontal edge. A single edge keeps
   one row and, when that edge would cover the info line, shifts the text. */
static void ui_reserve_header_border(const mm_ui_t *ui, bool reverse,
                                     int *header_row, int *list_edge) {
    int hshape = ui_resolve_header_shape(ui);
    bool ht, hb, hl, hr;
    mm_border_sides(hshape, &ht, &hb, &hl, &hr);
    int lines = (ht ? 1 : 0) + (hb ? 1 : 0);
    if (reverse) {
        if (hl || hr || (ht && hb)) {
            if (ht) *header_row += 1;
            *list_edge += lines;
        } else if (ht) {
            *header_row += 1;
            *list_edge += 1;
        } else if (hb) {
            *list_edge += 1;
        }
    } else if (hl || hr || (ht && hb)) {
        if (ht) *header_row -= 1;
        *list_edge -= lines;
    } else if (ht) {
        *list_edge -= 1;
    } else if (hb) {
        *header_row -= 1;
        *list_edge -= 1;
    }
}

/* --header-lines-border is off until the option is given. "line" faces the
   list: the top edge in the default layout, the bottom edge otherwise. */
static int ui_resolve_header_lines_shape(const mm_ui_t *ui) {
    if (!ui->header_lines_border_set) return MM_BORDER_NONE;
    int shape = ui->header_lines_border;
    if (shape == MM_BORDER_INLINE) shape = MM_BORDER_LINE;
    if (shape == MM_BORDER_LINE) {
        bool layout_default = !ui->reverse && ui->layout == 0;
        shape = layout_default ? MM_BORDER_TOP : MM_BORDER_BOTTOM;
    }
    return shape;
}

static bool ui_shape_faces_input(const mm_ui_t *ui, int shape) {
    bool top, bottom, left, right;
    mm_border_sides(shape, &top, &bottom, &left, &right);
    return ui->reverse ? top : bottom;
}

/* Labels are drawn on shapes that have a horizontal edge (fzf printLabel). */
static bool ui_shape_takes_label(int shape) {
    switch (shape) {
    case MM_BORDER_HORIZONTAL:
    case MM_BORDER_TOP:
    case MM_BORDER_BOTTOM:
    case MM_BORDER_ROUNDED:
    case MM_BORDER_SHARP:
    case MM_BORDER_BOLD:
    case MM_BORDER_BLOCK:
    case MM_BORDER_THINBLOCK:
    case MM_BORDER_DOUBLE:
    case MM_BORDER_DASHED:
        return true;
    default:
        return false;
    }
}

static void ui_draw_section_border(mm_window_t *win, const mm_border_glyphs_t *g,
                                   mm_color_t color, int left, int right,
                                   int top, int bottom, int shape) {
    if (!g || right < left || bottom < top) return;
    bool ht, hb, hl, hr;
    mm_border_sides(shape, &ht, &hb, &hl, &hr);
    if (ht) {
        mm_window_move(win, top, left);
        if (hl) mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top_left);
        int x0 = left + (hl ? 1 : 0);
        int x1 = right - (hr ? 1 : 0);
        for (int x = x0; x <= x1; x++)
            mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top);
        if (hr) mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->top_right);
    }
    if (hb && (!ht || bottom != top)) {
        mm_window_move(win, bottom, left);
        if (hl) mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom_left);
        int x0 = left + (hl ? 1 : 0);
        int x1 = right - (hr ? 1 : 0);
        for (int x = x0; x <= x1; x++)
            mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom);
        if (hr) mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->bottom_right);
    }
    int y0 = top + (ht ? 1 : 0);
    int y1 = bottom - (hb ? 1 : 0);
    for (int y = y0; y <= y1; y++) {
        if (hl) {
            mm_window_move(win, y, left);
            mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->left);
            /* The column inside a left edge is part of the frame. */
            if (right > left)
                mm_window_cprint(win, color, MM_COL_DEFAULT, 0, " ");
        }
        if (hr) {
            mm_window_move(win, y, right);
            mm_window_cprint(win, color, MM_COL_DEFAULT, 0, g->right);
        }
    }
}

static void ui_draw_border_label(mm_window_t *win, mm_ui_t *ui, const char *label,
                                 int pos, bool on_bottom, int shape,
                                 int left, int right, int top, int bottom) {
    if (!label || !*label || !ui_shape_takes_label(shape)) return;
    int width = right - left + 1;
    int lw = ui_str_width(label);
    if (width < 1 || lw < 1) return;
    int rel;
    if (pos == 0) rel = (width - lw) / 2;
    else if (pos < 0) rel = width + pos + 1 - lw;
    else rel = pos - 1;
    if (rel > width - lw) rel = width - lw;
    if (rel < 0) rel = 0;
    int row = (shape == MM_BORDER_BOTTOM || on_bottom) ? bottom : top;
    mm_window_move(win, row, left + rel);
    mm_window_cprint(win, ui_theme_color(ui, ui->theme.border_label, 145),
                     MM_COL_DEFAULT, 0, label);
}

/* Number of display rows occupied by a (possibly multiline) text block. */
static int ui_text_rows(const char *text) {
    if (!text || !*text) return 0;
    int rows = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') rows++;
    return rows;
}

/* One drawn header or footer row, in the order it was painted. `text`
   points into the live header/footer string and is only used until the
   next action replaces that string. */
typedef struct {
    int row;
    int kind; /* 1 header-lines, 2 explicit header, 3 footer */
    const char *text;
    size_t text_len;
} mm_click_line_t;

static mm_click_line_t ui_click_map[128];
static int ui_click_n;
static int ui_click_text_x;

static void ui_click_reset(int text_x) {
    ui_click_n = 0;
    ui_click_text_x = text_x;
}

static void ui_click_note(int row, int kind, const char *text, size_t text_len) {
    if (ui_click_n >= (int)(sizeof(ui_click_map) / sizeof(ui_click_map[0]))) return;
    ui_click_map[ui_click_n].row = row;
    ui_click_map[ui_click_n].kind = kind;
    ui_click_map[ui_click_n].text = text;
    ui_click_map[ui_click_n].text_len = text_len;
    ui_click_n++;
}

/* Trim a copy of a token and report its display width. */
static char *ui_click_trim(const char *token, int *width) {
    size_t n = strlen(token);
    while (n > 0 && isspace((unsigned char)token[n - 1])) n--;
    char *copy = malloc(n + 1);
    if (!copy) return NULL;
    memcpy(copy, token, n);
    copy[n] = 0;
    *width = ui_str_width(copy);
    return copy;
}

static int ui_click_token_count(const mm_ui_t *ui, const mm_click_line_t *line,
                                bool header) {
    if (!line->text || line->text_len == 0) return 0;
    char *text = malloc(line->text_len + 1);
    if (!text) return 0;
    memcpy(text, line->text, line->text_len);
    text[line->text_len] = 0;
    mm_delimiter_t awk = {0};
    int *prefix = NULL;
    int ntok = 0;
    char **toks = mm_tokenize(text, header ? ui->delim : awk, &prefix, &ntok);
    free(text);
    if (toks) {
        for (int i = 0; i < ntok; i++) free(toks[i]);
        free(toks);
    }
    free(prefix);
    return ntok;
}

/* FZF_CLICK_*_WORD and, for a header, FZF_CLICK_HEADER_NTH. */
static void ui_click_set_word(const mm_ui_t *ui, const mm_click_line_t *hit,
                              int column, int nth_base, bool last_line) {
    bool header = hit->kind != 3;
    unsetenv(header ? "FZF_CLICK_HEADER_WORD" : "FZF_CLICK_FOOTER_WORD");
    if (header) unsetenv("FZF_CLICK_HEADER_NTH");
    if (!hit->text || hit->text_len == 0 || column < 1) return;
    char *text = malloc(hit->text_len + 1);
    if (!text) return;
    memcpy(text, hit->text, hit->text_len);
    text[hit->text_len] = 0;
    mm_delimiter_t awk = {0};
    int *prefix = NULL;
    int ntok = 0;
    char **toks = mm_tokenize(text, header ? ui->delim : awk, &prefix, &ntok);
    free(text);
    if (!toks || !prefix) {
        if (toks) {
            for (int i = 0; i < ntok; i++) free(toks[i]);
            free(toks);
        }
        free(prefix);
        return;
    }
    int col = column - 1;
    for (int i = 0; i < ntok; i++) {
        int width = 0;
        char *trimmed = ui_click_trim(toks[i], &width);
        int start = prefix[i];
        if (trimmed && col >= start && col < start + (width > 0 ? width : 0)) {
            setenv(header ? "FZF_CLICK_HEADER_WORD" : "FZF_CLICK_FOOTER_WORD",
                   trimmed, 1);
            if (header) {
                char nth[64];
                int n = nth_base + i + 1;
                if (last_line && i == ntok - 1)
                    snprintf(nth, sizeof(nth), "%d..", n);
                else
                    snprintf(nth, sizeof(nth), "%d", n);
                setenv("FZF_CLICK_HEADER_NTH", nth, 1);
            }
            free(trimmed);
            break;
        }
        free(trimmed);
    }
    for (int i = 0; i < ntok; i++) free(toks[i]);
    free(toks);
    free(prefix);
}

/* A release on a header or footer row runs the matching click event.
   Returns 0 when the click missed, 1 when it was handled, 2 when the
   action ends the loop. */
static int ui_dispatch_click(mm_ui_t *ui, int x, int y) {
    const mm_click_line_t *hit = NULL;
    for (int i = 0; i < ui_click_n; i++) {
        if (ui_click_map[i].row == y) { hit = &ui_click_map[i]; break; }
    }
    if (!hit) return false;
    int column = x - ui_click_text_x + 1;
    if (column < 1) return false;
    bool header = hit->kind != 3;
    int line = 1;
    int nth_base = 0;
    int max_row = hit->row;
    for (int i = 0; i < ui_click_n; i++) {
        bool same = header ? ui_click_map[i].kind != 3 : ui_click_map[i].kind == 3;
        if (!same) continue;
        if (ui_click_map[i].row < hit->row) {
            line++;
            if (header) nth_base += ui_click_token_count(ui, &ui_click_map[i], true);
        }
        if (ui_click_map[i].row > max_row) max_row = ui_click_map[i].row;
    }
    char num[32];
    snprintf(num, sizeof(num), "%d", line);
    setenv(header ? "FZF_CLICK_HEADER_LINE" : "FZF_CLICK_FOOTER_LINE", num, 1);
    snprintf(num, sizeof(num), "%d", column);
    setenv(header ? "FZF_CLICK_HEADER_COLUMN" : "FZF_CLICK_FOOTER_COLUMN", num, 1);
    ui_click_set_word(ui, hit, column, nth_base, hit->row == max_row);
    return ui_exec_event(ui, header ? "click-header" : "click-footer") ? 1 : 2;
}

static void ui_sort_ints(int *v, int n) {
    for (int i = 1; i < n; i++) {
        int x = v[i];
        int j = i;
        while (j > 0 && v[j - 1] > x) {
            v[j] = v[j - 1];
            j--;
        }
        v[j] = x;
    }
}

/* Match positions for the rows about to be drawn. The scan itself does
   not allocate a position list for every hit. */
static void ui_highlight_visible(mm_ui_t *ui, int list_rows) {
    if (!ui->lazy_highlight || !ui->results || ui->nresults <= 0) return;
    const char *query = ui_match_query(ui);
    if (!query || !query[0]) return;
    int from = ui->offset;
    if (from < 0) from = 0;
    if (from > ui->nresults) from = ui->nresults;
    int rows = list_rows > 0 ? list_rows : 1;
    int to = (rows > ui->nresults - from) ? ui->nresults : from + rows;
    int n = to - from;
    if (n < 0) n = 0;
    if (!ui->window_pos || ui->window_pos_from != from || ui->window_pos_n != n) {
        ui_clear_window_pos(ui);
        if (n == 0) return;
        ui->window_pos = calloc((size_t)n, sizeof(*ui->window_pos));
        if (!ui->window_pos) return;
        ui->window_pos_n = n;
        ui->window_pos_from = from;
    } else {
        bool missing = false;
        for (int i = 0; i < n; i++) {
            if (!ui->window_pos[i].pos) { missing = true; break; }
        }
        if (!missing) return;
    }

    bool forward = mm_criteria_forward(&ui->crit);
    mm_pattern_t *pat = mm_pattern_build(query, ui->fuzzy, ui->extended,
                                         ui->case_mode, !ui->literal, forward,
                                         true);
    if (!pat) return;
    for (int i = 0; i < n; i++) {
        if (ui->window_pos[i].pos) continue;
        int item = ui->results[from + i];
        int *pos = NULL;
        int npos = 0;
        if (item >= 0 && item < ui->count) {
            mm_text_t text = mm_item_match_text(&ui->items[item]);
            if (text.length < 0) continue;
            int score = 0;
            if (!mm_pattern_match(pat, &text, &score, &pos, &npos)) {
                free(pos);
                pos = NULL;
                npos = 0;
            }
        }
        if (!pos) {
            pos = malloc(sizeof(int));
            if (!pos) continue;
            npos = 0;
        } else if (npos > 1) {
            ui_sort_ints(pos, npos);
        }
        ui->window_pos[i].pos = pos;
        ui->window_pos[i].npos = npos;
    }
    mm_pattern_free(pat);
}

/* -1 on the preview window means "use --preview-border". */
static int ui_preview_shape(const mm_ui_t *ui) {
    int shape = ui->preview_win.border_shape;
    if (shape < 0) shape = ui->preview_border;
    return shape;
}

static void ui_render(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *listwin,
                      mm_window_t *promptwin, mm_window_t *infowin) {
    int total = mm_renderer_max_y(r);
    int width = mm_renderer_max_x(r);
    /* --height draws a window of that many rows from the top of the
       terminal. The border and the list both use this height. */
    if (ui->height > 0 && ui->height < total) total = ui->height;

    /* Erase only when the preview moves or hides. Those frames leave
       border glyphs in cells the new layout does not write. A scroll or a
       cursor move overwrites the same cells, so wiping the window again
       only adds bytes. */
    {
        int layout = -1;
        if (ui->preview && !ui->preview_hidden) {
            const mm_preview_win_t *w = &ui->preview_win;
            layout = (w->pos << 20) ^ (w->size << 4) ^ (w->border_shape & 0x7f);
            if (w->size_percent) layout ^= 1 << 30;
            if (w->wrap) layout ^= 1 << 29;
        }
        if (layout != ui->preview_layout) {
            ui->preview_layout = layout;
            for (int y = 0; y < total; y++) {
                mm_renderer_move(r, y, 0);
                mm_renderer_csi(r, "2K");
            }
        }
    }

    /* A border box takes the first and the last line, and one column on
       each side; the content is then laid out inside it. */
    bool b_top, b_bottom, b_left, b_right;
    mm_border_sides(ui->border, &b_top, &b_bottom, &b_left, &b_right);

    /* A preview window takes one side of the screen; the list gets the rest.
       The preview is placed inside the border, so its share is computed from
       the area between the border edges. */
    int pv_x = 0, pv_y = 0, pv_w = 0, pv_h = 0;
    bool has_preview = ui->preview != NULL && !ui->preview_hidden;
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
        bool size_percent = ui->preview_win.size_percent;
        if (size_percent) {
            if (size < 1) size = 1;
            if (size > 100) size = 100;
            switch (ui->preview_win.pos) {
            case MM_PREVIEW_LEFT:
            case MM_PREVIEW_RIGHT: pv_total = inner_w * size / 100; break;
            default:               pv_total = inner_h * size / 100; break;
            }
            if (pv_total < 2) pv_total = 2;
        } else {
            /* fzf calculateSize adds the preview frame: outer = size + min - 1,
               where min is 1 plus the border cells (and a scrollbar column
               when the frame has no right edge). The result is clamped to
               leave the list its minimum window. */
            bool vertical = ui->preview_win.pos != MM_PREVIEW_LEFT &&
                            ui->preview_win.pos != MM_PREVIEW_RIGHT;
            bool edge_top, edge_bottom, edge_left, edge_right;
            mm_border_sides(ui_preview_shape(ui), &edge_top, &edge_bottom,
                            &edge_left, &edge_right);
            bool any_edge = edge_top || edge_bottom || edge_left || edge_right;
            bool full = edge_top && edge_bottom && edge_left && edge_right;
            int chrome = 0;
            if (vertical) {
                if (full) chrome = 2;
                else if (edge_top || edge_bottom)
                    chrome = (edge_top ? 1 : 0) + (edge_bottom ? 1 : 0);
            } else if (full) {
                /* Each side is the glyph plus a padding column (borderWidth 1). */
                chrome = 4;
            } else if (any_edge) {
                chrome = 3; /* left glyph and padding, plus the scrollbar column */
            } else {
                chrome = 1; /* scrollbar column, no frame */
            }
            int min_size = 1 + chrome;
            int base = vertical ? inner_h : inner_w;
            /* calculateSize leaves the list minHeight (3) or minWidth (4).
               A vertical preview is then capped again by the lines still
               free after the input rows and the preview's own minimum. */
            int max = base - (vertical ? 3 : 4);
            if (vertical) {
                int avail = base - 2 - min_size;
                if (avail < max) max = avail;
            }
            if (max < min_size) max = min_size;
            int raw = size + min_size - 1;
            if (raw < min_size) raw = min_size;
            if (raw > max) raw = max;
            /* Placement below peels one outer-border cell off pv_total.
               fzf already measured this size inside that border. */
            int peel = 0;
            switch (ui->preview_win.pos) {
            case MM_PREVIEW_LEFT:   peel = b_left ? 1 : 0; break;
            case MM_PREVIEW_TOP:    peel = b_top ? 1 : 0; break;
            case MM_PREVIEW_BOTTOM: peel = b_bottom ? 1 : 0; break;
            default:                peel = b_right ? 1 : 0; break;
            }
            pv_total = raw + peel;
        }
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
            /* A percentage is already a share of the inner height. Peeling
               the outer bottom border off it leaves a blank row above that
               border. Absolute sizes added that cell in the peel above. */
            pv_h = pv_total - (b_bottom && !ui->preview_win.size_percent ? 1 : 0);
            pv_x = area_x + (b_left ? 1 : 0);
            pv_w = area_w - (b_left ? 1 : 0);
            pv_y = area_y + area_h - (b_bottom && !ui->preview_win.size_percent ? 1 : 0) - pv_h;
            area_h = pv_y - area_y;
            break;
        default:
            pv_w = pv_total - (b_right ? 1 : 0);
            pv_x = area_x + area_w - 1 - (b_right ? 1 : 0) - pv_w + 1;
            /* A preview with no right edge sits against the outer border,
               and the list keeps the column that used to separate them. */
            {
                bool edge_top, edge_bottom, edge_left, edge_right;
                mm_border_sides(ui_preview_shape(ui), &edge_top, &edge_bottom,
                                &edge_left, &edge_right);
                (void)edge_top; (void)edge_bottom; (void)edge_left;
                if (b_right && !edge_right) pv_x += 1;
            }
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
    /* inline-right keeps the count on the prompt and a separator row
       under it. inline keeps both on the prompt line. */
    bool info_on_prompt = ui->info && (ui->info_inline || ui->info_inline_right);
    bool has_sep = ui->separator_default || (ui->separator && ui->separator[0]);
    int sep_rows = (ui->info_inline_right && has_sep) ? 1 : 0;
    int inline_sep_row = -1;
    int block_row, list_first, list_last;
    /* The footer sits at the top of the list area in the default layout and
       at the bottom with --reverse. A line border adds one row; a box adds
       the edges the shape actually draws. */
    bool has_footer = ui->footer && *ui->footer;
    int footer_shape = MM_BORDER_NONE;
    int footer_rows = 0;
    if (has_footer) {
        footer_shape = ui_resolve_footer_shape(ui);
        footer_rows = ui_text_rows(ui->footer) + ui_border_line_count(footer_shape);
        if (footer_rows < 1) footer_rows = 1;
    }
    int hl_shape = ui_resolve_header_lines_shape(ui);
    bool hl_separate = ui->header_lines_border_set && rest_hlines &&
                       hl_shape != MM_BORDER_NONE;
    int hl_extra = hl_separate ? ui_border_line_count(hl_shape) : 0;
    int hl_box_top = -1, hl_box_bottom = -1;
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
        block_row = prompt_row + (info_on_prompt ? 0 : 1);
        info_row  = ui->info ? (info_on_prompt ? prompt_row : block_row) : -1;
        int row = block_row + 1 + (info_on_prompt ? sep_rows : 0);
        if (!ui->header_first && has_header) {
            header_row = row;
            row += header_rows;
        }
        if (rest_hlines) {
            if (hl_separate) {
                bool ht, hb, edge_l, edge_r;
                mm_border_sides(hl_shape, &ht, &hb, &edge_l, &edge_r);
                int anchor = row;
                hl_box_top = anchor;
                hl_box_bottom = anchor + hlines_rows + hl_extra - 1;
                header_lines_row = anchor + (ht ? 1 : 0);
                row = hl_box_bottom + 1;
            } else {
                header_lines_row = row;
                row += rest_rows;
            }
        }
        list_first = row;
        list_last  = content_bottom;
        if (footer_rows) list_last -= footer_rows;
    } else {
        /* The list grows upwards from the info line at the bottom. */
        prompt_row = content_bottom;
        if (has_first_block) {
            if (first_is_explicit) header_row = content_bottom - first_rows + 1;
            else                   header_lines_row = content_bottom;
            prompt_row = content_bottom - first_rows;
        }
        block_row = prompt_row - (info_on_prompt ? 0 : 1);
        info_row  = ui->info ? (info_on_prompt ? prompt_row : block_row) : -1;
        int row = block_row - 1 - (info_on_prompt ? sep_rows : 0);
        if (!ui->header_first && has_header) {
            /* The explicit header sits directly above the info line. */
            header_row = row - header_rows + 1;
            row -= header_rows;
        }
        if (rest_hlines) {
            /* Header lines are stacked above the explicit header, with the
               first input line closest to it. A border wraps that block. */
            if (hl_separate) {
                bool ht, hb, edge_l, edge_r;
                mm_border_sides(hl_shape, &ht, &hb, &edge_l, &edge_r);
                int anchor = row;
                hl_box_bottom = anchor;
                hl_box_top = anchor - (hlines_rows + hl_extra) + 1;
                header_lines_row = anchor - (hb ? 1 : 0);
                row = hl_box_top - 1;
            } else {
                header_lines_row = row;
                row -= rest_rows;
            }
        }
        list_last  = row;
        list_first = content_top + footer_rows;
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
            if (draw_header_box)
                ui_reserve_header_border(ui, true, &header_row, &list_first);
            if (draw_list_box) {
                list_first += 2;
                list_last--;
            }
        } else {
            if (draw_input_box) list_last--;
            if (draw_header_box)
                ui_reserve_header_border(ui, false, &header_row, &list_last);
            if (draw_list_box) {
                list_first++;
                list_last--;
            }
        }
    }

    if (sep_rows > 0)
        inline_sep_row = ui->reverse ? prompt_row + 1 : prompt_row - 1;

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

    /* Footer: pinned to the outer edge of the list area. The border shape
       decides whether that edge is a line, a box, or absent. */
    ui_click_reset(area_x + content_x + 2);
    if (has_footer && footer_rows > 0) {
        int top, bottom;
        if (ui->reverse) {
            bottom = content_bottom;
            top = content_bottom - footer_rows + 1;
        } else {
            top = content_top;
            bottom = content_top + footer_rows - 1;
        }
        bool edge_top, edge_bottom, edge_left, edge_right;
        mm_border_sides(footer_shape, &edge_top, &edge_bottom, &edge_left, &edge_right);
        int text_row = top + (edge_top ? 1 : 0);
        int text_col = area_x + content_x + 2;
        int clear_end = area_x + area_w - 1;
        const char *p = ui->footer;
        int row = text_row;
        while (p && *p && row <= bottom - (edge_bottom ? 1 : 0)) {
            const char *nl = strchr(p, '\n');
            size_t seglen = nl ? (size_t)(nl - p) : strlen(p);
            char *seg = malloc(seglen + 1);
            if (seg) {
                memcpy(seg, p, seglen);
                seg[seglen] = 0;
                mm_window_move(listwin, row, text_col);
                mm_window_cprint(listwin,
                                 ui_theme_color(ui, ui->theme.footer, MM_THEME_HEADER),
                                 MM_COL_DEFAULT, 0, seg);
                ui_clear_to_eol(listwin, clear_end,
                                text_col + ui_str_width(seg), MM_COL_DEFAULT);
                free(seg);
            }
            ui_click_note(row, 3, p, seglen);
            row++;
            if (!nl) break;
            p = nl + 1;
        }
        if (footer_shape != MM_BORDER_NONE) {
            const mm_border_glyphs_t *g = ui->no_unicode
                ? mm_border_glyphs_ascii(footer_shape)
                : mm_border_glyphs(footer_shape);
            int left = area_x;
            int right = area_x + area_w - 1;
            if (right < left) right = left;
            ui_draw_section_border(listwin, g,
                                   ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                   left, right, top, bottom, footer_shape);
            ui_draw_border_label(listwin, ui, ui->footer_label,
                                 ui->footer_label_pos, ui->footer_label_bottom,
                                 footer_shape, left, right, top, bottom);
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
                /* The header shares the list columns. A line that does not
                   fit is cut with the ellipsis, leaving the scrollbar
                   column empty so the preview border stays one cell away. */
                int header_limit = list_w - 2;
                if (header_limit < 1) header_limit = 1;
                int shown_w = 0;
                char *shown = ui_fit_text(seg, header_limit, &shown_w, ui->ellipsis);
                mm_window_move(listwin, row, area_x + content_x + 2);
                if (shown) {
                    mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.header, MM_THEME_HEADER),
                                     MM_COL_DEFAULT, 0, shown);
                    ui_clear_to_eol(listwin, header_limit, shown_w, MM_COL_DEFAULT);
                    free(shown);
                }
                free(seg);
            }
            ui_click_note(row, 2, p, seglen);
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
                /* A row that used to be a list line still has its gutter. */
                int gutter = area_x + content_x + list_border_inset;
                mm_window_move(listwin, row, gutter);
                ui_clear_to_eol(listwin, area_x + area_w - 1, gutter, MM_COL_DEFAULT);
                mm_window_move(listwin, row, area_x + content_x + 2);
                mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.header, MM_THEME_HEADER),
                                 MM_COL_DEFAULT, 0, seg);
                free(seg);
            }
            ui_click_note(row, 1, p, seglen);
            row += step;
            if (!nl) break;
            p = nl + 1;
            /* A trailing newline is a blank header row. ui_text_rows counts it. */
            if (!*p) {
                int gutter = area_x + content_x + list_border_inset;
                mm_window_move(listwin, row, gutter);
                ui_clear_to_eol(listwin, area_x + area_w - 1, gutter, MM_COL_DEFAULT);
                ui_click_note(row, 1, "", 0);
                break;
            }
        }
        if (hl_separate && hl_box_top >= 0 && hl_box_bottom >= hl_box_top) {
            const mm_border_glyphs_t *g = ui->no_unicode
                ? mm_border_glyphs_ascii(hl_shape) : mm_border_glyphs(hl_shape);
            int left = area_x;
            int right = area_x + area_w - 1;
            if (right < left) right = left;
            ui_draw_section_border(listwin, g,
                                   ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                                   left, right, hl_box_top, hl_box_bottom, hl_shape);
        }
    }

    /* Prompt. */
    mm_window_move(promptwin, prompt_row, area_x + content_x);
    const char *pr = ui->no_input ? "" : (ui->prompt ? ui->prompt : "> ");
    mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.prompt, MM_THEME_PROMPT),
                     MM_COL_DEFAULT, ui->no_bold ? 0 : MM_BOLD, pr);
    /* The query is drawn in the prompt colour, like the reference does. */
    if (!ui->no_input) {
        mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.prompt, MM_THEME_PROMPT),
                         MM_COL_DEFAULT, 0, ui->query_buf);
    }
    int used = ui_str_width(pr) + (ui->no_input ? 0 : ui_str_width(ui->query_buf));
    if (!ui->no_input && ui->ghost && *ui->ghost && !ui->query_len) {
        mm_window_cprint(promptwin, ui_theme_color(ui, ui->theme.ghost, MM_COL_DEFAULT),
                         MM_COL_DEFAULT, MM_DIM, ui->ghost);
        used += ui_str_width(ui->ghost);
    }
    ui_clear_to_eol(promptwin, list_w, used, MM_COL_DEFAULT);
    /* Keep the terminal cursor aligned with the UTF-8 query insertion point. */
    int query_cursor_width = ui->no_input ? 0 : ui_query_cursor_width(ui);
    mm_window_move(promptwin, prompt_row,
                   area_x + content_x + ui_str_width(pr) + query_cursor_width);

    /* Hidden info and inline-right each keep a separator row of their own.
       inline draws its dashes on the prompt line instead. */
    if (!ui->info || inline_sep_row >= 0) {
        int sep_row = inline_sep_row >= 0 ? inline_sep_row : block_row;
        int sep_len = list_w;
        if (sep_len > 0 && sep_row >= 0) {
            char *sep = malloc((size_t)sep_len * 3 + 1);
            if (sep) {
                for (int i = 0; i < sep_len; i++) {
                    sep[i * 3 + 0] = (char)0xE2;
                    sep[i * 3 + 1] = (char)0x94;
                    sep[i * 3 + 2] = (char)0x80;
                }
                sep[sep_len * 3] = 0;
                mm_window_move(listwin, sep_row, area_x + content_x);
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
        int listed = ui->count;
        if (ui->header_line_count > 0) {
            int hidden = ui->header_line_count;
            if (hidden > listed) hidden = listed;
            listed -= hidden;
        }
        int total_items = ui->nresults > listed ? ui->nresults : listed;
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
        if (ui->info_inline) {
            /* fzf: one gap column, the prefix (default " < "), the count,
               one space, then dashes to the end of the list line. */
            const char *prefix = (ui->info_prefix && ui->info_prefix[0])
                                     ? ui->info_prefix : " < ";
            int px = area_x + content_x + ui_str_width(pr);
            if (!ui->no_input) px += ui_str_width(ui->query_buf);
            px += 1;
            mm_window_move(infowin, prompt_row, px);
            mm_window_cprint(infowin,
                ui_theme_color(ui, ui->theme.prompt, MM_THEME_PROMPT),
                MM_COL_DEFAULT, ui->no_bold ? 0 : MM_BOLD, prefix);
            mm_window_cprint(infowin,
                ui_theme_color(ui, ui->theme.info, MM_THEME_INFO),
                MM_COL_DEFAULT, 0, info_text);
            int query_w = ui->no_input ? 0 : ui_str_width(ui->query_buf);
            int from_content = ui_str_width(pr) + query_w + 1 +
                               ui_str_width(prefix) + n;
            int seplen = info_w - from_content - 1;
            if (seplen < 0) seplen = 0;
            if (seplen > 0) {
                char *sep = malloc((size_t)seplen * 3 + 1);
                if (sep) {
                    for (int i = 0; i < seplen; i++) {
                        sep[i * 3 + 0] = (char)0xE2;
                        sep[i * 3 + 1] = (char)0x94;
                        sep[i * 3 + 2] = (char)0x80;
                    }
                    sep[seplen * 3] = 0;
                    mm_window_cprint(infowin,
                        ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                        MM_COL_DEFAULT, 0, " ");
                    mm_window_cprint(infowin,
                        ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                        MM_COL_DEFAULT, 0, sep);
                    free(sep);
                }
            }
        } else {
        int info_start_x = area_x + info_x;
        if (ui->info_inline_right) {
            /* Spinner column, a space, the count, and one trailing column. */
            info_start_x = area_x + area_w - n - 3;
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
        /* A border already sitting against the info line replaces the
           default separator. An explicit --separator is kept. */
        bool suppress_sep = false;
        if (ui->separator_default) {
            if (ui->header_lines_border_set && rest_hlines && ui->layout != 1 &&
                ui_shape_faces_input(ui, hl_shape))
                suppress_sep = true;
            if (!suppress_sep && ui->header_border_set && has_header &&
                !ui->header_first && ui->header_border != MM_BORDER_NONE) {
                int hshape = ui_resolve_header_shape(ui);
                if (ui_shape_faces_input(ui, hshape)) suppress_sep = true;
            }
        }
        if (!ui->info_inline_right && used < info_w) {
            /* The separator is U+2500 repeated; each glyph is three bytes.
               Size the buffer from the glyph count, not the other way round. */
            /* One column goes to the space that separates the counts from
               the separator. fzf keeps that space when the dashes are
               replaced by a facing border (printInfoImpl). */
            int seplen = info_w - used - 1;
            if (seplen < 0) seplen = 0;
            if (suppress_sep) {
                if (seplen > 0)
                    mm_window_cprint(infowin,
                        ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                        MM_COL_DEFAULT, ui->no_color ? MM_DIM : 0, " ");
            } else {
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
        }
    }

    /* The text area excludes the pointer, the space after it and the
       scrollbar column (fzf: t.wrapCols). */
    int text_limit = list_w - 2;
    if (text_limit < 1) text_limit = 1;
    int bar_len = 0, bar_start = 0;
    int per_line = 1;
    if (ui->nresults > 0 && (!ui->no_multi_line || ui->wrap || ui->gap > 0)) {
        int sample_at = ui->offset;
        int limit = ui->nresults - list_rows - 1;
        if (sample_at > limit) sample_at = limit;
        if (sample_at < 0) sample_at = 0;
        int sum = 0, counted = 0;
        for (int s = 0; s < list_rows && sample_at + s < ui->nresults; s++) {
            const char *sample = ui->lines[ui->results[sample_at + s]];
            int lines = 1;
            if (sample && !ui->no_multi_line) {
                for (const char *q = sample; *q; q++)
                    if (*q == '\n') lines++;
            }
            sum += lines + ui->gap;
            counted++;
        }
        if (counted > 0 && sum / counted > 0) per_line = sum / counted;
    }
    ui_scrollbar_range(ui->nresults, list_rows, ui->offset, per_line,
                       &bar_len, &bar_start);
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
    ui_highlight_visible(ui, list_rows);
    int nitems = ui->nresults - ui->offset;
    if (nitems < 0) nitems = 0;
    const char *jump_labels = ui->jump_labels && *ui->jump_labels
                            ? ui->jump_labels : MM_DEFAULT_JUMP_LABELS;
    if (ui->jump_mode) ui->jump_label_count = 0;
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
        if (ui->jump_mode && (size_t)block < strlen(jump_labels))
            ui->jump_label_count = block + 1;
        {
        int item_idx = ui->results[idx];
        char *display_text = NULL;
        char *single_line_text = NULL;
        const char *text = ui->lines[item_idx];
        if (ui->with_nth && ui->with_nth_n > 0) {
            display_text = mm_transform_line(text, ui->delim,
                                             ui->with_nth, ui->with_nth_n);
            if (display_text) text = display_text;
        }
        if (ui->no_multi_line && text && (strchr(text, '\n') || strchr(text, '\r'))) {
            single_line_text = ui_show_breaks(text);
            if (single_line_text) text = single_line_text;
        }
        const mm_match_pos_t *matched = NULL;
        if (ui->match_pos && idx < ui->match_pos_n)
            matched = &ui->match_pos[idx];
        else if (ui->window_pos && idx >= ui->window_pos_from &&
                 idx < ui->window_pos_from + ui->window_pos_n)
            matched = &ui->window_pos[idx - ui->window_pos_from];
        bool raw_nonmatch = ui->raw && ui->query_len > 0 &&
                            (!matched || matched->npos == 0);
        bool cur = (idx == ui->cursor);
        bool sel = ui->selected && ui->selected[item_idx];
        int text_width = 0;
        char *fitted = NULL;
        char **pieces = NULL;
        int *piece_off = NULL;
        int n_pieces = 0;
        bool break_lines = false;
        int sign_width = 0;
        const char *sign = ui->wrap_sign ? ui->wrap_sign : "";

        /* Match column range, used to keep a hit visible while scrolling. */
        int match_end_col = 0;
        bool scroll_match = !ui->no_hscroll && !ui->wrap && text_limit > 0 &&
                            cur && matched && matched->npos > 0 && matched->pos;
        if (scroll_match) {
            int first = matched->pos[0];
            int last = matched->pos[0];
            for (int k = 1; k < matched->npos; k++) {
                if (matched->pos[k] < first) first = matched->pos[k];
                if (matched->pos[k] > last) last = matched->pos[k];
            }
            size_t len = strlen(text);
            size_t i = 0;
            int rune = 0;
            int col = 0;
            while (i < len) {
                wchar_t cp;
                size_t used = mm_utf8_decode(text + i, len - i, &cp);
                if (used == 0) used = 1;
                int cw = mm_runewidth(cp);
                col += cw;
                if (rune == last) { match_end_col = col; break; }
                i += used;
                rune++;
                (void)first;
            }
        }

        int *view_pos = NULL;
        mm_match_pos_t view_match = { NULL, 0 };
        const mm_match_pos_t *shown = matched;

        if (ui->wrap) {
            sign_width = ui_str_width(sign);
            int cont = text_limit - sign_width;
            if (cont < 1) cont = 1;
            n_pieces = ui_wrap_text(text, text_limit, cont, ui->wrap_word, &pieces, &piece_off);
        } else if (!ui->no_multi_line && text && strchr(text, '\n')) {
            /* --read0 item: each embedded newline starts another row. */
            break_lines = true;
            const char *ell = ui->ellipsis ? ui->ellipsis : MM_ELLIPSIS;
            n_pieces = ui_split_item_lines(text, &pieces, &piece_off);
            for (int c = 0; c < n_pieces; c++) {
                int w = 0;
                char *fit = ui_fit_text(pieces[c] ? pieces[c] : "", text_limit, &w, ell);
                free(pieces[c]);
                pieces[c] = fit ? fit : strdup("");
            }
        } else if ((ui->freeze_left > 0 || ui->freeze_right > 0) && !ui->no_hscroll) {
            int vn = 0;
            fitted = ui_apply_freeze(ui, text, text_limit, matched,
                                     &text_width, &view_pos, &vn);
            if (matched && matched->npos > 0) {
                view_match.pos = view_pos;
                view_match.npos = vn;
                shown = &view_match;
            }
        } else {
            int full_w = ui_str_width(text);
            const char *ell = ui->ellipsis ? ui->ellipsis : MM_ELLIPSIS;
            int ell_w = ui_str_width(ell);
            if (ell_w < 1) ell_w = MM_ELLIPSIS_WIDTH;
            /* A match that already sits in the left window stays put.
               Otherwise the window ends hscroll_off columns after the
               match, with an ellipsis on each side that was cut. */
            int prefix_keep = full_w > text_limit ? text_limit - ell_w : text_limit;
            if (prefix_keep < 0) prefix_keep = 0;
            if (scroll_match && full_w > text_limit && match_end_col > prefix_keep) {
                int right_edge = match_end_col + (ui->hscroll_off > 0 ? ui->hscroll_off : 0);
                if (right_edge > full_w) right_edge = full_w;
                bool right_ell = right_edge < full_w;
                bool left_ell = true;
                int content = text_limit - ell_w - (right_ell ? ell_w : 0);
                if (content < 1) content = 1;
                int start = right_edge - content;
                if (start <= 0) {
                    start = 0;
                    left_ell = false;
                    content = text_limit - (right_ell ? ell_w : 0);
                    if (content < 1) content = 1;
                }
                int skip_runes = 0;
                fitted = ui_slice_text(text, start, content, left_ell, right_ell,
                                       ell, &text_width, &skip_runes);
                if (fitted && matched->npos > 0) {
                    int ell_runes = left_ell ? ui_rune_count(ell) : 0;
                    view_pos = malloc(sizeof(int) * (size_t)matched->npos);
                    if (view_pos) {
                        int nn = 0;
                        int content_runes = ui_rune_count(fitted) - ell_runes -
                            (right_ell ? ui_rune_count(ell) : 0);
                        for (int k = 0; k < matched->npos; k++) {
                            int rel = matched->pos[k] - skip_runes;
                            if (rel < 0 || rel >= content_runes) continue;
                            view_pos[nn++] = rel + ell_runes;
                        }
                        view_match.pos = view_pos;
                        view_match.npos = nn;
                        shown = &view_match;
                    }
                }
            } else {
                fitted = ui_fit_text(text, text_limit, &text_width, ell);
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
        mm_color_t fg;
        if (!cur || (ui->plain_theme &&
                     (ui->theme.current.color == MM_COL_DEFAULT ||
                      ui->theme.current.color == MM_COL_UNDEFINED)))
            fg = ui_theme_color(ui, ui->theme.fg, MM_COL_DEFAULT);
        else
            fg = ui_theme_color(ui, ui->theme.current, MM_THEME_CURRENT);
        mm_color_t bg = cur ? ui_theme_color(ui, ui->theme.dark_bg, MM_THEME_DARK_BG)
                            : MM_COL_DEFAULT;
        mm_attr_t attr = line_attr | (raw_nonmatch ? MM_DIM : 0);
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
        const char *gutter = raw_nonmatch
                                 ? (ui->gutter_raw ? ui->gutter_raw
                                                   : (ui->no_unicode ? " " : "\u2596"))
                                 : (ui->gutter ? ui->gutter
                                               : (ui->no_unicode ? " " : "\u258c"));
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
                             : (cur ? ui_theme_color(ui, ui->theme.fg, pfg)
                                    : MM_COL_DEFAULT);
        /* The current line shows the pointer, the other lines the
           gutter character. Continuations of a multi-line item reuse it. */
        char jump_ptr[2] = {0, 0};
        const char *ptr = cur ? ((ui->pointer) ? ui->pointer
                                                : (ui->no_unicode ? " " : "\u258c"))
                              : gutter;
        if (ui->jump_mode && (size_t)block < strlen(jump_labels)) {
            jump_ptr[0] = jump_labels[block];
            ptr = jump_ptr;
        }
        /* A selected multi-line item uses the three-part marker. */
        const char *ml_top = ui->no_unicode ? "." : "\u257b";
        const char *ml_mid = ui->no_unicode ? "|" : "\u2503";
        const char *ml_bot = ui->no_unicode ? "'" : "\u2579";
        bool ml_selected = break_lines && n_pieces > 1 && sel && ui->multi;
        const char *first_mark = ml_selected ? ml_top : mark;
        if (visible) {
            mm_window_cprint(listwin, pfg, bg, pointer_attr, ptr);
            mm_window_cprint(listwin, mfg, bg, 0, first_mark);
        }
        if (visible && ui->ansi) {
            mm_renderer_stderr(listwin->r, fitted ? fitted : text);
        } else if (visible && n_pieces > 0) {
            /* The wrapped lines: the first one is here, the continuations
               are drawn on the lines below. */
            for (int c = 0; c < n_pieces; c++) {
                if (screen_y + c < list_first || screen_y + c > list_last) continue;
                if (c > 0) {
                    const char *cont_ptr;
                    if (!break_lines)
                        cont_ptr = (cur && ui->pointer) ? ui->pointer : "\u258c";
                    else if (ui->jump_mode)
                        cont_ptr = gutter;
                    else
                        cont_ptr = ptr;
                    const char *cont_mark = mark;
                    if (ml_selected)
                        cont_mark = (c == n_pieces - 1) ? ml_bot : ml_mid;
                    mm_window_move(listwin, screen_y + c, area_x + content_x + list_border_inset);
                    mm_window_cprint(listwin, pfg, bg, pointer_attr, cont_ptr);
                    mm_window_cprint(listwin, mfg, bg, 0, cont_mark);
                    /* The wrap sign uses the style of the line with the
                       dim attribute added (fzf's behaviour). A multi-line
                       item has no wrap sign between its own lines. */
                    if (!break_lines)
                        mm_window_cprint(listwin, fg, bg, attr | MM_DIM, sign);
                }
                /* The matched offsets of this piece. */
                mm_match_pos_t sub = { NULL, 0 };
                int *subpos = NULL;
                int nsub = 0;
                int piece_runes = break_lines ? ui_rune_count(pieces[c]) : 0;
                if (matched && matched->npos > 0 && matched->pos) {
                    subpos = malloc(sizeof(int) * (size_t)matched->npos);
                    if (subpos) {
                        for (int k = 0; k < matched->npos; k++) {
                            int p = matched->pos[k] - piece_off[c];
                            if (p < 0) continue;
                            if (break_lines && p >= piece_runes) continue;
                            subpos[nsub++] = p;
                        }
                    }
                }
                sub.pos = subpos;
                sub.npos = nsub;
                ui_print_item(listwin, ui, pieces[c] ? pieces[c] : "", &sub, fg, bg, attr, cur);
                free(subpos);
                /* Clear the rest of this line.  Every line is cleared on its
                   own, so that the spaces never run past the edge of the
                   row. */
                int used = 2 + ((c > 0 && !break_lines) ? sign_width : 0) +
                           ui_str_width(pieces[c] ? pieces[c] : "");
                mm_color_t clear_bg = (cur && ui->highlight_line) ? bg : MM_COL_DEFAULT;
                ui_clear_to_eol(listwin, area_x + area_w - 1,
                                area_x + content_x + list_border_inset + used, clear_bg);
            }
        } else if (visible) {
            ui_print_item(listwin, ui, fitted ? fitted : text, shown, fg, bg, attr, cur);
            mm_color_t clear_bg = (cur && ui->highlight_line) ? bg : MM_COL_DEFAULT;
            ui_clear_to_eol(listwin, area_x + area_w - 1,
                            area_x + content_x + list_border_inset + 2 + text_width, clear_bg);
        }
        free(fitted);
        free(view_pos);
        free(single_line_text);
        free(display_text);
        if (pieces) {
            for (int c = 0; c < n_pieces; c++) free(pieces[c]);
            free(pieces);
        }
        free(piece_off);

        /* Scrollbar, drawn in the last column of every visible row of the
           item. The range is in list-area rows, the same coordinates fzf
           uses for printBar. */
        if (!ui->no_scrollbar && bar_len > 0) {
            int col = b_right ? area_x + area_w - 2 : area_x + area_w - 1;
            for (int screen_line = screen_y; screen_line <= block_last; screen_line++) {
                if (screen_line < list_first || screen_line > list_last) continue;
                int row_in_list = screen_line - list_first;
                if (row_in_list >= bar_start && row_in_list < bar_start + bar_len) {
                    mm_window_move(listwin, screen_line, col);
                    mm_window_cprint(listwin, ui_theme_color(ui, ui->theme.scrollbar, MM_THEME_BORDER),
                                     MM_COL_DEFAULT, 0,
                                     ui->no_unicode ? "|" : "\u2502");
                } else {
                    mm_window_move(listwin, screen_line, col);
                    mm_window_cprint(listwin, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, " ");
                }
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
        if (draw_header_box) {
            int hshape = ui_resolve_header_shape(ui);
            const mm_border_glyphs_t *hsg = ui->no_unicode
                ? mm_border_glyphs_ascii(hshape) : mm_border_glyphs(hshape);
            bool ht, hb, edge_l, edge_r;
            mm_border_sides(hshape, &ht, &hb, &edge_l, &edge_r);
            int box_top = ht ? header_row - 1 : header_row;
            int box_bottom = hb ? header_row + header_rows : header_row + header_rows - 1;
            if (box_bottom < box_top) box_bottom = box_top;
            ui_draw_section_border(listwin, hsg ? hsg : hg, bcol, left, right,
                                   box_top, box_bottom, hshape);
        }
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
    const char *pr2 = ui->no_input ? "" : (ui->prompt ? ui->prompt : "> ");
    mm_renderer_move(r, prompt_row,
                     area_x + content_x + ui_str_width(pr2) +
                     (ui->no_input ? 0 : ui_query_cursor_width(ui)));

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
    if (entry.color != MM_COL_UNDEFINED && entry.color != MM_COL_DEFAULT)
        return entry.color;
    /* A plain base has no dark-theme fallbacks; only explicit colours show. */
    if (ui->plain_theme) return MM_COL_DEFAULT;
    return fallback;
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
                                  int content_h, int nlines, int offset) {
    (void)x;
    (void)y;
    (void)w;
    if (nlines <= content_h || content_h <= 0) return;
    int bar_len = content_h * content_h / nlines;
    if (bar_len < 1) bar_len = 1;
    int max_offset = nlines - content_h;
    if (max_offset < 1) max_offset = 1;
    if (offset < 0) offset = 0;
    if (offset > max_offset) offset = max_offset;
    int bar_start = (content_h - bar_len) * offset / max_offset;
    if (bar_start > content_h - bar_len) bar_start = content_h - bar_len;
    if (bar_start < 0) bar_start = 0;
    /* Rewrite the whole column. A scroll moves the thumb, and the cells it
       leaves have to become spaces. fzf does this in renderPreviewScrollbar;
       drawing only the thumb leaves the previous bar on the next page. */
    const char *glyph = ui->no_unicode ? "|" : "\u2502";
    mm_color_t scol = ui_theme_color(ui, ui->theme.scrollbar, MM_THEME_BORDER);
    for (int i = 0; i < content_h; i++) {
        mm_window_move(win, content_y + i, content_x + content_w);
        mm_window_cprint(win, scol, MM_COL_DEFAULT, 0,
                         (i >= bar_start && i < bar_start + bar_len) ? glyph : " ");
    }
    char info[32];
    int shown = offset + 1;
    if (shown > nlines) shown = nlines;
    int n = snprintf(info, sizeof(info), "%d/%d", shown, nlines);
    if (n > 0 && content_w - 2 - n >= 0) {
        mm_window_move(win, content_y, content_x + content_w - n);
        mm_window_cprint(win, ui_theme_color(ui, ui->theme.info, MM_THEME_INFO),
                         MM_COL_DEFAULT, 0, info);
    }
}

/* Clip one preview line the way fzf's trimRight does, then expand tabs.
   The limit is a display width. A tab occupies the columns up to the next
   tab stop, and a rune that would pass the limit is left off. */
static char *ui_preview_clip(const char *line, size_t len, int limit, int tabstop) {
    if (tabstop < 1) tabstop = 8;
    while (len && (line[len - 1] == '\n' || line[len - 1] == '\r')) len--;
    size_t start = 0;
    while (start < len && line[start] == '\r') start++;
    line += start;
    len -= start;
    if (limit < 1 || len == 0) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }
    int width = 0;
    size_t i = 0, cut = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(line + i, len - i, &cp);
        if (used == 0) used = 1;
        int cw = (cp == L'\t') ? tabstop - (width % tabstop) : mm_runewidth(cp);
        if (cw < 1) cw = 1;
        if (width + cw > limit) break;
        width += cw;
        i += used;
        cut = i;
    }
    char *out = malloc(len * (size_t)tabstop + 1);
    if (!out) return NULL;
    size_t j = 0;
    width = 0;
    i = 0;
    while (i < cut) {
        wchar_t cp;
        size_t used = mm_utf8_decode(line + i, cut - i, &cp);
        if (used == 0) { out[j++] = line[i++]; continue; }
        if (cp == L'\t') {
            int spaces = tabstop - (width % tabstop);
            for (int s = 0; s < spaces; s++) out[j++] = ' ';
            width += spaces;
        } else {
            memcpy(out + j, line + i, used);
            j += used;
            width += mm_runewidth(cp);
        }
        i += used;
    }
    out[j] = '\0';
    return out;
}

static void ui_draw_preview(mm_ui_t *ui, mm_renderer_t *r, mm_window_t *win,
                            int x, int y, int w, int h) {
    (void)r;
    if (w <= 0 || h <= 0) return;
    int shape = ui_preview_shape(ui);
    bool side_top, side_bottom, side_left, side_right;
    mm_border_sides(shape, &side_top, &side_bottom, &side_left, &side_right);
    bool any_border = side_top || side_bottom || side_left || side_right;
    bool full_border = side_top && side_bottom && side_left && side_right;
    bool left_only = side_left && !side_right && !side_top && !side_bottom;
    const mm_border_glyphs_t *g = NULL;
    if (any_border) {
        g = ui->no_unicode ? mm_border_glyphs_ascii(shape) : mm_border_glyphs(shape);
        if (!g) any_border = false;
    }
    mm_color_t bcol = ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER);

    bool outer_top, outer_bottom, outer_left, outer_right;
    mm_border_sides(ui->border, &outer_top, &outer_bottom, &outer_left, &outer_right);
    /* Inside an outer border, a preview with no left edge keeps the outer
       padding column. border-left draws its own padding instead. */
    int gutter = (!side_left && outer_left) ? 1 : 0;
    int content_x = x + (side_left ? 2 : gutter);
    int content_y = y + (side_top ? 1 : 0);
    /* A side border reserves its glyph and a padding column. */
    int content_w = w - (side_left ? 2 : 0) - (side_right ? 2 : 0) - gutter;
    /* No right edge: fzf reserves that column for the preview scrollbar. */
    if (!side_right) content_w -= 1;
    if (content_w < 1) content_w = 1;
    int content_h = h - (side_top ? 1 : 0) - (side_bottom ? 1 : 0);
    ui->preview_page_size = content_h > 0 ? content_h : 1;

    /* With a gap the reference pads the preview content with the border
       colour. */
    if (side_left && g) {
        /* The reference draws the space between the border and the content
           in the border colour. */
        mm_window_move(win, content_y, x + 1);
        mm_window_cprint(win, ui_theme_color(ui, ui->theme.border, MM_THEME_BORDER),
                         MM_COL_DEFAULT, 0, " ");
    }

    if (full_border && g) {
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
    } else if (left_only && g) {
        /* border-left: draw the divider without top/bottom corners or a
           border on the far side of the preview. */
        for (int i = 0; i < h; i++) {
            mm_window_move(win, y + i, x);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, g->left);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, " ");
        }
    } else if (any_border && g && (side_top || side_bottom)) {
        /* border-top / border-bottom: one horizontal edge, no side frame. */
        for (int which = 0; which < 2; which++) {
            bool on = which == 0 ? side_top : side_bottom;
            if (!on) continue;
            int row = which == 0 ? y : (y + h - 1);
            const char *mid = which == 0 ? g->top : g->bottom;
            int line_x = x + gutter;
            int line_w = w - gutter;
            if (line_w < 1) line_w = 1;
            char *line = malloc((size_t)line_w * 4 + 8);
            if (!line) break;
            size_t o = 0;
            size_t glen = strlen(mid);
            for (int i = 0; i < line_w && glen > 0; i++) {
                memcpy(line + o, mid, glen);
                o += glen;
            }
            line[o] = 0;
            mm_window_move(win, row, line_x);
            mm_window_cprint(win, bcol, MM_COL_DEFAULT, 0, line);
            free(line);
            if (ui->preview_label && *ui->preview_label) {
                int label_width = ui_str_width(ui->preview_label);
                if (label_width > 0 && label_width <= line_w) {
                    int label_row = ui->preview_label_bottom ? y + h - 1 : row;
                    if (!side_top && side_bottom) label_row = y + h - 1;
                    int label_x = line_x + (line_w - label_width) / 2;
                    if (ui->preview_label_pos > 0)
                        label_x = line_x + ui->preview_label_pos - 1;
                    else if (ui->preview_label_pos < 0)
                        label_x = line_x + line_w - label_width + ui->preview_label_pos + 1;
                    if (label_x < line_x) label_x = line_x;
                    mm_window_move(win, label_row, label_x);
                    mm_window_cprint(win,
                        ui_theme_color(ui, ui->theme.border_label, 145),
                        MM_COL_DEFAULT, 0, ui->preview_label);
                }
            }
        }
    }

    /* The content area is cleared, so that the leftovers of the previous
       frame do not show through. */
    if (any_border) {
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
        int skipped = 0;
        while (*p && skipped < ui->preview_offset) {
            const char *nl = strchr(p, '\n');
            if (!nl) { p += strlen(p); break; }
            p = nl + 1;
            skipped++;
        }
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
                size_t word_break = SIZE_MAX;
                while (cut < len) {
                    wchar_t cp;
                    size_t used = mm_utf8_decode(p + cut, len - cut, &cp);
                    if (used == 0) used = 1;
                    int cw = mm_runewidth(cp);
                    if (cols + cw > avail) break;
                    if (ui->preview_win.wrap_word && cp == L' ')
                        word_break = cut;
                    cols += cw;
                    cut += used;
                }
                bool broke_word = false;
                if (ui->preview_win.wrap_word && cut < len &&
                    word_break != SIZE_MAX && word_break > off) {
                    cut = word_break;
                    broke_word = true;
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
                if (broke_word) {
                    while (off < len && p[off] == ' ') off++;
                }
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
                                 content_h, nlines, ui->preview_offset);
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
        int skipped = 0;
        while (*p && skipped < ui->preview_offset) {
            const char *nl = strchr(p, '\n');
            if (!nl) { p += strlen(p); break; }
            p = nl + 1;
            skipped++;
        }
        while (*p && row < content_y + content_h) {
            const char *nl = strchr(p, '\n');
            size_t len = nl ? (size_t)(nl - p) : strlen(p);
            char *line = ui_preview_clip(p, len, content_w, ui->tabstop);
            if (!line) break;
            mm_window_move(win, row, content_x);
            mm_window_cprint(win, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, line);
            ui_preview_pad(win, ui, content_w, ui_str_width(line));
            free(line);
            row++;
            if (!nl) break;
            p = nl + 1;
        }

        ui_preview_scrollbar(ui, win, x, y, w, content_x, content_y, content_w,
                             content_h, nlines, ui->preview_offset);
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

    char *track_key = NULL;
    if (ui->id_nth_spec && (ui->track || ui->track_current) &&
        ui->results && ui->cursor >= 0 && ui->cursor < ui->nresults)
        track_key = ui_track_key_for(ui, ui->results[ui->cursor]);

    char **selected_keys = NULL;
    int selected_key_count = 0;
    if (ui->id_nth_spec && ui->multi && ui->nselected > 0 &&
        ui->sel_order && ui->sel_order_n > 0) {
        selected_keys = calloc((size_t)ui->sel_order_n, sizeof(*selected_keys));
        if (selected_keys) {
            for (int i = 0; i < ui->sel_order_n; i++) {
                int item = ui->sel_order[i];
                if (item < 0 || item >= ui->count || !ui->selected[item])
                    continue;
                char *key = ui_track_key_for(ui, item);
                if (key) selected_keys[selected_key_count++] = key;
            }
        }
    }

    char **lines = NULL;
    int n = 0, cap = 0;
    bool failed = false;
    char *line = NULL;
    size_t lcap = 0;
    ssize_t len;
    while ((len = getline(&line, &lcap, fp)) != -1) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r'))
            line[--len] = 0;
        if (n == cap) {
            if (cap > INT_MAX / 2) {
                failed = true;
                break;
            }
            int new_cap = cap ? cap * 2 : 64;
            if ((size_t)new_cap > SIZE_MAX / sizeof(*lines)) {
                failed = true;
                break;
            }
            char **grown = realloc(lines, (size_t)new_cap * sizeof(*lines));
            if (!grown) {
                failed = true;
                break;
            }
            lines = grown;
            cap = new_cap;
        }
        lines[n] = malloc((size_t)len + 1);
        if (!lines[n]) {
            failed = true;
            break;
        }
        memcpy(lines[n], line, (size_t)len + 1);
        n++;
    }
    if (ferror(fp)) failed = true;
    free(line);
    (void)pclose(fp);

    if (failed) {
        free(track_key);
        ui_free_identity_keys(selected_keys, selected_key_count);
        for (int i = 0; i < n; i++) free(lines[i]);
        free(lines);
        return;
    }
    /* An empty command output is a valid empty result set. */
    if (!lines) {
        lines = calloc(1, sizeof(*lines));
        if (!lines) {
            free(track_key);
            ui_free_identity_keys(selected_keys, selected_key_count);
            return;
        }
    }

    mm_item_t *items = calloc((size_t)(n ? n : 1), sizeof(*items));
    bool *selected = calloc((size_t)(n ? n : 1), sizeof(*selected));
    bool *excluded = calloc((size_t)(n ? n : 1), sizeof(*excluded));
    if (!items || !selected || !excluded) {
        free(track_key);
        ui_free_identity_keys(selected_keys, selected_key_count);
        free(items);
        free(selected);
        free(excluded);
        for (int i = 0; i < n; i++) free(lines[i]);
        free(lines);
        return;
    }
    int initialized = 0;
    for (int i = 0; i < n; i++) {
        if (!mm_item_init(&items[i], lines[i], i)) {
            mm_item_free(&items[i]);
            failed = true;
            break;
        }
        initialized++;
        mchars_decode(&items[i].text);
    }
    if (failed) {
        free(track_key);
        ui_free_identity_keys(selected_keys, selected_key_count);
        for (int i = 0; i < initialized; i++) mm_item_free(&items[i]);
        free(items);
        free(selected);
        free(excluded);
        for (int i = 0; i < n; i++) free(lines[i]);
        free(lines);
        return;
    }

    /* Commit the replacement only after every new item is ready.  Keep the
       old result indices until ui_search has used them for --track. */
    for (int i = 0; i < ui->count; i++)
        mm_item_free(&ui->items[i]);
    ui_free_line_storage(ui);
    ui_free_rune_slabs(ui);
    ui_qcache_clear(ui);
    ui->results_item_count = 0;
    ui->has_non_ascii = false;
    ui->lazy_highlight = false;
    free(ui->items);
    free(ui->lines);
    free(ui->selected);
    free(ui->excluded);
    ui->lines = lines;
    ui->items = items;
    ui->selected = selected;
    ui->excluded = excluded;
    ui->has_excluded = false;
    ui->plain_results = false;
    ui->count = n;
    ui->lines_cap = n;
    ui->nselected = 0;
    ui->sel_order_n = 0;
    if (selected_key_count > 0)
        ui_restore_selected_keys(ui, selected_keys, selected_key_count);
    ui_free_identity_keys(selected_keys, selected_key_count);
    ui_rebuild_header_lines(ui);
    for (int item = 0; item < ui->count && item < ui->header_line_count; item++)
        ui_deselect_item(ui, item);
    if (!ui->track && !ui->track_current) ui->cursor = 0;
    ui->offset = 0;
    free(ui->pending_track_key);
    ui->pending_track_key = track_key;
    ui_search(ui);
}

/* A negative --min-height is the "N+" form: N plus the rows the chrome
   needs. fzf's default is 10+. An explicit non-negative value is a flat floor. */
static int ui_text_line_count(const char *text) {
    if (!text || !*text) return 0;
    int n = 1;
    for (const char *p = text; *p; p++)
        if (*p == '\n') n++;
    if (text[strlen(text) - 1] == '\n') n--;
    return n;
}

static bool ui_height_is_percent(const char *spec) {
    if (!spec) return false;
    const char *p = spec;
    if (*p == '~' || *p == '-') p++;
    if (*p < '0' || *p > '9') return false;
    while (*p >= '0' && *p <= '9') p++;
    return *p == '%';
}

/* Whether the info section has no separator row of its own. */
static bool ui_no_separator_line(const mm_ui_t *ui) {
    if (ui->no_input || ui->info_inline) return true;
    int shape = ui->input_border_set ? ui->input_border : MM_BORDER_NONE;
    if (shape == MM_BORDER_LINE)
        shape = ui->reverse ? MM_BORDER_BOTTOM : MM_BORDER_TOP;
    bool top, bottom, left, right;
    mm_border_sides(shape, &top, &bottom, &left, &right);
    bool faces = ui->reverse ? bottom : top;
    bool sep = ui->separator_default ? !faces : (ui->separator && ui->separator[0]);
    if (!ui->info || ui->info_inline_right) return !sep;
    return false;
}

/* Turn the "N+" minimum into a row count. Percent heights only; an absolute
   --height keeps the value the user wrote. */
static void ui_expand_min_height(mm_ui_t *ui) {
    if (ui->min_height >= 0) return;
    if (!ui_height_is_percent(ui->height_spec)) return;
    int h = -ui->min_height;
    h += ui_border_line_count(ui->border);
    h += ui_border_line_count(ui->list_border_set ? ui->list_border : MM_BORDER_NONE);
    if (!ui->no_input) {
        int input_shape = ui->input_border_set ? ui->input_border : MM_BORDER_NONE;
        h += 1 + ui_border_line_count(input_shape);
        if (!ui_no_separator_line(ui)) h++;
    }
    if (ui->header && ui->header[0]) {
        int shape = ui->header_border_set ? ui->header_border : MM_BORDER_NONE;
        h += ui_border_line_count(shape) + ui_text_line_count(ui->header);
    }
    if (ui->header_line_count > 0) {
        int shape = ui->header_border_set ? ui->header_border : MM_BORDER_NONE;
        if (ui->header_lines_border_set && ui->header_lines_border != MM_BORDER_NONE)
            shape = ui->header_lines_border;
        h += ui_border_line_count(shape) + ui->header_line_count;
    }
    if (ui->preview && !ui->preview_win.hidden &&
        ui->preview_win.pos == MM_PREVIEW_TOP) {
        int shape = ui_preview_shape(ui);
        if (shape == MM_BORDER_LINE) shape = MM_BORDER_TOP;
        h += ui_border_line_count(shape) + 10;
    }
    h += ui->margin * 2 + ui->padding * 2;
    if (h < 1) h = 1;
    ui->min_height = h;
}

/* Evaluate a --height specification.  Returns the number of rows the inline
   window may use, or 0 for the full screen.  A percentage is a fraction of
   the terminal. --min-height is a floor, not extra rows added on top of
   that fraction. The default floor already includes the window chrome. */
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
    int floor = min_height < 0 ? -min_height : min_height;
    if (h < floor) h = floor;
    if (h > rows) h = rows;
    return h;
}

/* Start (or restart) the preview command for the current item. */
static bool ui_command_append(char **out, size_t *length, size_t *capacity,
                              const char *text, size_t n);

/* Remember which item and query the preview command was started for.
   A later key that does not change either must not run the command again:
   preview-page-down would lose the offset it just applied. */
static void ui_preview_note(mm_ui_t *ui) {
    int idx = -1;
    if (ui->nresults > 0 && ui->cursor >= 0 && ui->cursor < ui->nresults)
        idx = ui->results[ui->cursor];
    ui->preview_item = idx;
    free(ui->preview_query);
    ui->preview_query = strdup(ui->query_buf ? ui->query_buf : "");
}

static void ui_preview_restart(mm_ui_t *ui) {
    if (!ui->preview) return;
    int idx = ui->nresults > 0 ? ui->results[ui->cursor] : -1;
    if (idx < 0 || idx >= ui->count) {
        ui_preview_note(ui);
        return;
    }

    char *raw = NULL, *shell = NULL;
    size_t raw_len = 0, raw_cap = 0, shell_len = 0, shell_cap = 0;
    int selected_count = 0;
    bool allocation_failed = false;
    bool have_selection = ui->selected && ui->nselected > 0 &&
                          ui->sel_order && ui->sel_order_n > 0;
    bool *seen = have_selection ? calloc((size_t)ui->count, sizeof(*seen)) : NULL;
    if (have_selection && !seen) allocation_failed = true;

    for (int i = 0; !allocation_failed && i < (have_selection ? ui->sel_order_n : 1); i++) {
        int item = have_selection ? ui->sel_order[i] : idx;
        if (item < 0 || item >= ui->count) continue;
        if (have_selection && (!ui->selected[item] || seen[item])) continue;
        if (seen) seen[item] = true;

        const char *line = ui->lines[item] ? ui->lines[item] : "";
        size_t line_len = strlen(line);
        if (!ui_command_append(&raw, &raw_len, &raw_cap, line, line_len) ||
            !ui_command_append(&raw, &raw_len, &raw_cap, "\n", 1)) {
            allocation_failed = true;
            break;
        }

        char *quoted = mm_shell_quote(line);
        if (!quoted) {
            allocation_failed = true;
            break;
        }
        if (selected_count > 0 &&
            !ui_command_append(&shell, &shell_len, &shell_cap, " ", 1)) {
            free(quoted);
            allocation_failed = true;
            break;
        }
        bool appended = ui_command_append(&shell, &shell_len, &shell_cap,
                                          quoted, strlen(quoted));
        free(quoted);
        if (!appended) {
            allocation_failed = true;
            break;
        }
        selected_count++;
    }

    free(seen);
    if (allocation_failed || selected_count == 0) {
        free(raw);
        free(shell);
        raw = strdup("");
        shell = strdup("");
    }
    mm_preview_set_selected(ui->preview, raw ? raw : "", shell ? shell : "");
    free(raw);
    free(shell);
    ui->preview_offset = 0;
    mm_preview_start(ui->preview, ui->lines[idx], ui->query_buf, idx);
    ui->preview_fresh = true;
    ui_preview_note(ui);
}

/* A fast preview (printf, ls) has usually exited before the repaint. Read
   it now so that paint shows the new text. A slow command is left for the
   poll loop; the wait is only the time until output arrives, capped. */
static void ui_preview_catch_up(mm_ui_t *ui) {
    if (!ui->preview_fresh || !ui->preview || ui->preview_hidden) return;
    ui->preview_fresh = false;
    mm_preview_drain(ui->preview, 8);
}

/* Re-run the preview only when the current item or the query changed.
   Scrolling, layout keys, and other actions keep the existing output. */
static void ui_preview_sync(mm_ui_t *ui) {
    if (!ui->preview || ui->preview_hidden) return;
    int idx = -1;
    if (ui->nresults > 0 && ui->cursor >= 0 && ui->cursor < ui->nresults)
        idx = ui->results[ui->cursor];
    const char *query = ui->query_buf ? ui->query_buf : "";
    if (ui->preview_query && idx == ui->preview_item &&
        strcmp(ui->preview_query, query) == 0)
        return;
    ui_preview_restart(ui);
}


/* --- selection output --- */
/* Mark an item as selected and remember the order of the selection. */
static void ui_select_item(mm_ui_t *ui, int item) {
    if (!ui->selected || ui->selected[item]) return;
    if (ui->sel_order_n + 1 > ui->sel_order_cap) {
        if (ui->sel_order_cap > INT_MAX / 2) return;
        int ncap = ui->sel_order_cap ? ui->sel_order_cap * 2 : 16;
        if ((size_t)ncap > SIZE_MAX / sizeof(*ui->sel_order)) return;
        int *n = realloc(ui->sel_order, sizeof(*ui->sel_order) * (size_t)ncap);
        if (!n) return;
        ui->sel_order = n;
        ui->sel_order_cap = ncap;
    }
    ui->selected[item] = true;
    ui->nselected++;
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

static bool ui_emit_item(mm_ui_t *ui, FILE *out, int item, char sep) {
    if (ui->accept_nth_spec) {
        char *accepted = mm_accept_nth_expand(ui->accept_nth_spec,
                                              ui->lines[item], ui->delim, item);
        if (!accepted) return false;
        fputs(accepted, out);
        free(accepted);
    } else {
        fputs(ui->lines[item], out);
    }
    fputc(sep, out);
    return !ferror(out);
}

static bool ui_print_selection(mm_ui_t *ui, FILE *out) {
    char sep = ui->print0 ? '\0' : '\n';
    if (ui->nselected > 0 && ui->sel_order) {
        /* The selected items are printed in the order they were selected. */
        for (int i = 0; i < ui->sel_order_n; i++) {
            int item = ui->sel_order[i];
            if (item < 0 || item >= ui->count) continue;
            if (!ui_emit_item(ui, out, item, sep)) return false;
        }
        return true;
    } else if (ui->nresults > 0) {
        /* Nothing is selected: the current item is printed. */
        int item_idx = ui->results[ui->cursor];
        return ui_emit_item(ui, out, item_idx, sep);
    }
    return false;
}

/* Append bytes to a command under construction, growing it safely. */
static bool ui_command_append(char **out, size_t *length, size_t *capacity,
                              const char *text, size_t n) {
    if (n > SIZE_MAX - *length - 1) return false;
    size_t need = *length + n + 1;
    if (need > *capacity) {
        size_t cap = *capacity ? *capacity : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        char *grown = realloc(*out, cap);
        if (!grown) return false;
        *out = grown;
        *capacity = cap;
    }
    if (n) memcpy(*out + *length, text, n);
    *length += n;
    (*out)[*length] = '\0';
    return true;
}

/* Build shell-quoted arguments in selection order, or in result order
   for the matched-item placeholder. */
static char *ui_expand_item_list(mm_ui_t *ui, bool matched) {
    char *out = NULL;
    size_t length = 0, capacity = 0;
    int appended = 0;

    if (matched) {
        for (int i = 0; i < ui->nresults; i++) {
            int item = ui->results[i];
            if (item < 0 || item >= ui->count) continue;
            char *quoted = mm_shell_quote(ui->lines[item]);
            if (!quoted) goto fail;
            if (appended > 0 &&
                !ui_command_append(&out, &length, &capacity, " ", 1)) {
                free(quoted);
                goto fail;
            }
            bool ok = ui_command_append(&out, &length, &capacity,
                                        quoted, strlen(quoted));
            free(quoted);
            if (!ok) goto fail;
            appended++;
        }
    } else if (ui->nselected > 0 && ui->sel_order &&
               ui->sel_order_n > 0) {
        for (int i = 0; i < ui->sel_order_n; i++) {
            int item = ui->sel_order[i];
            if (item < 0 || item >= ui->count || !ui->selected[item]) continue;
            char *quoted = mm_shell_quote(ui->lines[item]);
            if (!quoted) goto fail;
            if (appended > 0 &&
                !ui_command_append(&out, &length, &capacity, " ", 1)) {
                free(quoted);
                goto fail;
            }
            bool ok = ui_command_append(&out, &length, &capacity,
                                        quoted, strlen(quoted));
            free(quoted);
            if (!ok) goto fail;
            appended++;
        }
    } else {
        int item = ui->nresults > 0 && ui->cursor >= 0 &&
                   ui->cursor < ui->nresults ? ui->results[ui->cursor] : -1;
        const char *value = item >= 0 && item < ui->count ? ui->lines[item] : "";
        char *quoted = mm_shell_quote(value);
        if (!quoted) goto fail;
        bool ok = ui_command_append(&out, &length, &capacity,
                                    quoted, strlen(quoted));
        free(quoted);
        if (!ok) goto fail;
    }

    if (!out) return strdup("");
    return out;

fail:
    free(out);
    return NULL;
}

/* Replace fzf command placeholders. Values inserted into the shell command
   are quoted as single arguments unless the placeholder explicitly requests
   raw text. */
/* Create a newline-delimited item file for {f}, {+f}, or {*f}.
   The returned shell word is quoted; the file remains owned by the UI until
   shutdown so asynchronous commands may finish reading it. */
static char *ui_expand_item_file(mm_ui_t *ui, int mode) {
    char *raw = NULL;
    size_t raw_len = 0, raw_cap = 0;

    if (mode == 2) {
        for (int i = 0; i < ui->nresults; i++) {
            int item = ui->results[i];
            if (item < 0 || item >= ui->count) continue;
            const char *line = ui->lines[item] ? ui->lines[item] : "";
            if (!ui_command_append(&raw, &raw_len, &raw_cap, line, strlen(line)) ||
                !ui_command_append(&raw, &raw_len, &raw_cap, "\n", 1))
                goto fail;
        }
    } else if (mode == 1 && ui->nselected > 0 &&
               ui->sel_order && ui->sel_order_n > 0) {
        for (int i = 0; i < ui->sel_order_n; i++) {
            int item = ui->sel_order[i];
            if (item < 0 || item >= ui->count || !ui->selected[item]) continue;
            const char *line = ui->lines[item] ? ui->lines[item] : "";
            if (!ui_command_append(&raw, &raw_len, &raw_cap, line, strlen(line)) ||
                !ui_command_append(&raw, &raw_len, &raw_cap, "\n", 1))
                goto fail;
        }
    } else {
        int item = ui->nresults > 0 && ui->cursor >= 0 &&
                   ui->cursor < ui->nresults ? ui->results[ui->cursor] : -1;
        if (item >= 0 && item < ui->count) {
            const char *line = ui->lines[item] ? ui->lines[item] : "";
            if (!ui_command_append(&raw, &raw_len, &raw_cap, line, strlen(line)) ||
                !ui_command_append(&raw, &raw_len, &raw_cap, "\n", 1))
                goto fail;
        }
    }

    const char *directory = getenv("TMPDIR");
    if (!directory || !*directory) directory = "/tmp";
    size_t dir_len = strlen(directory);
    if (dir_len > SIZE_MAX - 32) goto fail;
    size_t path_cap = dir_len + 32;
    char *path = malloc(path_cap);
    if (!path) goto fail;
    int written = snprintf(path, path_cap, "%s/cfzf-items-XXXXXX", directory);
    if (written < 0 || (size_t)written >= path_cap) {
        free(path);
        goto fail;
    }

    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        goto fail;
    }
    size_t offset = 0;
    while (offset < raw_len) {
        ssize_t n = write(fd, raw + offset, raw_len - offset);
        if (n > 0) {
            offset += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        close(fd);
        unlink(path);
        free(path);
        goto fail;
    }
    if (close(fd) != 0) {
        unlink(path);
        free(path);
        goto fail;
    }

    if (ui->placeholder_files_n == ui->placeholder_files_cap) {
        size_t next_cap = ui->placeholder_files_cap
                        ? ui->placeholder_files_cap * 2 : 8;
        if (next_cap < ui->placeholder_files_cap ||
            next_cap > SIZE_MAX / sizeof(*ui->placeholder_files)) {
            unlink(path);
            free(path);
            goto fail;
        }
        char **grown = realloc(ui->placeholder_files,
                               next_cap * sizeof(*ui->placeholder_files));
        if (!grown) {
            unlink(path);
            free(path);
            goto fail;
        }
        ui->placeholder_files = grown;
        ui->placeholder_files_cap = next_cap;
    }
    ui->placeholder_files[ui->placeholder_files_n++] = path;
    char *quoted = mm_shell_quote(path);
    free(raw);
    return quoted;

fail:
    free(raw);
    return NULL;
}

/* Remove temporary item files allocated after a caller's checkpoint. */
static void ui_cleanup_placeholder_files(mm_ui_t *ui, size_t first) {
    if (first > ui->placeholder_files_n) first = ui->placeholder_files_n;
    for (size_t i = first; i < ui->placeholder_files_n; i++) {
        if (ui->placeholder_files[i]) {
            (void)unlink(ui->placeholder_files[i]);
            free(ui->placeholder_files[i]);
            ui->placeholder_files[i] = NULL;
        }
    }
    ui->placeholder_files_n = first;
}

static char *ui_expand_command(mm_ui_t *ui, const char *cmd, bool force_plus) {
    char *out = NULL;
    size_t length = 0, capacity = 0;
    int idx = ui->nresults > 0 ? ui->results[ui->cursor] : -1;

    for (size_t i = 0; cmd[i]; i++) {
        /* A backslash escapes a recognized placeholder instead of expanding it. */
        if (cmd[i] == '\\' && cmd[i + 1] == '{') {
            size_t literal_len = 0;
            if (cmd[i + 2] == '}') literal_len = 2;
            else if (cmd[i + 2] == 'q' && cmd[i + 3] == '}') literal_len = 3;
            else if (cmd[i + 2] == 'n' && cmd[i + 3] == '}') literal_len = 3;
            else if ((cmd[i + 2] == '+' || cmd[i + 2] == '*') &&
                     cmd[i + 3] == '}') literal_len = 3;
            else if (cmd[i + 2] == 'f' && cmd[i + 3] == '}') literal_len = 3;
            else if ((cmd[i + 2] == '+' || cmd[i + 2] == '*') &&
                     cmd[i + 3] == 'f' && cmd[i + 4] == '}') literal_len = 4;
            if (literal_len) {
                if (!ui_command_append(&out, &length, &capacity,
                                       cmd + i + 1, literal_len))
                    goto fail;
                i += literal_len;
                continue;
            }
        }

        if (cmd[i] == '{' && cmd[i + 1] == '}') {
            if (force_plus) {
                char *items = ui_expand_item_list(ui, false);
                if (!items) goto fail;
                bool ok = ui_command_append(&out, &length, &capacity, items, strlen(items));
                free(items);
                if (!ok) goto fail;
            } else {
                const char *value = idx >= 0 && idx < ui->count ? ui->lines[idx] : "";
                char *quoted = mm_shell_quote(value);
                if (!quoted) goto fail;
                bool ok = ui_command_append(&out, &length, &capacity, quoted, strlen(quoted));
                free(quoted);
                if (!ok) goto fail;
            }
            i++;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'f' && cmd[i + 2] == '}') {
            char *file = ui_expand_item_file(ui, 0);
            if (!file) goto fail;
            bool ok = ui_command_append(&out, &length, &capacity, file, strlen(file));
            free(file);
            if (!ok) goto fail;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && (cmd[i + 1] == '+' || cmd[i + 1] == '*') &&
            cmd[i + 2] == 'f' && cmd[i + 3] == '}') {
            char *file = ui_expand_item_file(ui, cmd[i + 1] == '*' ? 2 : 1);
            if (!file) goto fail;
            bool ok = ui_command_append(&out, &length, &capacity, file, strlen(file));
            free(file);
            if (!ok) goto fail;
            i += 3;
            continue;
        }
        if (cmd[i] == '{' && (cmd[i + 1] == '+' || cmd[i + 1] == '*') &&
            cmd[i + 2] == '}') {
            char *items = ui_expand_item_list(ui, cmd[i + 1] == '*');
            if (!items) goto fail;
            bool ok = ui_command_append(&out, &length, &capacity, items, strlen(items));
            free(items);
            if (!ok) goto fail;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'q' && cmd[i + 2] == '}') {
            const char *query = ui->query_buf ? ui->query_buf : "";
            char *quoted = mm_shell_quote(query);
            if (!quoted) goto fail;
            bool ok = ui_command_append(&out, &length, &capacity, quoted, strlen(quoted));
            free(quoted);
            if (!ok) goto fail;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'n' && cmd[i + 2] == '}') {
            char num[32];
            int n = snprintf(num, sizeof(num), "%d", idx >= 0 ? idx : 0);
            if (n < 0 || (size_t)n >= sizeof(num) ||
                !ui_command_append(&out, &length, &capacity, num, (size_t)n))
                goto fail;
            i += 2;
            continue;
        }
        if (!ui_command_append(&out, &length, &capacity, cmd + i, 1))
            goto fail;
    }
    if (!out) return strdup("");
    return out;

fail:
    free(out);
    return NULL;
}

/* Run a command with the system shell, waiting for it to finish. */
static void ui_run_command(mm_ui_t *ui, const char *cmd, bool silent,
                            bool force_plus) {
    if (!cmd || !*cmd) return;
    size_t placeholder_checkpoint = ui->placeholder_files_n;
    char *expanded = ui_expand_command(ui, cmd, force_plus);
    if (!expanded) {
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return;
    }
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
    ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
    if (!silent) {
        /* Re-enter the alternate screen and repaint. */
        mm_renderer_init(ui->renderer, ui->renderer->ttyin, ui->renderer->ttyout,
                         ui->mouse, 8, true, true);
    }
}

/* Apply action-chain text produced by transform or bg-transform. */
static bool ui_apply_transform_output(mm_ui_t *ui, char *output, size_t len) {
    size_t first = 0;
    while (first < len && isspace((unsigned char)output[first])) first++;
    while (len > first && isspace((unsigned char)output[len - 1])) len--;
    if (first > 0 && len > first) memmove(output, output + first, len - first);
    len -= first;
    output[len] = '\0';

    bool keep_running = true;
    if (len > 0 && len <= SIZE_MAX - 8) {
        char *spec = malloc(len + 8);
        if (spec) {
            memcpy(spec, "ctrl-x:", 7);
            memcpy(spec + 7, output, len + 1);
            mm_keymap_t actions = {0};
            const char *error = NULL;
            if (mm_bind_parse(&actions, spec, false, &error)) {
                for (int i = 0; i < actions.n; i++) {
                    mm_binding_t action = actions.items[i];
                    if (!ui_run_action(ui, &action)) {
                        keep_running = false;
                        break;
                    }
                }
            }
            mm_keymap_free(&actions);
            free(spec);
        }
    }
    return keep_running;
}

/* Run a command and return its stdout. *out_len is the byte count. */
static char *ui_capture_output(mm_ui_t *ui, const char *cmd, size_t *out_len) {
    *out_len = 0;
    if (!cmd || !*cmd) return calloc(1, 1);
    size_t placeholder_checkpoint = ui->placeholder_files_n;
    char *expanded = ui_expand_command(ui, cmd, false);
    if (!expanded) {
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return NULL;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        free(expanded);
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return NULL;
    }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        if (ui->shell && *ui->shell) execl(ui->shell, ui->shell, "-c", expanded, (char *)NULL);
        else execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    }
    free(expanded);
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return NULL;
    }
    size_t len = 0, cap = 256;
    char *output = malloc(cap);
    if (!output) {
        close(fds[0]);
        while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return NULL;
    }
    for (;;) {
        if (len + 128 + 1 > cap) {
            if (cap > SIZE_MAX / 2) break;
            char *grown = realloc(output, cap * 2);
            if (!grown) break;
            output = grown; cap *= 2;
        }
        ssize_t n = read(fds[0], output + len, cap - len - 1);
        if (n > 0) { len += (size_t)n; continue; }
        if (n < 0 && errno == EINTR) continue;
        break;
    }
    close(fds[0]);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
    output[len] = '\0';
    ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
    *out_len = len;
    return output;
}

static void ui_trim_first_line(char *output, size_t *len) {
    size_t n = 0;
    while (n < *len && output[n] != '\n') n++;
    if (n > 0 && output[n - 1] == '\r') n--;
    output[n] = '\0';
    *len = n;
}

/* Run transform(command), capture stdout, then execute the returned action chain. */
static bool ui_transform(mm_ui_t *ui, const char *cmd) {
    size_t len = 0;
    char *output = ui_capture_output(ui, cmd, &len);
    if (!output) return true;
    bool keep = ui_apply_transform_output(ui, output, len);
    free(output);
    return keep;
}

/* Start a transform without blocking input. Only one job is active at a time. */
static void ui_bg_transform_cancel(mm_ui_t *ui);

static void ui_bg_transform_start(mm_ui_t *ui, const char *cmd,
                                  mm_action_t target, bool first_line) {
    ui->bg_transform_target = target;
    ui->bg_transform_first_line = first_line;
    if (!cmd || !*cmd) return;
    ui_bg_transform_cancel(ui);
    size_t placeholder_checkpoint = ui->placeholder_files_n;
    char *expanded = ui_expand_command(ui, cmd, false);
    if (!expanded) {
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return;
    }
    int fds[2];
    if (pipe(fds) != 0) {
        free(expanded);
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return;
    }
    pid_t pid = fork();
    if (pid == 0) {
        (void)setpgid(0, 0);
        close(fds[0]);
        if (dup2(fds[1], STDOUT_FILENO) < 0) _exit(127);
        close(fds[1]);
        if (ui->shell && *ui->shell) execl(ui->shell, ui->shell, "-c", expanded, (char *)NULL);
        else execl("/bin/sh", "sh", "-c", expanded, (char *)NULL);
        _exit(127);
    }
    free(expanded);
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
        return;
    }
    (void)setpgid(pid, pid);
    int flags = fcntl(fds[0], F_GETFL, 0);
    if (flags >= 0) (void)fcntl(fds[0], F_SETFL, flags | O_NONBLOCK);
    ui->bg_transform_pid = pid;
    ui->bg_transform_fd = fds[0];
    ui->bg_transform_placeholder_start = placeholder_checkpoint;
}

/* Drain output without blocking; wait for process exit without blocking too. */
static bool ui_bg_transform_poll(mm_ui_t *ui) {
    if (ui->bg_transform_pid <= 0) return true;
    if (ui->bg_transform_fd >= 0) {
        char buf[4096];
        for (;;) {
            ssize_t n = read(ui->bg_transform_fd, buf, sizeof(buf));
            if (n > 0) {
                if ((size_t)n > SIZE_MAX - ui->bg_transform_len - 1) {
                    close(ui->bg_transform_fd);
                    ui->bg_transform_fd = -1;
                    break;
                }
                size_t need = ui->bg_transform_len + (size_t)n + 1;
                if (need > ui->bg_transform_cap) {
                    size_t cap = ui->bg_transform_cap ? ui->bg_transform_cap : 256;
                    while (cap < need && cap <= SIZE_MAX / 2) cap *= 2;
                    if (cap < need) {
                        close(ui->bg_transform_fd);
                        ui->bg_transform_fd = -1;
                        break;
                    }
                    char *grown = realloc(ui->bg_transform_output, cap);
                    if (!grown) {
                        close(ui->bg_transform_fd);
                        ui->bg_transform_fd = -1;
                        break;
                    }
                    ui->bg_transform_output = grown;
                    ui->bg_transform_cap = cap;
                }
                memcpy(ui->bg_transform_output + ui->bg_transform_len, buf, (size_t)n);
                ui->bg_transform_len += (size_t)n;
                ui->bg_transform_output[ui->bg_transform_len] = '\0';
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
            close(ui->bg_transform_fd);
            ui->bg_transform_fd = -1;
            break;
        }
    }

    int status;
    pid_t waited = waitpid(ui->bg_transform_pid, &status, WNOHANG);
    if (waited == 0 || (waited < 0 && errno == EINTR)) return true;
    if (waited < 0 && errno != ECHILD) return true;
    ui->bg_transform_pid = 0;
    size_t placeholder_checkpoint = ui->bg_transform_placeholder_start;
    ui->bg_transform_placeholder_start = ui->placeholder_files_n;
    ui_cleanup_placeholder_files(ui, placeholder_checkpoint);
    /* Detach the completed job before applying its actions: the returned
       chain may start or cancel another background transform. */
    char *output = ui->bg_transform_output;
    size_t output_len = ui->bg_transform_len;
    mm_action_t target = ui->bg_transform_target;
    bool first_line = ui->bg_transform_first_line;
    ui->bg_transform_output = NULL;
    ui->bg_transform_len = ui->bg_transform_cap = 0;
    ui->bg_transform_target = MM_ACT_NONE;
    ui->bg_transform_first_line = false;
    if (!output) output = calloc(1, 1);
    bool keep = true;
    if (output) {
        if (target != MM_ACT_NONE && target != MM_ACT_BG_TRANSFORM) {
            if (first_line) ui_trim_first_line(output, &output_len);
            mm_binding_t fake = {0};
            fake.action = target;
            fake.arg = output;
            keep = ui_run_action(ui, &fake);
        } else {
            keep = ui_apply_transform_output(ui, output, output_len);
        }
    }
    free(output);
    return keep;
}

static void ui_bg_transform_cancel(mm_ui_t *ui) {
    if (ui->bg_transform_pid > 0) {
        pid_t pid = ui->bg_transform_pid;
        if (kill(-pid, SIGTERM) < 0)
            (void)kill(pid, SIGTERM);

        /* Do not let bg-cancel hang forever on a command that ignores
           SIGTERM. Give the process group a short grace period, then kill it. */
        bool reaped = false;
        const struct timespec pause = { .tv_sec = 0, .tv_nsec = 10000000 };
        for (int i = 0; i < 10; i++) {
            pid_t waited = waitpid(pid, NULL, WNOHANG);
            if (waited == pid || (waited < 0 && errno == ECHILD)) {
                reaped = true;
                break;
            }
            if (waited < 0 && errno != EINTR) break;
            (void)nanosleep(&pause, NULL);
        }
        (void)kill(-pid, SIGKILL);
        if (!reaped) {
            while (waitpid(pid, NULL, 0) < 0 && errno == EINTR) {}
        }
    }
    ui->bg_transform_pid = 0;
    if (ui->bg_transform_fd >= 0) close(ui->bg_transform_fd);
    ui->bg_transform_fd = -1;
    free(ui->bg_transform_output);
    ui->bg_transform_output = NULL;
    ui->bg_transform_len = ui->bg_transform_cap = 0;
    ui_cleanup_placeholder_files(ui, ui->bg_transform_placeholder_start);
    ui->bg_transform_placeholder_start = ui->placeholder_files_n;
}

/* Preview scrolling operates on output lines and clamps to the available tail. */
static int ui_preview_line_count(const mm_ui_t *ui) {
    const char *out = ui->preview ? mm_preview_output(ui->preview) : NULL;
    if (!out || !*out) return 0;
    int n = 1;
    for (const char *p = out; *p; p++) if (*p == '\n') n++;
    size_t len = strlen(out);
    if (len && out[len - 1] == '\n') n--;
    return n;
}

static void ui_preview_scroll(mm_ui_t *ui, int delta) {
    if (!ui->preview || ui->preview_hidden) return;
    int lines = ui_preview_line_count(ui);
    int page = ui->preview_page_size > 1 ? ui->preview_page_size : 1;
    int bottom_offset = lines > page ? lines - page : 0;
    int max_offset = ui->preview_win.wrap && lines > 0 ? lines - 1 : bottom_offset;
    if (delta == INT_MIN) ui->preview_offset = 0;
    else if (delta == INT_MAX) ui->preview_offset = bottom_offset;
    else {
        long next = (long)ui->preview_offset + delta;
        if (next < 0) next = 0;
        if (next > max_offset) next = max_offset;
        ui->preview_offset = (int)next;
    }
}

/* Execute one action. Returns false when the action should end the loop. */
static bool ui_transform_kind(mm_action_t action, mm_action_t *target,
                              bool *first_line, bool *background) {
    static const struct {
        mm_action_t action;
        mm_action_t target;
        bool first_line;
        bool background;
    } table[] = {
        { MM_ACT_TRANSFORM_PROMPT, MM_ACT_CHANGE_PROMPT, true, false },
        { MM_ACT_TRANSFORM_QUERY, MM_ACT_CHANGE_QUERY, true, false },
        { MM_ACT_TRANSFORM_HEADER, MM_ACT_CHANGE_HEADER, false, false },
        { MM_ACT_TRANSFORM_FOOTER, MM_ACT_CHANGE_FOOTER, false, false },
        { MM_ACT_TRANSFORM_HEADER_LABEL, MM_ACT_CHANGE_HEADER_LABEL, true, false },
        { MM_ACT_TRANSFORM_FOOTER_LABEL, MM_ACT_CHANGE_FOOTER_LABEL, true, false },
        { MM_ACT_TRANSFORM_INPUT_LABEL, MM_ACT_CHANGE_INPUT_LABEL, true, false },
        { MM_ACT_TRANSFORM_LIST_LABEL, MM_ACT_CHANGE_LIST_LABEL, true, false },
        { MM_ACT_TRANSFORM_BORDER_LABEL, MM_ACT_CHANGE_BORDER_LABEL, true, false },
        { MM_ACT_TRANSFORM_PREVIEW_LABEL, MM_ACT_CHANGE_PREVIEW_LABEL, true, false },
        { MM_ACT_TRANSFORM_HEADER_LINES, MM_ACT_CHANGE_HEADER_LINES, true, false },
        { MM_ACT_TRANSFORM_GHOST, MM_ACT_CHANGE_GHOST, true, false },
        { MM_ACT_TRANSFORM_NTH, MM_ACT_CHANGE_NTH, true, false },
        { MM_ACT_TRANSFORM_WITH_NTH, MM_ACT_CHANGE_WITH_NTH, true, false },
        { MM_ACT_TRANSFORM_POINTER, MM_ACT_CHANGE_POINTER, true, false },
        { MM_ACT_TRANSFORM_SEARCH, MM_ACT_SEARCH, true, false },
        { MM_ACT_BG_TRANSFORM_PROMPT, MM_ACT_CHANGE_PROMPT, true, true },
        { MM_ACT_BG_TRANSFORM_QUERY, MM_ACT_CHANGE_QUERY, true, true },
        { MM_ACT_BG_TRANSFORM_HEADER, MM_ACT_CHANGE_HEADER, false, true },
        { MM_ACT_BG_TRANSFORM_FOOTER, MM_ACT_CHANGE_FOOTER, false, true },
        { MM_ACT_BG_TRANSFORM_HEADER_LABEL, MM_ACT_CHANGE_HEADER_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_FOOTER_LABEL, MM_ACT_CHANGE_FOOTER_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_INPUT_LABEL, MM_ACT_CHANGE_INPUT_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_LIST_LABEL, MM_ACT_CHANGE_LIST_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_BORDER_LABEL, MM_ACT_CHANGE_BORDER_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_PREVIEW_LABEL, MM_ACT_CHANGE_PREVIEW_LABEL, true, true },
        { MM_ACT_BG_TRANSFORM_HEADER_LINES, MM_ACT_CHANGE_HEADER_LINES, true, true },
        { MM_ACT_BG_TRANSFORM_GHOST, MM_ACT_CHANGE_GHOST, true, true },
        { MM_ACT_BG_TRANSFORM_NTH, MM_ACT_CHANGE_NTH, true, true },
        { MM_ACT_BG_TRANSFORM_WITH_NTH, MM_ACT_CHANGE_WITH_NTH, true, true },
        { MM_ACT_BG_TRANSFORM_POINTER, MM_ACT_CHANGE_POINTER, true, true },
        { MM_ACT_BG_TRANSFORM_SEARCH, MM_ACT_SEARCH, true, true },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (table[i].action != action) continue;
        *target = table[i].target;
        *first_line = table[i].first_line;
        *background = table[i].background;
        return true;
    }
    return false;
}

static bool ui_execute_action(mm_ui_t *ui, const mm_binding_t *b) {
    /* One-shot tracking ends as soon as the user explicitly moves focus.
       Query edits and refreshes keep it active so the same item can be found
       in the updated result order. */
    if (ui->track_current && !ui->track) {
        switch (b->action) {
        case MM_ACT_UP: case MM_ACT_DOWN:
        case MM_ACT_UP_MATCH: case MM_ACT_DOWN_MATCH:
        case MM_ACT_FIRST: case MM_ACT_LAST: case MM_ACT_BEST:
        case MM_ACT_PAGE_UP: case MM_ACT_PAGE_DOWN:
        case MM_ACT_HALF_PAGE_UP: case MM_ACT_HALF_PAGE_DOWN:
        case MM_ACT_OFFSET_UP: case MM_ACT_OFFSET_DOWN:
        case MM_ACT_OFFSET_MIDDLE:
        case MM_ACT_JUMP: case MM_ACT_JUMP_ACCEPT:
            ui->track_current = false;
            break;
        default:
            break;
        }
    }
    {
        mm_action_t target = MM_ACT_NONE;
        bool first_line = false, background = false;
        if (ui_transform_kind(b->action, &target, &first_line, &background)) {
            if (background) {
                ui_bg_transform_start(ui, b->arg, target, first_line);
                return true;
            }
            size_t len = 0;
            char *output = ui_capture_output(ui, b->arg, &len);
            if (!output) return true;
            if (first_line) ui_trim_first_line(output, &len);
            mm_binding_t fake = {0};
            fake.action = target;
            fake.arg = output;
            bool keep = ui_execute_action(ui, &fake);
            free(output);
            return keep;
        }
    }
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
        if (ui->nresults > 0) {
            int step = ui_nav_step(ui, b->action == MM_ACT_UP_MATCH);
            int next = ui->cursor;
            for (int tried = 0; tried < ui->nresults; tried++) {
                next += step;
                if (ui->cycle) next = (next + ui->nresults) % ui->nresults;
                else if (next < 0 || next >= ui->nresults) break;
                if (!ui->raw || ui->query_len == 0 ||
                    (ui->match_pos && next < ui->match_pos_n &&
                     ui->match_pos[next].npos > 0)) {
                    ui->cursor = next;
                    break;
                }
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
    case MM_ACT_SELECT:
        if (ui->multi && ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            if (!ui->selected[item] &&
                (ui->multi_max <= 0 || ui->nselected < ui->multi_max))
                ui_select_item(ui, item);
        }
        break;
    case MM_ACT_TOGGLE_IN:
    case MM_ACT_TOGGLE_OUT: {
        mm_binding_t mapped = *b;
        bool toggle_down = (b->action == MM_ACT_TOGGLE_IN)
            ? ui->layout == 0 : ui->layout != 0;
        mapped.action = toggle_down ? MM_ACT_TOGGLE_DOWN : MM_ACT_TOGGLE_UP;
        return ui_execute_action(ui, &mapped);
    }
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
            int step = ui_nav_step(ui, false);
            int next = ui->cursor + step;
            if (next >= 0 && next < ui->nresults) ui->cursor = next;
        }
        break;
    case MM_ACT_SELECT_DOWN:
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            int limit = ui->multi ? ui->multi_max : 1;
            if (!ui->selected[item] &&
                (limit <= 0 || ui->nselected < limit)) {
                if (limit == 1) ui_clear_selection(ui);
                ui_select_item(ui, item);
            }
            int step = ui_nav_step(ui, false);
            int next = ui->cursor + step;
            if (next >= 0 && next < ui->nresults) ui->cursor = next;
        }
        break;
    case MM_ACT_DESELECT_UP:
        if (ui->nresults > 0) {
            int item = ui->results[ui->cursor];
            ui_deselect_item(ui, item);
            int step = ui_nav_step(ui, true);
            int next = ui->cursor + step;
            if (next >= 0 && next < ui->nresults) ui->cursor = next;
        }
        break;
    case MM_ACT_REFRESH_PREVIEW:
        if (ui->preview) ui_preview_restart(ui);
        break;
    case MM_ACT_CHANGE_POINTER:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->pointer, b->arg);
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
    case MM_ACT_BEST:
        ui->cursor = 0;
        if (ui->raw && ui->query_len > 0 && ui->match_pos) {
            for (int i = 0; i < ui->nresults && i < ui->match_pos_n; i++) {
                if (ui->match_pos[i].npos > 0) { ui->cursor = i; break; }
            }
        }
        ui->offset = 0;
        break;
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
    case MM_ACT_BACKWARD_CHAR:
        if (ui->query_pos > 0) {
            ui->query_pos--;
            while (ui->query_pos > 0 &&
                   ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                ui->query_pos--;
        }
        break;
    case MM_ACT_FORWARD_CHAR:
        if ((size_t)ui->query_pos < ui->query_len) {
            ui->query_pos++;
            while ((size_t)ui->query_pos < ui->query_len &&
                   ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
                ui->query_pos++;
        }
        break;
    case MM_ACT_BEGINNING_OF_LINE:
        ui->query_pos = 0;
        break;
    case MM_ACT_END_OF_LINE:
        ui->query_pos = (int)ui->query_len;
        break;
    case MM_ACT_BACKWARD_WORD:
        while (ui->query_pos > 0 &&
               ui->query_buf[ui->query_pos - 1] != ' ')
            ui->query_pos--;
        while (ui->query_pos > 0 &&
               ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
            ui->query_pos--;
        break;
    case MM_ACT_FORWARD_WORD:
        while ((size_t)ui->query_pos < ui->query_len &&
               ui->query_buf[ui->query_pos] != ' ')
            ui->query_pos++;
        while ((size_t)ui->query_pos < ui->query_len &&
               ((unsigned char)ui->query_buf[ui->query_pos] & 0xC0) == 0x80)
            ui->query_pos++;
        break;
    case MM_ACT_BACKWARD_SUBWORD:
        ui->query_pos = (int)query_subword_start(
            ui->query_buf, (size_t)ui->query_pos);
        break;
    case MM_ACT_BACKWARD_KILL_SUBWORD: {
        size_t pos = (size_t)ui->query_pos;
        if (pos > ui->query_len) pos = ui->query_len;
        size_t start = query_subword_start(ui->query_buf, pos);
        if (start < pos) {
            memmove(ui->query_buf + start, ui->query_buf + pos,
                    ui->query_len - pos + 1);
            ui->query_len -= pos - start;
            ui->query_pos = (int)start;
            ui_search(ui);
        }
        break;
    }
    case MM_ACT_FORWARD_SUBWORD:
        ui->query_pos = (int)query_subword_end(
            ui->query_buf, ui->query_len, (size_t)ui->query_pos);
        break;
    case MM_ACT_BACKWARD_DELETE_CHAR:
        query_backspace(ui);
        ui_search(ui);
        break;
    case MM_ACT_DELETE_CHAR:
        if (query_delete(ui)) ui_search(ui);
        break;
    case MM_ACT_DELETE_CHAR_EOF: {
        bool can_delete = (size_t)ui->query_pos < ui->query_len;
        if (query_delete(ui)) {
            ui_search(ui);
        } else if (!can_delete && ui->query_pos == 0) {
            ui->exit_code = 130;
            ui->aborted = true;
            return false;
        }
        break;
    }
    case MM_ACT_BACKWARD_DELETE_CHAR_EOF:
        if (ui->query_len == 0) {
            ui->exit_code = 130;
            ui->aborted = true;
            return false;
        }
        if (ui->query_pos > 0) {
            query_backspace(ui);
            ui_search(ui);
        }
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
    case MM_ACT_KILL_SUBWORD: {
        size_t pos = (size_t)ui->query_pos;
        if (pos > ui->query_len) pos = ui->query_len;
        size_t end = query_kill_subword_end(ui->query_buf, ui->query_len, pos);
        if (end > pos) {
            size_t n = end - pos;
            char *yanked = malloc(n + 1);
            if (yanked) {
                memcpy(yanked, ui->query_buf + pos, n);
                yanked[n] = '\0';
                free(ui->yank_buf);
                ui->yank_buf = yanked;
            }
            memmove(ui->query_buf + pos, ui->query_buf + end,
                    ui->query_len - end + 1);
            ui->query_len -= n;
            query_note_edit(ui);
            ui_search(ui);
        }
        break;
    }
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
        if (b->arg) ui_run_command(ui, b->arg, false, false);
        break;
    case MM_ACT_EXECUTE_MULTI:
        if (b->arg) ui_run_command(ui, b->arg, false, true);
        break;
    case MM_ACT_EXECUTE_SILENT:
        if (b->arg) ui_run_command(ui, b->arg, true, false);
        break;
    case MM_ACT_TRANSFORM:
        return ui_transform(ui, b->arg);
    case MM_ACT_BG_TRANSFORM:
        ui_bg_transform_start(ui, b->arg, MM_ACT_BG_TRANSFORM, false);
        break;
    case MM_ACT_BG_CANCEL:
        ui_bg_transform_cancel(ui);
        break;
    case MM_ACT_EXCLUDE:
    case MM_ACT_EXCLUDE_MULTI: {
        bool multi_action = b->action == MM_ACT_EXCLUDE_MULTI;
        bool changed = false;
        if (multi_action && ui->multi && ui->nselected > 0) {
            for (int i = 0; i < ui->count; i++) {
                if (ui->selected[i] && !ui->excluded[i]) {
                    ui->excluded[i] = true;
                    ui->has_excluded = true;
                    ui->plain_results = false;
                    ui->selected[i] = false;
                    changed = true;
                }
            }
            if (changed) {
                ui->nselected = 0;
                ui->sel_order_n = 0;
                for (int i = 0; i < ui->count; i++)
                    if (ui->selected[i]) ui->nselected++;
            }
        } else if (ui->nresults > 0 && ui->cursor >= 0 &&
                   ui->cursor < ui->nresults) {
            int item = ui->results[ui->cursor];
            if (item >= 0 && item < ui->count && !ui->excluded[item]) {
                ui->excluded[item] = true;
                ui->has_excluded = true;
                ui->plain_results = false;
                if (ui->selected[item]) {
                    ui->selected[item] = false;
                    if (ui->nselected > 0) ui->nselected--;
                    int out = 0;
                    for (int i = 0; i < ui->sel_order_n; i++)
                        if (ui->sel_order[i] != item)
                            ui->sel_order[out++] = ui->sel_order[i];
                    ui->sel_order_n = out;
                }
                changed = true;
            }
        }
        if (changed) {
            ui_qcache_clear(ui);
            ui->results_item_count = 0;
            ui_search(ui);
            if (ui->cursor >= ui->nresults) ui->cursor = ui->nresults > 0 ? ui->nresults - 1 : 0;
        }
        break;
    }
    case MM_ACT_PUT:
        if (b->arg && *b->arg) {
            query_insert(ui, b->arg, strlen(b->arg));
            ui_search(ui);
        }
        break;
    case MM_ACT_CHANGE_BORDER_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->border_label, b->arg);
        break;
    case MM_ACT_CHANGE_FOOTER:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->footer, b->arg);
        break;
    case MM_ACT_CHANGE_FOOTER_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->footer_label, b->arg);
        break;
    case MM_ACT_CHANGE_GHOST:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->ghost, b->arg);
        break;
    case MM_ACT_CHANGE_HEADER_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->header_label, b->arg);
        break;
    case MM_ACT_CHANGE_INPUT_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->input_label, b->arg);
        break;
    case MM_ACT_CHANGE_LIST_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->list_label, b->arg);
        break;
    case MM_ACT_CHANGE_PREVIEW_LABEL:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->preview_label, b->arg);
        break;
    case MM_ACT_CHANGE_QUERY:
        if (b->arg) { query_set(ui, b->arg); ui_search(ui); }
        break;
    case MM_ACT_CHANGE_PROMPT:
        if (b->arg) (void)ui_set_runtime_string(ui, &ui->prompt, b->arg);
        break;
    case MM_ACT_CHANGE_HEADER:
        if (b->arg) {
            if (ui_set_runtime_string(ui, &ui->saved_header, b->arg) && !ui->header_hidden)
                ui->header = ui->saved_header;
        }
        break;
    case MM_ACT_CHANGE_HEADER_LINES: {
        int n = 0;
        if (!ui_parse_nonneg_int(b->arg, &n) || n == ui->header_line_count) break;
        int prev_item = -1;
        int prev_pos = 0;
        if (ui->results && ui->cursor >= 0 && ui->cursor < ui->nresults) {
            prev_item = ui->results[ui->cursor];
            prev_pos = ui->cursor - ui->offset;
            if (prev_pos < 0) prev_pos = 0;
        }
        ui->header_line_count = n;
        for (int item = 0; item < ui->count && item < n; item++)
            ui_deselect_item(ui, item);
        ui_rebuild_header_lines(ui);
        ui_qcache_clear(ui);
        ui->results_item_count = 0;
        ui_search(ui);
        if (prev_item >= 0) {
            for (int i = 0; i < ui->nresults; i++) {
                if (ui->results[i] != prev_item) continue;
                ui->cursor = i;
                ui->offset = i - prev_pos;
                if (ui->offset < 0) ui->offset = 0;
                break;
            }
        }
        break;
    }
    case MM_ACT_CHANGE_NTH:
    case MM_ACT_CHANGE_WITH_NTH: {
        mm_range_t *ranges = NULL;
        int nranges = 0;
        const char *spec = b->arg ? b->arg : "";
        if (*spec && !mm_parse_ranges(spec, &ranges, &nranges)) {
            free(ranges);
            break;
        }
        if (b->action == MM_ACT_CHANGE_NTH) {
            free(ui->nth);
            ui->nth = ranges;
            ui->nth_n = nranges;
        } else {
            free(ui->with_nth);
            ui->with_nth = ranges;
            ui->with_nth_n = nranges;
        }
        ui_qcache_clear(ui);
        ui->results_item_count = 0;
        ui_search(ui);
        break;
    }
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
    case MM_ACT_JUMP:
    case MM_ACT_JUMP_ACCEPT:
        ui->jump_mode = ui->nresults > 0
                      ? (b->action == MM_ACT_JUMP_ACCEPT ? 2 : 1) : 0;
        ui->jump_label_count = 0;
        break;
    case MM_ACT_RELOAD:
    case MM_ACT_RELOAD_SYNC:
        /* This port currently collects reload output synchronously. */
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
        /* Defer exec until the event loop has restored terminal state. */
        if (b->arg && *b->arg) {
            char *expanded = ui_expand_command(ui, b->arg, false);
            if (expanded) {
                free(ui->become_command);
                ui->become_command = expanded;
                return false;
            }
        }
        break;
    case MM_ACT_IGNORE:
        break;
    case MM_ACT_CLEAR_MULTI:
        memset(ui->selected, 0, (size_t)ui->count * sizeof(bool));
        ui->nselected = 0;
        break;
    case MM_ACT_HIDE_HEADER:
        if (ui->header) ui->saved_header = ui->header;
        ui->header_hidden = true;
        ui->header = NULL;
        break;
    case MM_ACT_HIDE_PREVIEW:
        ui->preview_hidden = true;
        break;
    case MM_ACT_SHOW_PREVIEW:
        ui->preview_hidden = false;
        if (ui->preview) ui_preview_restart(ui);
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
        if (ui->header) ui->saved_header = ui->header;
        ui->header_hidden = !ui->header_hidden;
        ui->header = ui->header_hidden ? NULL : ui->saved_header;
        break;
    case MM_ACT_SHOW_HEADER:
        ui->header_hidden = false;
        ui->header = ui->saved_header;
        break;
    case MM_ACT_SHOW_INPUT:
        /* Already visible: fzf skips the redraw and continues the chain. */
        if (!ui->no_input) break;
        ui->no_input = false;
        break;
    case MM_ACT_HIDE_INPUT:
        if (ui->no_input) break;
        ui->no_input = true;
        break;
    case MM_ACT_TOGGLE_INPUT:
        ui->no_input = !ui->no_input;
        break;
    case MM_ACT_TOGGLE_MULTI_LINE:
        ui->no_multi_line = !ui->no_multi_line;
        break;
    case MM_ACT_TOGGLE_HSCROLL:
        ui->no_hscroll = !ui->no_hscroll;
        break;
    case MM_ACT_TOGGLE_PREVIEW:
        if (ui->preview) {
            ui->preview_hidden = !ui->preview_hidden;
            if (!ui->preview_hidden) ui_preview_restart(ui);
        }
        break;
    case MM_ACT_TOGGLE_PREVIEW_WRAP:
        if (ui->preview) ui->preview_win.wrap = !ui->preview_win.wrap;
        break;
    case MM_ACT_TOGGLE_PREVIEW_WRAP_WORD:
        if (ui->preview) {
            ui->preview_win.wrap_word = !ui->preview_win.wrap_word;
            if (ui->preview_win.wrap_word) ui->preview_win.wrap = true;
        }
        break;
    case MM_ACT_CHANGE_PREVIEW:
        if (b->arg && ui->preview) {
            mm_preview_set_command(ui->preview, b->arg);
            ui_preview_restart(ui);
        }
        break;
    case MM_ACT_CHANGE_PREVIEW_WINDOW:
        if (b->arg && ui->preview) {
            /* fzf resets to the original --preview-window, applies the first
               alternative, then rotates that alternative to the end. An
               empty alternative restores the original. Wrap state is kept
               unless the alternative sets it. */
            mm_binding_t *mut = (mm_binding_t *)b;
            const char *arg = mut->arg;
            const char *bar = strchr(arg, '|');
            size_t n = bar ? (size_t)(bar - arg) : strlen(arg);
            char *tok = malloc(n + 1);
            if (!tok) break;
            memcpy(tok, arg, n);
            tok[n] = 0;
            mm_preview_win_t next = ui->preview_win_initial;
            next.wrap = ui->preview_win.wrap;
            next.wrap_word = ui->preview_win.wrap_word;
            if (n > 0 && ui->preview_win_initial.hidden) next.hidden = false;
            bool ok = mm_preview_window_apply(tok, &next);
            free(tok);
            if (!ok) break;
            bool was_hidden = ui->preview_hidden;
            ui->preview_win = next;
            ui->preview_hidden = next.hidden;
            if (!next.hidden && was_hidden) ui_preview_restart(ui);
            if (bar) {
                size_t rest = strlen(bar + 1);
                char *rotated = malloc(rest + 1 + n + 1);
                if (rotated) {
                    memcpy(rotated, bar + 1, rest);
                    rotated[rest] = '|';
                    memcpy(rotated + rest + 1, arg, n);
                    rotated[rest + 1 + n] = 0;
                    free(mut->arg);
                    mut->arg = rotated;
                }
            }
        }
        break;
    case MM_ACT_PREVIEW_TOP: ui_preview_scroll(ui, INT_MIN); break;
    case MM_ACT_PREVIEW_BOTTOM: ui_preview_scroll(ui, INT_MAX); break;
    case MM_ACT_PREVIEW_UP: ui_preview_scroll(ui, -1); break;
    case MM_ACT_PREVIEW_DOWN: ui_preview_scroll(ui, 1); break;
    case MM_ACT_PREVIEW_PAGE_UP:
        ui_preview_scroll(ui, -(ui->preview_page_size > 1 ? ui->preview_page_size : 1));
        break;
    case MM_ACT_PREVIEW_PAGE_DOWN:
        ui_preview_scroll(ui, ui->preview_page_size > 1 ? ui->preview_page_size : 1);
        break;
    case MM_ACT_PREVIEW_HALF_PAGE_UP:
        ui_preview_scroll(ui, -(ui->preview_page_size > 2 ? ui->preview_page_size / 2 : 1));
        break;
    case MM_ACT_PREVIEW_HALF_PAGE_DOWN:
        ui_preview_scroll(ui, ui->preview_page_size > 2 ? ui->preview_page_size / 2 : 1);
        break;
    case MM_ACT_PREVIEW:
        /* A preview command is configured separately; this action only
           changes visibility and never destroys the configured runner. */
        if (ui->preview) {
            ui->preview_hidden = !ui->preview_hidden;
            if (!ui->preview_hidden) ui_preview_restart(ui);
        }
        break;
    case MM_ACT_DISABLE_RAW:
        ui->raw = false;
        ui_search(ui);
        break;
    case MM_ACT_ENABLE_RAW:
        ui->raw = true;
        ui_search(ui);
        break;
    case MM_ACT_TOGGLE_RAW:
        ui->raw = !ui->raw;
        ui_search(ui);
        break;
    case MM_ACT_DISABLE_SEARCH:
        ui->search_enabled = false;
        ui_search(ui);
        break;
    case MM_ACT_ENABLE_SEARCH:
        ui->search_enabled = true;
        ui_search(ui);
        break;
    case MM_ACT_TOGGLE_SEARCH:
        ui->search_enabled = !ui->search_enabled;
        ui_search(ui);
        break;
    case MM_ACT_TOGGLE_SORT:
        ui->no_sort = !ui->no_sort;
        ui_qcache_clear(ui);
        ui->results_item_count = 0;
        ui_search(ui);
        break;
    case MM_ACT_TOGGLE_TRACK:
        ui->track = !ui->track;
        break;
    case MM_ACT_TOGGLE_TRACK_CURRENT:
        if (!ui->track && ui->nresults > 0)
            ui->track_current = !ui->track_current;
        break;
    case MM_ACT_TOGGLE_WRAP:
        ui->wrap = !ui->wrap;
        break;
    case MM_ACT_TOGGLE_WRAP_WORD:
        ui->wrap_word = !ui->wrap_word;
        break;
    case MM_ACT_TRACK:
        if (!ui->track && ui->nresults > 0) ui->track_current = true;
        break;
    case MM_ACT_UNBIND:
        if (b->arg) (void)mm_bind_unbind(&ui->keymap, &ui->disabled_keymap, b->arg);
        break;
    case MM_ACT_REBIND:
        if (b->arg) (void)mm_bind_rebind(&ui->keymap, &ui->disabled_keymap, b->arg);
        break;
    case MM_ACT_TOGGLE_BIND:
        if (b->arg) (void)mm_bind_toggle(&ui->keymap, &ui->disabled_keymap, b->arg);
        break;
    case MM_ACT_UNIX_LINE_DISCARD:
        /* Discard everything from the start of the line to the cursor. */
        if (ui->query_pos > 0) {
            size_t pos = (size_t)ui->query_pos;
            memmove(ui->query_buf, ui->query_buf + pos,
                    ui->query_len - pos + 1);
            ui->query_len -= pos;
            ui->query_pos = 0;
            ui->query_buf[ui->query_len] = 0;
        }
        ui_search(ui);
        break;
    case MM_ACT_UNTRACK_CURRENT:
        if (!ui->track) ui->track_current = false;
        break;
    case MM_ACT_WAIT:
        break;
    case MM_ACT_YANK:
        if (ui->yank_buf && ui->yank_buf[0]) {
            query_insert(ui, ui->yank_buf, strlen(ui->yank_buf));
            ui_search(ui);
        }
        break;
    case MM_ACT_SEARCH: {
        const char *text = b->arg ? b->arg : "";
        char *copy = strdup(text);
        if (!copy) break;
        free(ui->search_override);
        ui->search_override = copy;
        ui_search(ui);
        break;
    }
    case MM_ACT_TRIGGER: {
        if (!b->arg || ui->trigger_depth > 8) break;
        ui->trigger_depth++;
        const char *p = b->arg;
        bool keep = true;
        while (keep && *p) {
            while (*p == ' ' || *p == '\t' || *p == ',') p++;
            if (!*p) break;
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
            char name[64];
            if (len == 0 || len >= sizeof(name)) {
                p = comma ? comma + 1 : p + strlen(p);
                continue;
            }
            memcpy(name, p, len);
            name[len] = '\0';
            mm_event_t ev;
            if (mm_bind_parse_key(name, &ev)) {
                const mm_binding_t *found[16];
                int nf = mm_bind_lookup(&ui->keymap, &ev, found, 16);
                for (int i = 0; i < nf && keep && !ui->stop_chain; i++)
                    keep = ui_run_action(ui, found[i]);
                ui->stop_chain = false;
            }
            p = comma ? comma + 1 : p + strlen(p);
        }
        ui->trigger_depth--;
        if (!keep) return false;
        break;
    }
    default:
        /* Actions that need features this port does not implement yet are
           accepted and ignored rather than silently mis-executed. */
        break;
    }
    return true;
}

/* fzf restores the pre-action query when the input section is hidden, and
   stops the rest of the chain without leaving the finder. */
static bool ui_run_action(mm_ui_t *ui, const mm_binding_t *b) {
    char *snap = strdup(ui->query_buf ? ui->query_buf : "");
    bool keep = ui_execute_action(ui, b);
    if (ui->no_input && snap &&
        (!ui->query_buf || strcmp(ui->query_buf, snap) != 0)) {
        query_restore(ui, snap);
        ui_search(ui);
    } else if (ui->no_input) {
        ui->query_pos = (int)ui->query_len;
    }
    free(snap);
    return keep;
}

static bool ui_tty_pending(int ttyin) {
    if (ttyin < 0) return false;
    struct pollfd pfd = { .fd = ttyin, .events = POLLIN };
    int ready;
    do {
        ready = poll(&pfd, 1, 0);
    } while (ready < 0 && errno == EINTR);
    return ready > 0 && (pfd.revents & (POLLIN | POLLERR | POLLHUP));
}

static int64_t ui_mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Read one available chunk. When the pipe is empty and block_ms > 0, wait
   that long for the producer or a key. Returns true when items were added
   or the stream finished. */
static bool ui_pull_input(mm_ui_t *ui, int ttyin, int block_ms) {
    if (ui->input_fd < 0 || ui->input_done) return false;
    bool changed = mm_ui_read_items(ui, ttyin);
    if (ui->input_done || ui->input_fd < 0 || ui_tty_pending(ttyin))
        return changed;
    if (changed || block_ms <= 0) return changed;
    struct pollfd pfds[2];
    nfds_t np = 1;
    pfds[0].fd = ui->input_fd;
    pfds[0].events = POLLIN;
    if (ttyin >= 0) {
        pfds[1].fd = ttyin;
        pfds[1].events = POLLIN;
        np = 2;
    }
    int rc;
    do {
        rc = poll(pfds, np, block_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc > 0 && (pfds[0].revents & (POLLIN | POLLHUP | POLLERR)))
        changed = mm_ui_read_items(ui, ttyin) || changed;
    return changed;
}

int mm_ui_run(mm_ui_t *ui) {
    if (!ui->saved_header) ui->saved_header = ui->header;
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
    ui_expand_min_height(ui);
    int win_height = ui->height_spec ? mm_ui_height(ui->height_spec, total_rows,
                                                ui->min_height) : 0;
    if (!mm_renderer_init(&r, ttyin, ttyout, ui->mouse, ui->tabstop, !ui->no_clear, win_height == 0)) {
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
    setenv("FZF_CLICK_HEADER_LINE", "0", 1);
    setenv("FZF_CLICK_HEADER_COLUMN", "0", 1);
    setenv("FZF_CLICK_FOOTER_LINE", "0", 1);
    setenv("FZF_CLICK_FOOTER_COLUMN", "0", 1);
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

    /* Show items as they arrive. A fast walk keeps the pipe full, so waiting
       for it to go idle hides the list until the walker finishes. */
    bool running = true;
    bool load_sent = false;
    bool painted = false;
    /* Items read since the last paint. A chunk can arrive just after a
       frame; the next pass still has to draw it once 16 ms have elapsed,
       even when that pass's read finds the pipe empty. */
    bool input_dirty = false;
    int64_t last_paint = 0;
    int focused_item = -2; /* sentinel forces the initial focus event */
    if (ui->input_fd < 0 || ui->input_done) ui_search(ui);
    while (running && ui->input_fd >= 0 && !ui->input_done &&
           !ui_tty_pending(ttyin)) {
        bool more = ui_pull_input(ui, ttyin, 1);
        bool done = ui->input_fd < 0 || ui->input_done;
        if (more) input_dirty = true;
        int64_t now = ui_mono_ms();
        if ((input_dirty || done) &&
            (!painted || done || now - last_paint >= MM_PAINT_MS)) {
            if (ui->count != ui->nresults) ui_search(ui);
            ui_exec_focus_if_changed(ui, &focused_item);
            ui_render(ui, &r, &listwin, &promptwin, &infowin);
            last_paint = now;
            painted = true;
            input_dirty = false;
        }
        if (done) break;
    }
    if (ui->count != ui->nresults) ui_search(ui);
    if ((ui->input_fd < 0 || ui->input_done) && !load_sent) {
        /* A load action such as accept has to leave the finder. */
        if (!ui_exec_event(ui, "load")) running = false;
        load_sent = true;
    }

    /* Do not auto-accept a transient single result while a producer is still
       feeding the input stream. fzf evaluates --select-1/--exit-0 against the
       completed initial result set, not an arbitrary partial read. */
    bool input_complete = ui->input_fd < 0 || ui->input_done;
    if (input_complete &&
        ((ui->select_1 && ui->nresults == 1) ||
         (ui->exit_0 && ui->nresults == 0)))
        running = false;

    if (running) {
        ui_exec_focus_if_changed(ui, &focused_item);
        ui_render(ui, &r, &listwin, &promptwin, &infowin);
        last_paint = ui_mono_ms();
        painted = true;
        input_dirty = false;
    }

    int exit_code = 0;
    if (ui->preview) ui_preview_restart(ui);

    struct sigaction resize_action = {0};
    struct sigaction previous_resize_action;
    bool resize_handler_installed = false;
    resize_action.sa_handler = ui_handle_sigwinch;
    (void)sigemptyset(&resize_action.sa_mask);
    ui_resize_pending = 0;
    if (sigaction(SIGWINCH, &resize_action, &previous_resize_action) == 0)
        resize_handler_installed = true;

    bool mouse_down = false;
    int mouse_x = -1, mouse_y = -1;
    while (running) {
        mm_event_t ev;
        /* A cancellable read lets us poll the preview command while waiting
           for the next key press. */
        bool got_event = mm_read_event(ttyin, &input, &ev, true);
        if (ui_resize_pending) {
            ui_resize_pending = 0;
            mm_renderer_update_size(&r);
            int resized_width = mm_renderer_max_x(&r);
            int resized_height = mm_renderer_max_y(&r);
            if (r.inline_mode && ui->height_spec) {
                ui->height = mm_ui_height(ui->height_spec, resized_height,
                                          ui->min_height);
                r.inline_height = ui->height;
            }
            /* Page through the window that is actually on screen. The tty
               can be taller than --height. */
            int page_rows = resized_height;
            if (ui->height > 0 && ui->height < page_rows) page_rows = ui->height;
            ui->page_size = page_rows > 3 ? page_rows - 3 : 1;
            mm_window_init(&listwin, &r, 0, 0, resized_width,
                           resized_height > 0 ? resized_height : 1,
                           MM_WIN_LIST, MM_BORDER_NONE, false);
            mm_window_init(&promptwin, &r, 0, 0, resized_width, 1,
                           MM_WIN_INPUT, MM_BORDER_NONE, false);
            mm_window_init(&infowin, &r, 0, 0, resized_width, 1,
                           MM_WIN_INPUT, MM_BORDER_NONE, false);
            ui_exec_event(ui, "resize");
            /* The preview spec is unchanged, so the layout stamp would
               skip the erase and the previous box would stay outside the
               new one. fzf fullRedraw clears from the origin first. */
            mm_renderer_clear(&r);
            ui_render(ui, &r, &listwin, &promptwin, &infowin);
        }
        if (!got_event) break;
        if (ev.type == MM_EV_MOUSE) {
            int x = ev.x;
            int y = ev.y - r.yoffset;
            bool clicked = mouse_down && !ev.mouse_down && !ev.mouse_scroll &&
                           x == mouse_x && y == mouse_y;
            if (ev.mouse_down && !mouse_down) {
                mouse_x = x;
                mouse_y = y;
            }
            if (!ev.mouse_down) mouse_down = false;
            else mouse_down = true;
            if (ui->mouse && clicked) {
                int hit = ui_dispatch_click(ui, x, y);
                if (hit == 2) { running = false; break; }
                if (hit == 1) {
                    ui_exec_focus_if_changed(ui, &focused_item);
                    ui_preview_sync(ui);
                    ui_preview_catch_up(ui);
                    if (running) ui_render(ui, &r, &listwin, &promptwin, &infowin);
                }
            }
            continue;
        }
        if (ev.type == MM_EV_INVALID) {
            /* Timed out: read the items that arrived in the meantime and
               give the preview a chance to produce output. */
            bool more = ui_pull_input(ui, ttyin, 0);
            if (more) input_dirty = true;
            bool bg_transform_active = ui->bg_transform_pid > 0;
            if (!ui_bg_transform_poll(ui)) running = false;
            input_complete = ui->input_fd < 0 || ui->input_done;
            bool loaded_now = false;
            int64_t now = ui_mono_ms();
            bool due = input_dirty && (input_complete || !painted ||
                                       now - last_paint >= MM_PAINT_MS);
            if (due || loaded_now) {
                if (ui->count != ui->nresults) ui_search(ui);
            }
            if (!load_sent && input_complete) {
                if (!ui_exec_event(ui, "load")) running = false;
                load_sent = true;
                loaded_now = true;
            }
            if (input_complete &&
                ((ui->select_1 && ui->nresults == 1) ||
                 (ui->exit_0 && ui->nresults == 0)))
                running = false;
            if (more) ui_exec_focus_if_changed(ui, &focused_item);
            {
                bool preview_ready = ui->preview && mm_preview_poll(ui->preview);
                if ((due || loaded_now || bg_transform_active || preview_ready) &&
                    running) {
                    if (ui->count != ui->nresults) ui_search(ui);
                    ui_render(ui, &r, &listwin, &promptwin, &infowin);
                    last_paint = now;
                    painted = true;
                    if (ui->count == ui->nresults) input_dirty = false;
                }
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
        /* Jump mode consumes the next unbound key as a visible-row label. */
        {
            const mm_binding_t *found[16];
            int nf = mm_bind_lookup(&ui->keymap, &ev, found, 16);
            if (ui->jump_mode && nf == 0) {
                int mode = ui->jump_mode;
                int target = -1;
                if (ev.type == MM_EV_RUNE && ev.rune >= 0 && ev.rune < 128) {
                    const char *label = strchr(ui->jump_labels && *ui->jump_labels
                                               ? ui->jump_labels : MM_DEFAULT_JUMP_LABELS,
                                               (int)ev.rune);
                    if (label) {
                        size_t offset = (size_t)(label - (ui->jump_labels && *ui->jump_labels
                                                         ? ui->jump_labels : MM_DEFAULT_JUMP_LABELS));
                        if (offset < (size_t)ui->jump_label_count &&
                            offset < (size_t)(ui->nresults - ui->offset))
                            target = ui->offset + (int)offset;
                    }
                }
                ui->jump_mode = 0;
                if (target >= 0 && target < ui->nresults) {
                    ui->cursor = target;
                    ui_exec_event(ui, "jump");
                    if (mode == 2) running = false;
                } else {
                    ui_exec_event(ui, "jump-cancel");
                }
                ui_exec_focus_if_changed(ui, &focused_item);
                ui_preview_sync(ui);
                ui_preview_catch_up(ui);
                if (running) ui_render(ui, &r, &listwin, &promptwin, &infowin);
                continue;
            }
            if (ui->jump_mode && nf > 0) {
                ui->jump_mode = 0;
                ui_exec_event(ui, "jump-cancel");
            }
            /* User bindings take precedence over the built-in behaviour. */
            if (nf > 0) {
                bool cont = true;
                ui->stop_chain = false;
                ui->defer_search++;
                for (int i = 0; i < nf && cont && !ui->stop_chain; i++)
                    cont = ui_run_action(ui, found[i]);
                ui->stop_chain = false;
                ui->defer_search--;
                if (cont && ui->search_dirty) ui_search(ui);
                if (!cont) { running = false; break; }
                ui_exec_focus_if_changed(ui, &focused_item);
                ui_preview_sync(ui);
                ui_preview_catch_up(ui);
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
        case MM_EV_CTRL_K:
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
        ui_exec_focus_if_changed(ui, &focused_item);
        ui_preview_sync(ui);
        ui_preview_catch_up(ui);
        if (running) ui_render(ui, &r, &listwin, &promptwin, &infowin);
    }

    if (resize_handler_installed)
        (void)sigaction(SIGWINCH, &previous_resize_action, NULL);

    /* Record the query in the history file. */
    if (ui->history) mm_history_append(ui->history, ui->query_buf);

    /* Leave the alternate screen before writing the selection so that the
       output lands on the user's terminal, not in the discarded buffer. */
    mm_keybuf_free(&input);
    mm_renderer_close(&r);
    close(ttyin);
    close(ttyout);

    if (ui->become_command) {
        fflush(stdout);
        fflush(stderr);
        /* Inside a popup the parent owns the replacement. Write the command
           and exit 126 so it can exec after the pane closes. */
        if (ui->proxy_script && ui->proxy_script[0]) {
            char path[PATH_MAX];
            int nw = snprintf(path, sizeof(path), "%s.become", ui->proxy_script);
            if (nw > 0 && (size_t)nw < sizeof(path)) {
                FILE *bf = fopen(path, "wb");
                if (bf) {
                    fputs(ui->become_command, bf);
                    fputc('\0', bf);
                    extern char **environ;
                    if (environ) {
                        for (char **ev = environ; *ev; ev++) {
                            fputs(*ev, bf);
                            fputc('\0', bf);
                        }
                    }
                    fclose(bf);
                }
            }
            return 126;
        }
        /* The replacement command must be able to read any {f}/{+f}/{*f}
           files created during expansion. Do not unlink them on exec failure. */
        if (ui->shell && *ui->shell)
            execl(ui->shell, ui->shell, "-c", ui->become_command, (char *)NULL);
        else
            execl("/bin/sh", "sh", "-c", ui->become_command, (char *)NULL);
        fprintf(stderr, "fzf: failed to execute become command: %s\n", ui->become_command);
        return 2;
    }
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
    memset(nsel + ui->lines_cap, 0,
           (size_t)(ncap - ui->lines_cap) * sizeof(*nsel));
    ui->selected = nsel;
    bool *nexcluded = realloc(ui->excluded, sizeof(*nexcluded) * (size_t)ncap);
    if (!nexcluded) return false;
    memset(nexcluded + ui->lines_cap, 0,
           (size_t)(ncap - ui->lines_cap) * sizeof(*nexcluded));
    ui->excluded = nexcluded;
    ui->lines_cap = ncap;
    return true;
}

static bool ui_pending_append(mm_ui_t *ui, const char *data, size_t n) {
    if (n == 0) return true;
    if (n > SIZE_MAX - ui->pending_len - 1) return false;
    size_t required = ui->pending_len + n + 1;
    if (required > ui->pending_cap) {
        size_t ncap = ui->pending_cap ? ui->pending_cap : 4096;
        while (ncap < required) {
            if (ncap > SIZE_MAX / 2) {
                ncap = required;
                break;
            }
            ncap *= 2;
        }
        char *np = realloc(ui->pending, ncap);
        if (!np) return false;
        ui->pending = np;
        ui->pending_cap = ncap;
    }
    memcpy(ui->pending + ui->pending_len, data, n);
    ui->pending_len += n;
    ui->pending[ui->pending_len] = '\0';
    return true;
}

/* Append one item. Runes stay undecoded until a non-empty search. */
static bool ui_push_span(mm_ui_t *ui, const char *src, size_t llen) {
    if (ui->input_sep == '\n' && llen > 0 && src[llen - 1] == '\r') llen--;
    char *line = ui_store_line(ui, src, llen);
    if (!line) return false;
    if (!ui_grow(ui, 1)) {
        ui_discard_line(ui, line);
        return false;
    }
    int idx = ui->count;
    ui->lines[idx] = line;
    if (!mm_item_init(&ui->items[idx], line, idx)) {
        ui_discard_line(ui, line);
        ui->lines[idx] = NULL;
        return false;
    }
    ui->selected[idx] = false;
    ui->excluded[idx] = false;
    if (!ui->items[idx].text.ascii) ui->has_non_ascii = true;
    ui->count++;
    return true;
}

/* Split a read into lines. Complete lines are stored in place; only the
   unfinished tail is copied into the pending buffer. */
static bool ui_consume(mm_ui_t *ui, const char *data, size_t len, bool *added) {
    if (ui->pending_len > 0) {
        const char *sep = memchr(data, (unsigned char)ui->input_sep, len);
        size_t head = sep ? (size_t)(sep - data) : len;
        if (!ui_pending_append(ui, data, head)) return false;
        if (!sep) return true;
        if (!ui_push_span(ui, ui->pending, ui->pending_len)) return false;
        *added = true;
        ui->pending_len = 0;
        data += head + 1;
        len -= head + 1;
    }
    const char *p = data;
    const char *end = data + len;
    while (p < end) {
        const char *sep = memchr(p, (unsigned char)ui->input_sep, (size_t)(end - p));
        if (!sep) return ui_pending_append(ui, p, (size_t)(end - p));
        if (!ui_push_span(ui, p, (size_t)(sep - p))) return false;
        *added = true;
        p = sep + 1;
    }
    return true;
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
        if (!ui_consume(ui, buf, (size_t)n, &added)) {
            ui->input_done = true;
            break;
        }
        /* One chunk per call so the UI can paint between reads. A key is
           noticed by the caller before the next chunk. */
        break;
    }
    (void)ttyin;
    /* The last line of the stream is not terminated. */
    if (ui->input_done && ui->pending_len > 0) {
        if (ui_push_span(ui, ui->pending, ui->pending_len)) added = true;
        ui->pending_len = 0;
    }
    /* Report the end of the stream as a change so that the spinner is
       cleared even when the last read did not add an item. */
    if (added && ui->header_line_count > 0) ui_rebuild_header_lines(ui);
    return added || finished;
}

bool mm_ui_init(mm_ui_t *ui, char **lines, int count, int cap) {
    ui->result_events_initialized = false;
    ui->last_search_query = NULL;
    ui->search_override = NULL;
    ui->yank_buf = NULL;
    ui->stop_chain = false;
    ui->trigger_depth = 0;
    ui->bg_transform_target = MM_ACT_NONE;
    ui->bg_transform_first_line = false;
    /* No descriptor means input was collected before entering the UI.
       Streaming callers install their descriptor after initialization. */
    ui->input_fd = -1;
    ui->bg_transform_pid = 0;
    ui->bg_transform_fd = -1;
    ui->bg_transform_output = NULL;
    ui->bg_transform_len = ui->bg_transform_cap = 0;
    ui->bg_transform_placeholder_start = 0;
    ui->become_command = NULL;
    ui->preview_item = -1;
    ui->preview_query = NULL;
    ui->preview_layout = -2;
    ui->runtime_strings = NULL;
    ui->runtime_strings_n = ui->runtime_strings_cap = 0;
    ui->placeholder_files = NULL;
    ui->placeholder_files_n = ui->placeholder_files_cap = 0;
    ui->lines = lines;
    ui->count = 0;
    ui->lines_cap = cap;
    ui->count = count; /* lets mm_ui_free release every input line on failure */
    /* The arrays are allocated with the full capacity, so that the items
       can be appended while the program is running. */
    size_t alloc = (size_t)(cap ? cap : (count ? count : 1));
    ui->items = calloc(alloc, sizeof(mm_item_t));
    if (!ui->items) return false;
    for (int i = 0; i < count; i++) {
        if (!mm_item_init(&ui->items[i], lines[i], i)) {
            memset(&ui->items[i], 0, sizeof(ui->items[i]));
            return false;
        }
        if (!mchars_decode(&ui->items[i].text)) {
            mm_item_free(&ui->items[i]);
            memset(&ui->items[i], 0, sizeof(ui->items[i]));
            return false;
        }
    }
    ui->selected = calloc(alloc, sizeof(bool));
    ui->excluded = calloc(alloc, sizeof(bool));
    if (!ui->selected || !ui->excluded) return false;
    query_restore(ui, ui->query ? ui->query : "");
    ui->cursor = 0;
    ui->offset = 0;
    ui->nresults = 0;
    ui->results = NULL;
    ui->results_cap = 0;
    ui->plain_results = false;
    ui->has_excluded = false;
    ui_rebuild_header_lines(ui);
    return true;
}

void mm_ui_free(mm_ui_t *ui) {
    ui_bg_transform_cancel(ui);
    ui_cleanup_placeholder_files(ui, 0);
    free(ui->placeholder_files);
    free(ui->become_command);
    free(ui->preview_query);
    ui->preview_query = NULL;
    free(ui->header_lines_buf);
    ui->header_lines_buf = NULL;
    ui->header_lines = NULL;
    for (size_t i = 0; i < ui->runtime_strings_n; i++) free(ui->runtime_strings[i]);
    free(ui->runtime_strings);
    free(ui->nth);
    free(ui->with_nth);
    ui->nth = NULL;
    ui->with_nth = NULL;
    ui->nth_n = 0;
    ui->with_nth_n = 0;
    if (ui->items)
        for (int i = 0; i < ui->count; i++) mm_item_free(&ui->items[i]);
    ui_free_line_storage(ui);
    ui_free_rune_slabs(ui);
    ui_qcache_clear(ui);
    mm_match_pos_free(ui->match_pos, ui->match_pos_n);
    ui->match_pos = NULL;
    ui->match_pos_n = 0;
    ui_clear_window_pos(ui);
    free(ui->lines);
    ui->lines = NULL;
    free(ui->items);
    free(ui->selected);
    free(ui->sel_order);
    ui->sel_order = NULL;
    ui->sel_order_n = 0;
    ui->sel_order_cap = 0;
    free(ui->results);
    free(ui->search_results);
    free(ui->pending_track_key);
    free(ui->last_search_query);
    free(ui->search_override);
    free(ui->yank_buf);
    free(ui->query_buf);
    mm_delimiter_free(&ui->delim);
    mm_keymap_free(&ui->keymap);
    mm_keymap_free(&ui->disabled_keymap);
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
    ui->pending_track_key = NULL;
    ui->query_buf = NULL;
}
