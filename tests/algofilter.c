#define _POSIX_C_SOURCE 200809L
#include "../src/algo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <sys/types.h>
#include <wchar.h>
#include <wctype.h>

typedef struct { int score; int order; char *line; int start; int end; int trimlen; } item_t;

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
    free(g_dup);
    g_dup = strdup(s);
    mchars_t c; mchars_init(&c, g_dup);
    mchars_decode(&c);
    mm_text_t t = { c.runes, c.length };
     return t;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (!mm_algo_init("default")) return 2;
    if (argc < 3) { fprintf(stderr, "usage: %s <mode> <pattern> [cs]  (corpus on stdin)\n", argv[0]); return 2; }
    const char *mode = argv[1];
    int force_cs = (argc >= 4 && !strcmp(argv[3], "cs"));
    char *pat = strdup(argv[2]);
    mchars_t pc; mchars_init(&pc, pat); mchars_decode(&pc);
    /* smart-case: lowercase pattern unless it has an ASCII uppercase letter */
    int cs = force_cs;
    if (!cs) {
        cs = 0;
        for (int i = 0; i < pc.length; i++)
            if (pc.runes[i] >= 'A' && pc.runes[i] <= 'Z') { cs = 1; break; }
        if (!cs) for (int i = 0; i < pc.length; i++)
            if (pc.runes[i] >= 'A' && pc.runes[i] <= 'Z') pc.runes[i] += 32;
    }

    item_t *items = NULL; int n = 0, cap = 0;
    char *line = NULL; size_t lcap = 0; ssize_t len;
    int order = 0;
    while ((len = getline(&line, &lcap, stdin)) >= 0) {
        if (len && line[len-1] == '\n') line[--len] = 0;
        mm_text_t t = decode(line);
        mm_match_t m;
        if (!strcmp(mode, "v1")) m = mm_fuzzy_match_v1(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "v2")) m = mm_fuzzy_match_v2(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "exact")) m = mm_exact_match_naive(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "boundary")) m = mm_exact_match_boundary(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "prefix")) m = mm_prefix_match(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "suffix")) m = mm_suffix_match(&t, pc.runes, pc.length, cs, false, true, false);
        else if (!strcmp(mode, "equal")) m = mm_equal_match(&t, pc.runes, pc.length, cs, false, true, false);
        else return 2;
        int has = m.result.start >= 0;
        mm_match_free(&m);
        if (has) {
            if (n == cap) { cap = cap ? cap*2 : 64; items = realloc(items, cap*sizeof(item_t)); }
            items[n].score = m.result.score;
            items[n].order = order++;
            items[n].line = strdup(line);
            items[n].start = m.result.start; items[n].end = m.result.end;
            items[n].trimlen = trim_length(t.runes, t.length);
            n++;
        }
    }
    /* sort: score desc (memcmp int compare), tie -> input order */
    qsort(items, n, sizeof(item_t), cmp_items);
    for (int i = 0; i < n; i++) printf("%s\n", items[i].line);
    return 0;
}
