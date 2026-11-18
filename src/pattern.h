/* pattern.h - query term parsing and matching, ported from fzf src/pattern.go */
#ifndef MMENU_PATTERN_H
#define MMENU_PATTERN_H

#include "algo.h"
#include <stdbool.h>

typedef enum {
    CASE_RESPECT = 0,
    CASE_IGNORE,
    CASE_SMART,
} mm_case_t;

typedef enum {
    TERM_FUZZY = 0,
    TERM_EXACT,
    TERM_EXACT_BOUNDARY,
    TERM_PREFIX,
    TERM_SUFFIX,
    TERM_EQUAL,
} mm_term_type_t;

typedef struct {
    mm_term_type_t typ;
    bool inv;
    bool case_sensitive;
    bool normalize;
    wchar_t *text;
    int tlen;
} mm_term_t;

typedef struct {
    mm_term_t *terms;
    int count;
} mm_termset_t;

typedef struct {
    bool fuzzy;
    bool extended;
    bool case_sensitive;
    bool normalize;
    bool forward;
    bool with_pos;
    wchar_t *text; /* full query text (as given, adjusted for case) */
    int textlen;
    mm_termset_t *term_sets;
    int ntermsets;
    bool sortable;
} mm_pattern_t;

/* Build a pattern from a query (already UTF-8). fuzzy=false means exact mode.
   with_pos controls position tracking. */
mm_pattern_t *mm_pattern_build(const char *query, bool fuzzy, bool extended,
                               mm_case_t case_mode, bool normalize, bool forward,
                               bool with_pos);
void mm_pattern_free(mm_pattern_t *p);
bool mm_pattern_is_empty(const mm_pattern_t *p);

/* Override sortability (fzf +s / --no-sort). */
void mm_pattern_set_sortable(mm_pattern_t *p, bool sortable);

/* Match a decoded text against the pattern. Returns true when matched, and
   if p->with_pos, fills *pos (malloc'd, caller frees) with matched positions
   and sets *npos. Sets *score to total score. */
bool mm_pattern_match(const mm_pattern_t *p, const mm_text_t *text, int *score,
                      int **pos, int *npos);

/* Match without allocating a position list. On success, *begin is the first
   matched rune and *end is one past the last, or both are -1 when the match
   has no offsets. Those bounds are the min and max of the position list. */
bool mm_pattern_match_span(const mm_pattern_t *p, const mm_text_t *text,
                           int *score, int *begin, int *end);

/* A search segment (e.g. an --nth token) with its rune prefix offset. */
typedef struct {
    mm_text_t text;
    int prefix_length;
} mm_token_t;

/* Match against a list of token segments (fzf --nth). Each term is tried on
   each segment; matched positions are offset by prefix_length. */
bool mm_pattern_match_tokens(const mm_pattern_t *p, const mm_token_t *tokens, int ntokens,
                             int *score, int **pos, int *npos);

#endif /* MMENU_PATTERN_H */
