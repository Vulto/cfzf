/* ansi.h - ANSI escape-sequence parsing and color extraction (port of
   fzf src/ansi.go). */
#ifndef MM_ANSI_H
#define MM_ANSI_H

#include "tui.h"
#include <stdbool.h>
#include <stddef.h>

typedef struct mm_url_t {
    const char *uri;
    const char *params;
} mm_url_t;

typedef struct mm_ansi_state_t {
    mm_color_t fg, bg, ul;
    mm_attr_t  attr;
    mm_color_t lbg;
    const mm_url_t *url;
} mm_ansi_state_t;

typedef struct mm_ansi_offset_t {
    int32_t          offset[2];
    mm_ansi_state_t  color;
} mm_ansi_offset_t;

typedef struct mm_ansi_offsets_t {
    mm_ansi_offset_t *ptr;
    int               len;
    int               cap;
} mm_ansi_offsets_t;

/* Returns bytes consumed by the escape sequence starting at s[0], or 0 if no
   escape sequence begins at s. Equivalent to Go's nextAnsiEscapeSequence
   returning (start,end). out_start / out_end are byte indices relative to
   s, or -1 if none found. */
void mm_next_ansi_escape_sequence(const char *s, size_t len,
                                  int *out_start, int *out_end);

/* Interpret a single ANSI code token (as produced by
   nextAnsiEscapeSequence) against prev_state. Returns the new state. */
mm_ansi_state_t mm_interpret_code(const char *code, size_t code_len,
                                  const mm_ansi_state_t *prev_state);

/* Extract ANSI codes from str, calling proc(prev, state) for each plain
   segment (proc may be NULL). Returns the stripped string (malloc'd, caller
   frees) via *out_trimmed, offsets via *out_offsets (malloc'd, freed by
   mm_ansi_offsets_free), and the final state via out_have_state / out_state
   (fzf's state is nil when nothing is colored; we signal that via
   *out_have_state == false). state is the incoming state (may be NULL). */
void mm_extract_color(const char *str, size_t len, mm_ansi_state_t *state,
                      bool (*proc)(const char *seg, size_t seg_len,
                                   mm_ansi_state_t *st, void *ud),
                      void *ud,
                      char **out_trimmed, size_t *out_trimmed_len,
                      mm_ansi_offsets_t *out_offsets,
                      bool *out_have_state, mm_ansi_state_t *out_state);

void mm_ansi_offsets_free(mm_ansi_offsets_t *o);

bool mm_ansi_state_colored(const mm_ansi_state_t *s);
bool mm_ansi_state_equals(const mm_ansi_state_t *s,
                          const mm_ansi_state_t *t);

#endif /* MM_ANSI_H */
