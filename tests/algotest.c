#include "../src/algo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>

static mm_text_t decode(const char *s) {
    char *dup = strdup(s);
    mchars_t c; mchars_init(&c, dup);
    mchars_decode(&c);
    mm_text_t t = { c.runes, c.length };
    return t;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (!mm_algo_init("default")) { fprintf(stderr, "init fail\n"); return 2; }
    if (argc < 4) { fprintf(stderr, "usage: %s <mode> <pattern> <text>\n", argv[0]); return 2; }
    const char *mode = argv[1];
    char *pat = strdup(argv[2]);
    mchars_t pc; mchars_init(&pc, pat); mchars_decode(&pc);
    mm_text_t t = decode(argv[3]);
    mm_match_t m;
    if (!strcmp(mode, "v1")) m = mm_fuzzy_match_v1(&t, pc.runes, pc.length, false, false, true, false);
    else if (!strcmp(mode, "v2")) m = mm_fuzzy_match_v2(&t, pc.runes, pc.length, false, false, true, true);
    else if (!strcmp(mode, "exact")) m = mm_exact_match_naive(&t, pc.runes, pc.length, false, false, true, false);
    else if (!strcmp(mode, "boundary")) m = mm_exact_match_boundary(&t, pc.runes, pc.length, false, false, true, false);
    else if (!strcmp(mode, "prefix")) m = mm_prefix_match(&t, pc.runes, pc.length, false, false, true, false);
    else if (!strcmp(mode, "suffix")) m = mm_suffix_match(&t, pc.runes, pc.length, false, false, true, false);
    else if (!strcmp(mode, "equal")) m = mm_equal_match(&t, pc.runes, pc.length, false, false, true, false);
    else return 2;
    if (m.result.start < 0) { printf("no-match\n"); return 1; }
    printf("start=%d end=%d score=%d", m.result.start, m.result.end, m.result.score);
    if (m.pos) { printf(" pos="); for (int i = 0; i < m.npos; i++) printf("%s%d", i?",":"", m.pos[i]); }
    printf("\n");
    return 0;
}
