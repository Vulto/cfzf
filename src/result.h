/* result.h - match result and sort criteria, ported from fzf src/result.go */
#ifndef MMENU_RESULT_H
#define MMENU_RESULT_H

#include "item.h"
#include <stdbool.h>
#include <stdint.h>

typedef enum {
    BY_SCORE = 0,
    BY_CHUNK,
    BY_LENGTH,
    BY_BEGIN,
    BY_END,
    BY_PATHNAME,
} mm_criterion_t;

#define MM_MAX_CRITERIA 5

typedef struct {
    mm_criterion_t crit[MM_MAX_CRITERIA];
    int count; /* includes leading BY_SCORE */
} mm_sort_criteria_t;

/* Parse a scheme name ("default"/"path"/"history"). Returns false on error. */
bool mm_scheme_criteria(const char *scheme, mm_sort_criteria_t *out);

/* Parse a --tiebreak string like "length,begin". Returns false on error,
   sets *err to a static message. */
bool mm_parse_tiebreak(const char *str, mm_sort_criteria_t *out, const char **err);

/* fzf derives the matcher direction from the tiebreak criteria: if the second
   criterion is byEnd or byPathname, matching runs backward (forward=false).
   Returns true when matching should be forward. */
bool mm_criteria_forward(const mm_sort_criteria_t *crit);

typedef struct {
    mm_item_t *item;
    int32_t index;
    uint16_t points[MM_MAX_CRITERIA];
    int score;
    int32_t min_begin;
    int32_t min_end;
    int32_t max_end;
    bool valid_offset;
    int *pos;          /* the matched rune offsets (owned) */
    int  npos;
} mm_result_t2;

/* Build points from bounds (mirrors buildResultFromBounds). */
void mm_result_set_bounds(mm_result_t2 *r, mm_item_t *item, int score,
                          int min_begin, int min_end, int max_end, bool valid,
                          const mm_sort_criteria_t *crit);

/* Compare two results for ordering (a before b -> <0). */
int mm_result_cmp(const void *a, const void *b);

/* sort a result array in place */
void mm_results_sort(mm_result_t2 *rs, int n);

/* set global tac flag for index tiebreak direction */
void mm_results_set_tac(bool tac);

#endif /* MMENU_RESULT_H */
