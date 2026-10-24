/* history.c - query history file. */
#define _POSIX_C_SOURCE 200809L
#include "history.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool history_write(mm_history_t *h) {
    if (!h->path) return false;
    FILE *f = fopen(h->path, "w");
    if (!f) return false;
    /* The lines are joined with newlines; the trailing empty entry does not
       add one, so the file ends with a single newline. */
    for (int i = 0; i < h->n; i++) {
        if (i > 0) fputc('\n', f);
        fputs(h->lines[i], f);
    }
    fclose(f);
    return true;
}

mm_history_t *mm_history_load(const char *path, int max_size) {
    if (!path || !*path) return NULL;
    mm_history_t *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->path = strdup(path);
    h->max_size = max_size > 0 ? max_size : MM_HISTORY_MAX;
    if (!h->path) { free(h); return NULL; }

    FILE *f = fopen(path, "r");
    if (!f) {
        if (errno != ENOENT) { free(h->path); free(h); return NULL; }
        /* Create the file so that the history can be written later. */
        f = fopen(path, "w");
        if (!f) { free(h->path); free(h); return NULL; }
        fclose(f);
    } else {
        char *line = NULL;
        size_t cap = 0;
        ssize_t len;
        int cap_lines = 8;
        h->lines = malloc((size_t)cap_lines * sizeof(char *));
        if (!h->lines) { fclose(f); free(h->path); free(h); return NULL; }
        while ((len = getline(&line, &cap, f)) != -1) {
            while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
            if (h->n == cap_lines) {
                cap_lines *= 2;
                char **nl = realloc(h->lines, (size_t)cap_lines * sizeof(char *));
                if (!nl) { free(line); fclose(f); free(h->lines); free(h->path); free(h); return NULL; }
                h->lines = nl;
            }
            h->lines[h->n] = strdup(line);
            if (!h->lines[h->n]) { free(line); fclose(f); free(h->lines); free(h->path); free(h); return NULL; }
            h->n++;
        }
        free(line);
        fclose(f);
    }
    /* The last line is the (empty) current entry. */
    if (h->n == 0 || (h->n > 0 && h->lines[h->n - 1][0] != 0)) {
        char **nl = realloc(h->lines, (size_t)(h->n + 1) * sizeof(char *));
        if (!nl) { mm_history_free(h); return NULL; }
        h->lines = nl;
        h->lines[h->n] = strdup("");
        if (!h->lines[h->n]) { mm_history_free(h); return NULL; }
        h->n++;
    }
    h->cursor = h->n - 1;
    return h;
}

void mm_history_append(mm_history_t *h, const char *query) {
    if (!h || !query || !*query) return;
    /* Replace the current (last) entry with the new query and add a fresh
       empty entry, the way fzf does. */
    free(h->lines[h->n - 1]);
    h->lines[h->n - 1] = strdup(query);
    if (!h->lines[h->n - 1]) return;
    char **nl = realloc(h->lines, (size_t)(h->n + 1) * sizeof(char *));
    if (!nl) return;
    h->lines = nl;
    h->lines[h->n] = strdup("");
    if (!h->lines[h->n]) return;
    h->n++;
    h->cursor = h->n - 1;
    while (h->n - 1 > h->max_size) {
        free(h->lines[0]);
        memmove(h->lines, h->lines + 1, (size_t)(h->n - 1) * sizeof(char *));
        h->n--;
    }
    history_write(h);
}

const char *mm_history_prev(mm_history_t *h) {
    if (!h || h->cursor <= 0) return h ? h->lines[h->cursor] : NULL;
    h->cursor--;
    return h->lines[h->cursor];
}

const char *mm_history_next(mm_history_t *h) {
    if (!h) return NULL;
    if (h->cursor < h->n - 1) h->cursor++;
    return h->lines[h->cursor];
}

const char *mm_history_current(mm_history_t *h) {
    if (!h || h->n == 0) return "";
    return h->lines[h->cursor];
}

void mm_history_free(mm_history_t *h) {
    if (!h) return;
    for (int i = 0; i < h->n; i++) free(h->lines[i]);
    free(h->lines);
    free(h->path);
    free(h);
}
