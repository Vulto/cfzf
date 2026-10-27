/* filter.h - batch filter (--filter) search over items, sorted per criteria */
#ifndef MMENU_FILTER_H
#define MMENU_FILTER_H

#include "item.h"
#include "result.h"
#include "pattern.h"
#include "tokenizer.h"

typedef struct {
    char **lines;
    int count;
    /* Optional: items that have already been decoded.  When they are given,
       the search reuses them instead of decoding the lines again, which
       matters for the interactive mode where every keystroke triggers a
       new search. */
    mm_item_t *items;
    /* Index of the first line within the caller's item list, so that the
       results refer to the original items (used by --header-lines). */
    int        base;
    /* Optional --delimiter and --nth transform. If nth_ranges is NULL, items
       are searched whole. */
    mm_delimiter_t delim;
    const mm_range_t *nth_ranges;
    int nth_n;
    /* Independent display/search transformation for --with-nth. */
    const mm_range_t *with_nth_ranges;
    int with_nth_n;
} mm_input_t;

/* The rune offsets of the matched characters of one result. */
typedef struct {
    int *pos;
    int  npos;
} mm_match_pos_t;

/* Set the maximum worker count for subsequent batch searches. Zero keeps the
   default/environment-controlled worker count. */
void mm_filter_set_threads(int threads);

/* Run a batch search. Returns a malloc'd array of result indices (into lines),
   sets *out_n.  When out_pos is not NULL, *out_pos receives a malloc'd array
   with the matched offsets of every result; the caller frees it with
   mm_match_pos_free(). */
int *mm_batch_search(mm_input_t *input, mm_pattern_t *pat, const mm_sort_criteria_t *crit,
                     bool tac, int *out_n, mm_match_pos_t **out_pos);

/* Release the match positions returned by mm_batch_search(). */
void mm_match_pos_free(mm_match_pos_t *pos, int n);

#endif /* MMENU_FILTER_H */
