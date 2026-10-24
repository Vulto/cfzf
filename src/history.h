/* history.h - query history file (fzf's --history).
   Port of fzf's src/history.go. */
#ifndef MM_HISTORY_H
#define MM_HISTORY_H

#include <stdbool.h>

#define MM_HISTORY_MAX 10000

typedef struct {
    char  *path;
    char **lines;     /* one query per line; the last one is the current one */
    int    n;
    int    cursor;
    int    max_size;
} mm_history_t;

/* Load the history file, creating it when it does not exist. Returns NULL on
   failure. */
mm_history_t *mm_history_load(const char *path, int max_size);

/* Append a query to the history (empty queries are ignored) and write the
   file back. */
void mm_history_append(mm_history_t *h, const char *query);

/* Move the cursor and return the query at the new position. The returned
   pointer is owned by the history. */
const char *mm_history_prev(mm_history_t *h);
const char *mm_history_next(mm_history_t *h);

/* The query the cursor currently points at. */
const char *mm_history_current(mm_history_t *h);

/* Release the history. */
void mm_history_free(mm_history_t *h);

#endif /* MM_HISTORY_H */
