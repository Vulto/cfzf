#define _POSIX_C_SOURCE 200809L
#include "../src/pattern.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/types.h>
#include <wchar.h>
#include <wctype.h>

typedef struct { int score; int order; char *line; int trimlen; } item_t;

static int trim_length(const wchar_t *r, int n) {
    int i; for (i = n - 1; i >= 0; i--) if (!iswspace(r[i])) break;
    if (i < 0) return 0;
    int j; for (j = 0; j < n; j++) if (!iswspace(r[j])) break;
    return i - j + 1;
}
static int cmp_items(const void *a, const void *b) {
    const item_t *x = a, *y = b;
    if (x->score != y->score) return y->score - x->score;
    if (x->trimlen != y->trimlen) return x->trimlen - y->trimlen;
    return x->order - y->order;
}

static char *g_dup;
static mm_text_t decode(const char *s) {
    free(g_dup); g_dup = strdup(s);
    mchars_t c; mchars_init(&c, g_dup); mchars_decode(&c);
    mm_text_t t = { c.runes, c.length };
    return t;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (!mm_algo_init("default")) return 2;
    if (argc < 2) { fprintf(stderr, "usage: %s <query> [--extended] [--exact]\n", argv[0]); return 2; }
    const char *query = argv[1];
    bool extended = false, fuzzy = true;
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--extended")) extended = true;
        else if (!strcmp(argv[i], "--exact")) fuzzy = false;
    }
    mm_pattern_t *p = mm_pattern_build(query, fuzzy, extended, CASE_SMART, false, true, false);

    item_t *items = NULL; int n = 0, cap = 0;
    char *line = NULL; size_t lcap = 0; ssize_t len; int order = 0;
    while ((len = getline(&line, &lcap, stdin)) >= 0) {
        if (len && line[len-1] == '\n') line[--len] = 0;
        mm_text_t t = decode(line);
        int score = 0;
        bool ok = mm_pattern_match(p, &t, &score, NULL, NULL);
        if (ok) {
            if (n == cap) { cap = cap ? cap*2 : 64; items = realloc(items, (size_t)cap*sizeof(item_t)); }
            items[n].score = score; items[n].order = order++;
            items[n].line = strdup(line);
            items[n].trimlen = trim_length(t.runes, t.length);
            n++;
        }
    }
    if (p->sortable) qsort(items, n, sizeof(item_t), cmp_items);
    for (int i = 0; i < n; i++) printf("%s\n", items[i].line);
    return 0;
}
