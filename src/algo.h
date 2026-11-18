/* algo.h - fuzzy/exact matching and scoring, ported from fzf src/algo/algo.go */
#ifndef MMENU_ALGO_H
#define MMENU_ALGO_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "util.h"
#include "item.h"

typedef struct {
    int start;
    int end;
    int score;
} mm_result_t;

typedef struct {
    const wchar_t *runes;       /* decoded input; NULL when bytes is used */
    int length;                 /* rune count, or byte count when bytes is set */
    const unsigned char *bytes; /* ASCII bytes. NULL unless runes is NULL. */
} mm_text_t;

/* Initialize the scoring scheme ("default", "path", "history"). Returns
   false for unknown schemes. */
bool mm_algo_init(const char *scheme);

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

/* Select the fuzzy scoring implementation (1 or 2). Defaults to v2. */
bool mm_algo_set_version(int version);
mm_match_t mm_fuzzy_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                          bool case_sensitive, bool normalize, bool forward, bool with_pos);

/* Each matcher takes decoded text and a decoded pattern (code points).
   On no match, result.start == -1. pos is NULL unless with_pos and matched. */

mm_match_t mm_fuzzy_match_v1(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos);
mm_match_t mm_fuzzy_match_v2(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos);

/* Highest first-character bonus this ASCII byte can score under the
   active scheme. Case-insensitive a-z also considers A-Z. */
int16_t mm_max_first_bonus(unsigned char b, bool case_sensitive);

/* One ASCII byte against ASCII text. best_bonus < 0 computes that maximum.
   The offset and the score match fuzzyMatchV2 for a one-character pattern. */
bool mm_fuzzy_match_ascii1(const unsigned char *s, int n, unsigned char b,
                           bool case_sensitive, bool forward, int16_t best_bonus,
                           int *start, int *score);

/* One ranked hit. key == UINT64_MAX means the line is not ASCII: index is
   then the item slot, and the caller matches it with the general matcher.
   Otherwise index is the item's own index and key is the sort key. */
typedef struct {
    uint64_t key;
    int32_t index;
} mm_hit_key_t;

/* key_mode: 0 leaves key 0, 1 is score then length, 2 is score, pathname,
   length. *slow counts non-ASCII slots. Returns the number of rows written,
   or -1 if a non-ASCII line could not be represented. out holds end-begin. */
int mm_ascii1_rank(mm_item_t *items, const int *cands, int ncand,
                   int range_lo, int begin, int end, int item_count,
                   int header_lines, const bool *excluded,
                   unsigned char b, bool case_sensitive, bool forward,
                   int16_t best_bonus, int key_mode, mm_hit_key_t *out,
                   int *slow);

/* Same as mm_ascii1_rank for an ASCII pattern of two or more bytes.
   Lines longer than the byte fast path are returned as slow slots. */
int mm_ascii_n_rank(mm_item_t *items, const int *cands, int ncand,
                    int range_lo, int begin, int end, int item_count,
                    int header_lines, const bool *excluded,
                    const wchar_t *pat, int plen,
                    bool case_sensitive, bool forward,
                    int key_mode, mm_hit_key_t *out, int *slow);

/* ASCII pattern against ASCII bytes, without positions. A backward scheme
   still backtraces, so the start offset matches fzf's pathname tiebreak. */
bool mm_fuzzy_match_ascii(const unsigned char *s, int n,
                          const wchar_t *pattern, int plen,
                          bool case_sensitive, bool forward,
                          int *start, int *end, int *score);
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
