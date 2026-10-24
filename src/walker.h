/* walker.h - directory traversal, used when fzf is started without piped
   input (fzf lists the files under the current directory).
   Port of the behaviour of fzf's src/reader.go readFiles / fastwalk. */
#ifndef MM_WALKER_H
#define MM_WALKER_H

#include <stdbool.h>

typedef struct {
    bool file;    /* emit regular files (default) */
    bool dir;     /* emit directories */
    bool follow;  /* follow symlinks (default) */
    bool hidden;  /* include hidden entries (default) */
    /* Directory names to skip (--walker-skip), NULL terminated. */
    const char **skip;
    int         nskip;
} mm_walker_opts_t;

/* Default options, matching fzf: file, follow, hidden. */
void mm_walker_default(mm_walker_opts_t *opts);

/* Parse a --walker specification ("file,dir,follow,hidden"). Returns false
   on an unknown option. */
bool mm_walker_parse(const char *spec, mm_walker_opts_t *opts);

/* Walk `root` recursively and collect the paths. The paths are relative to
   the root and do not carry a "./" prefix. Returns a malloc'd array of
   malloc'd strings; the caller frees both. Returns false on allocation
   failure. */
bool mm_walk(const char *root, const mm_walker_opts_t *opts,
              char ***out_paths, int *out_n);

/* Walk `root` recursively, writing each path followed by a newline to `fd`.
   The descriptor is closed before returning. */
void mm_walk_stream(int fd, const char *root, const mm_walker_opts_t *opts);

/* Free the result of mm_walk. */
void mm_walk_free(char **paths, int n);

#endif /* MM_WALKER_H */
