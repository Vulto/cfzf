/* result.c - see result.h. Ported from fzf src/result.go (v0.74.3). */
#define _POSIX_C_SOURCE 200809L
#include "result.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>

bool mm_scheme_criteria(const char *scheme, mm_sort_criteria_t *out) {
    if (!scheme || strcmp(scheme, "default") == 0) {
        out->crit[0] = BY_SCORE; out->crit[1] = BY_LENGTH; out->count = 2;
        return true;
    }
    if (strcmp(scheme, "path") == 0) {
        out->crit[0] = BY_SCORE; out->crit[1] = BY_PATHNAME; out->crit[2] = BY_LENGTH;
        out->count = 3;
        return true;
    }
    if (strcmp(scheme, "history") == 0) {
        out->crit[0] = BY_SCORE; out->count = 1;
        return true;
    }
    return false;
}

bool mm_parse_tiebreak(const char *str, mm_sort_criteria_t *out, const char **err) {
    if (!str || !out || !err) {
        if (err) *err = "invalid tiebreak arguments";
        return false;
    }
    out->crit[0] = BY_SCORE;
    out->count = 1;
    bool has_index = false, has_chunk = false, has_length = false;
    bool has_begin = false, has_end = false, has_pathname = false;
    char *dup = strdup(str);
    if (!dup) {
        *err = "out of memory";
        return false;
    }
    char *save = NULL;
    for (char *tok = strtok_r(dup, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        for (char *p = tok; *p; p++) *p = (char)((*p >= 'A' && *p <= 'Z') ? *p + 32 : *p);
        if (strcmp(tok, "index") == 0) {
            if (has_index) { *err = "duplicate sort criteria: index"; free(dup); return false; }
            has_index = true;
        } else if (strcmp(tok, "chunk") == 0) {
            if (has_chunk) { *err = "duplicate sort criteria: chunk"; free(dup); return false; }
            if (has_index) { *err = "index should be the last criterion"; free(dup); return false; }
            has_chunk = true;
            if (out->count >= 4) { *err = "at most 3 tiebreaks are allowed"; free(dup); return false; }
            out->crit[out->count++] = BY_CHUNK;
        } else if (strcmp(tok, "pathname") == 0) {
            if (has_pathname) { *err = "duplicate sort criteria: pathname"; free(dup); return false; }
            if (has_index) { *err = "index should be the last criterion"; free(dup); return false; }
            has_pathname = true;
            if (out->count >= 4) { *err = "at most 3 tiebreaks are allowed"; free(dup); return false; }
            out->crit[out->count++] = BY_PATHNAME;
        } else if (strcmp(tok, "length") == 0) {
            if (has_length) { *err = "duplicate sort criteria: length"; free(dup); return false; }
            if (has_index) { *err = "index should be the last criterion"; free(dup); return false; }
            has_length = true;
            if (out->count >= 4) { *err = "at most 3 tiebreaks are allowed"; free(dup); return false; }
            out->crit[out->count++] = BY_LENGTH;
        } else if (strcmp(tok, "begin") == 0) {
            if (has_begin) { *err = "duplicate sort criteria: begin"; free(dup); return false; }
            if (has_index) { *err = "index should be the last criterion"; free(dup); return false; }
            has_begin = true;
            if (out->count >= 4) { *err = "at most 3 tiebreaks are allowed"; free(dup); return false; }
            out->crit[out->count++] = BY_BEGIN;
        } else if (strcmp(tok, "end") == 0) {
            if (has_end) { *err = "duplicate sort criteria: end"; free(dup); return false; }
            if (has_index) { *err = "index should be the last criterion"; free(dup); return false; }
            has_end = true;
            if (out->count >= 4) { *err = "at most 3 tiebreaks are allowed"; free(dup); return false; }
            out->crit[out->count++] = BY_END;
        } else {
            static char msg[128];
            snprintf(msg, sizeof(msg), "invalid sort criterion: %s", tok);
            *err = msg; free(dup); return false;
        }
    }
    free(dup);
    return true;
}

static int is_space_rune(wchar_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r' || c == 0x85 || c == 0xA0;
}

void mm_result_set_bounds(mm_result_t2 *r, mm_item_t *item, int score,
                          int min_begin, int min_end, int max_end, bool valid,
                          const mm_sort_criteria_t *crit) {
    r->item = item;
    r->index = item->index;
    r->score = score;
    r->min_begin = min_begin;
    r->min_end = min_end;
    r->max_end = max_end;
    r->valid_offset = valid;
    for (int i = 0; i < MM_MAX_CRITERIA; i++) r->points[i] = 0;

    int numChars = item->text.length;
    int trimLength = -1;
    for (int idx = 0; idx < crit->count; idx++) {
        if (crit->crit[idx] == BY_LENGTH || crit->crit[idx] == BY_END) {
            trimLength = mm_item_trim_length(item);
            break;
        }
    }
    for (int idx = 0; idx < crit->count; idx++) {
        uint16_t val = 0xFFFF;
        switch (crit->crit[idx]) {
        case BY_SCORE:
            val = (uint16_t)(0xFFFF - (uint16_t)score);
            break;
        case BY_CHUNK:
            if (valid) {
                int b = min_begin, e = max_end;
                for (; b >= 1; b--) if (is_space_rune(mchars_at(&item->text, b - 1))) break;
                for (; e < numChars; e++) if (is_space_rune(mchars_at(&item->text, e))) break;
                val = (uint16_t)(e - b);
            }
            break;
        case BY_LENGTH:
            val = (uint16_t)trimLength;
            break;
        case BY_PATHNAME:
            if (valid) {
                int lastDelim = -1;
                for (int i = numChars - 1; i >= 0; i--) {
                    wchar_t c = mchars_at(&item->text, i);
                    if (c == '/' || c == '\\') { lastDelim = i; break; }
                }
                if (lastDelim <= min_begin) val = (uint16_t)(min_begin - lastDelim);
            }
            break;
        case BY_BEGIN:
        case BY_END:
            if (valid) {
                int whitePrefixLen = 0;
                for (int i = 0; i < numChars; i++) {
                    wchar_t c = mchars_at(&item->text, i);
                    whitePrefixLen = i;
                    if (i == min_begin || !is_space_rune(c)) break;
                }
                if (crit->crit[idx] == BY_BEGIN) {
                    val = (uint16_t)(min_end - whitePrefixLen);
                } else {
                    val = (uint16_t)(0xFFFF - (int)(0xFFFF * (max_end - whitePrefixLen) / (trimLength + 1)));
                }
            }
            break;
        }
        r->points[3 - idx] = val;
    }
}

static bool g_tac;

void mm_results_set_tac(bool tac) { g_tac = tac; }

int mm_result_cmp(const void *a, const void *b) {
    const mm_result_t2 *x = a, *y = b;
    for (int idx = 3; idx >= 0; idx--) {
        uint16_t l = x->points[idx], rr = y->points[idx];
        if (l < rr) return -1;
        if (l > rr) return 1;
    }
    if (x->index == y->index) return 0;
    if (g_tac) return x->index > y->index ? -1 : 1;
    return x->index < y->index ? -1 : 1;
}

/* The comparator looks at points[3] .. points[0] in that order, so the whole
   ordering fits into a single 64-bit key.  Sorting on it with a stable LSD
   radix sort is much faster than qsort() with an indirect comparator, and
   the stability gives the same "ties keep the input order" result as the
   index tie-break of mm_result_cmp(). */
typedef struct {
    uint64_t key;
    int32_t  idx;
} mm_sort_entry_t;

static mm_sort_entry_t *g_sort_e, *g_sort_tmp;
static mm_result_t2 *g_sort_out;
static size_t g_sort_cap;

static bool sort_reserve(int n) {
    if (n < 0) return false;
    size_t need = (size_t)n;
    if (need <= g_sort_cap) return true;
    if (need > SIZE_MAX / sizeof(mm_sort_entry_t) ||
        need > SIZE_MAX / sizeof(mm_result_t2))
        return false;

    /* Retain spare capacity between interactive queries. Geometric growth
       prevents a sequence of slightly larger result sets from reallocating
       three scratch buffers on every refresh. */
    size_t cap = g_sort_cap ? g_sort_cap : 256;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) { cap = need; break; }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(mm_sort_entry_t) ||
        cap > SIZE_MAX / sizeof(mm_result_t2))
        return false;

    mm_sort_entry_t *newE = malloc(cap * sizeof(*newE));
    mm_sort_entry_t *newTmp = malloc(cap * sizeof(*newTmp));
    mm_result_t2 *newOut = malloc(cap * sizeof(*newOut));
    if (!newE || !newTmp || !newOut) {
        free(newE); free(newTmp); free(newOut);
        return false;
    }

    free(g_sort_e); free(g_sort_tmp); free(g_sort_out);
    g_sort_e = newE;
    g_sort_tmp = newTmp;
    g_sort_out = newOut;
    g_sort_cap = cap;
    return true;
}

void mm_results_sort(mm_result_t2 *rs, int n) {
    if (n < 2) return;
    if (!sort_reserve(n)) { qsort(rs, (size_t)n, sizeof(mm_result_t2), mm_result_cmp); return; }
    mm_sort_entry_t *e = g_sort_e;
    mm_sort_entry_t *tmp = g_sort_tmp;
    for (int i = 0; i < n; i++) {
        const uint16_t *p = rs[i].points;
        e[i].key = ((uint64_t)p[3] << 48) | ((uint64_t)p[2] << 32) |
                   ((uint64_t)p[1] << 16) | (uint64_t)p[0];
        e[i].idx = i;
    }
    /* With --tac the tie-break runs the other way, so the input order is
       reversed before the stable sort. */
    if (g_tac) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            mm_sort_entry_t t = e[i]; e[i] = e[j]; e[j] = t;
        }
    }
    /* LSD radix sort, 8 bits per pass. */
    if (tmp) {
        size_t count[256];
        mm_sort_entry_t *src = e, *dst = tmp;
        for (int shift = 0; shift < 64; shift += 8) {
            memset(count, 0, sizeof(count));
            for (int i = 0; i < n; i++) count[(src[i].key >> shift) & 0xFF]++;
            size_t sum = 0;
            for (int b = 0; b < 256; b++) { size_t c = count[b]; count[b] = sum; sum += c; }
            for (int i = 0; i < n; i++) dst[count[(src[i].key >> shift) & 0xFF]++] = src[i];
            mm_sort_entry_t *t = src; src = dst; dst = t;
        }
        /* The result may live in either buffer. */
        const mm_sort_entry_t *sorted = (src == e) ? e : tmp;
        mm_result_t2 *out = g_sort_out;
        if (out) {
            for (int i = 0; i < n; i++) out[i] = rs[sorted[i].idx];
            memcpy(rs, out, (size_t)n * sizeof(mm_result_t2));
        } else {
            for (int i = 0; i < n; i++) rs[i] = rs[sorted[i].idx];
        }
    } else {
        /* No room for the scratch buffer: fall back to the plain sort. */
        for (int i = 0; i < n; i++) rs[i] = rs[e[i].idx];
    }
    /* e, tmp and out are reused across calls; not freed here. */
}


bool mm_criteria_forward(const mm_sort_criteria_t *crit) {
    bool forward = true;
    for (int idx = crit->count - 1; idx > 0; idx--) {
        switch (crit->crit[idx]) {
        case BY_END:
        case BY_PATHNAME:
            forward = false;
            break;
        case BY_BEGIN:
            forward = true;
            break;
        default:
            break;
        }
    }
    return forward;
}
