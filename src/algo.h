/* algo.h - fuzzy/exact matching and scoring, ported from fzf src/algo/algo.go */
#ifndef MMENU_ALGO_H
#define MMENU_ALGO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "util.h"

typedef struct {
    int start;
    int end;
    int score;
} mm_result_t;

typedef struct {
    const wchar_t *runes; /* decoded input */
    int length;           /* rune count */
} mm_text_t;

/* Initialize the scoring scheme ("default", "path", "history"). Returns
   false for unknown schemes. */
bool mm_algo_init(const char *scheme);

/* Select the fuzzy scoring implementation (1 or 2). Defaults to v2. */
bool mm_algo_set_version(int version);
mm_match_t mm_fuzzy_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                          bool case_sensitive, bool normalize, bool forward, bool with_pos);

/* Normalize a single code point (Latin-1 + Latin Extended-A/B accents to base
   letters). Returns the code point unchanged for unmapped values. */
wchar_t mm_algo_normalize_rune(wchar_t r);

typedef struct {
    bool case_sensitive;
    bool normalize;
    bool forward;
    bool with_pos;
    mm_result_t result;
    int *pos; /* matched rune positions (allocated when with_pos and matched) */
    int npos;
} mm_match_t;

/* Each matcher takes decoded text and a decoded pattern (code points).
   On no match, result.start == -1. pos is NULL unless with_pos and matched. */

mm_match_t mm_fuzzy_match_v1(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_fuzzy_match_v2(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_exact_match_naive(const mm_text_t *text, const wchar_t *pattern, int plen,
                                bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_exact_match_boundary(const mm_text_t *text, const wchar_t *pattern, int plen,
                                   bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_prefix_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                           bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_suffix_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                           bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_equal_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                          bool case_sensitive, bool normalize, bool forward, bool with_pos);

void mm_match_free(mm_match_t *m);

#endif /* MMENU_ALGO_H */
