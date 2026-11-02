/* walker.c - recursive directory traversal (fzf's default file listing). */
#define _POSIX_C_SOURCE 200809L
#include "walker.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Guard against symlink loops. */
#define MM_WALK_MAX_DEPTH 256

typedef struct {
    char *name;
    bool  is_dir;
    bool  is_reg;
    bool  link_to_dir;
} mm_walk_entry_t;

typedef struct {
    char **items;
    int    n;
    int    cap;
    bool   failed;
} mm_pathlist_t;

#define MM_WALK_STREAM_BUFFER_SIZE (64u * 1024u)
#define MM_WALK_STREAM_FLUSH_THRESHOLD (4u * 1024u)

typedef struct {
    int fd;
    char *buffer;
    size_t len;
    size_t cap;
    bool failed;
} mm_walk_stream_t;

void mm_walker_default(mm_walker_opts_t *opts) {
    if (!opts) return;
    opts->file = true;
    opts->dir = false;
    opts->follow = true;
    opts->hidden = true;
    static const char *default_skip[] = { ".git", "node_modules" };
    opts->skip = default_skip;
    opts->nskip = 2;
}

bool mm_walker_parse(const char *spec, mm_walker_opts_t *opts) {
    if (!opts) return false;
    opts->file = opts->dir = opts->follow = opts->hidden = false;
    opts->skip = NULL;
    opts->nskip = 0;
    if (!spec || !*spec) return true;
    const char *p = spec;
    while (*p) {
        const char *comma = strchr(p, ',');
        size_t len = comma ? (size_t)(comma - p) : strlen(p);
        if (len == 4 && !memcmp(p, "file", 4)) opts->file = true;
        else if (len == 3 && !memcmp(p, "dir", 3)) opts->dir = true;
        else if (len == 6 && !memcmp(p, "follow", 6)) opts->follow = true;
        else if (len == 6 && !memcmp(p, "hidden", 6)) opts->hidden = true;
        else return false;
        if (!comma) break;
        p = comma + 1;
    }
    return true;
}

static bool plist_push(mm_pathlist_t *pl, char *path) {
    if (pl->n == pl->cap) {
        if (pl->cap > INT_MAX / 2) { pl->failed = true; return false; }
        int cap = pl->cap ? pl->cap * 2 : 64;
        if ((size_t)cap > SIZE_MAX / sizeof(char *)) {
            pl->failed = true;
            return false;
        }
        char **items = realloc(pl->items, (size_t)cap * sizeof(char *));
        if (!items) { pl->failed = true; return false; }
        pl->items = items;
        pl->cap = cap;
    }
    pl->items[pl->n++] = path;
    return true;
}

static bool write_all(int fd, const char *buf, size_t len) {
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, buf + off, len - off);
        if (w < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (w == 0) return false;
        off += (size_t)w;
    }
    return true;
}

static bool stream_flush(mm_walk_stream_t *stream) {
    if (!stream || stream->failed) return false;
    if (stream->len == 0) return true;
    if (!write_all(stream->fd, stream->buffer, stream->len)) {
        stream->failed = true;
        stream->len = 0;
        return false;
    }
    stream->len = 0;
    return true;
}

/* Append a complete path record; flush in batches instead of issuing two
   syscalls per path. Flush at directory boundaries once 4 KiB is buffered so
   recursive walks continue streaming without waiting for the entire tree. */
static bool stream_write_line(mm_walk_stream_t *stream, const char *path,
                              size_t len, bool slash) {
    if (!stream || stream->failed) return false;
    size_t extra = (slash ? 1u : 0u) + 1u;
    size_t total = len + extra;
    if (total > stream->cap) {
        if (!stream_flush(stream) ||
            !write_all(stream->fd, path, len) ||
            (slash && !write_all(stream->fd, "/", 1)) ||
            !write_all(stream->fd, "\n", 1)) {
            stream->failed = true;
            return false;
        }
        return true;
    }
    if (stream->len + total > stream->cap && !stream_flush(stream))
        return false;
    memcpy(stream->buffer + stream->len, path, len);
    stream->len += len;
    if (slash) stream->buffer[stream->len++] = '/';
    stream->buffer[stream->len++] = '\n';
    return true;
}

/* Emit a path, either into the collected list or as a line on the stream. */
static bool emit_path(mm_pathlist_t *pl, int out_fd,
                      mm_walk_stream_t *stream, const char *path) {
    if (out_fd >= 0) {
        size_t len = strlen(path);
        if (stream) return stream_write_line(stream, path, len, false);
        return write_all(out_fd, path, len) && write_all(out_fd, "\n", 1);
    }
    char *copy = strdup(path);
    if (!copy) { if (pl) pl->failed = true; return false; }
    if (!plist_push(pl, copy)) { free(copy); return false; }
    return true;
}

static bool emit_dir(mm_pathlist_t *pl, int out_fd,
                     mm_walk_stream_t *stream, const char *path) {
    size_t len = strlen(path);
    if (out_fd >= 0 && stream)
        return stream_write_line(stream, path, len, true);
    char *withslash = malloc(len + 2);
    if (!withslash) { if (pl) pl->failed = true; return false; }
    memcpy(withslash, path, len);
    withslash[len] = '/';
    withslash[len + 1] = 0;
    if (out_fd >= 0) {
        bool ok = write_all(out_fd, withslash, len + 1) &&
                  write_all(out_fd, "\n", 1);
        free(withslash);
        return ok;
    }
    if (!plist_push(pl, withslash)) { free(withslash); return false; }
    return true;
}

static const char *walk_entry_name(const mm_walk_entry_t *entry) {
    return entry->name;
}

static int walk_entry_cmp(const void *a, const void *b) {
    const mm_walk_entry_t *x = a;
    const mm_walk_entry_t *y = b;
    if (x->is_reg && !y->is_reg) return -1;
    if (!x->is_reg && y->is_reg) return 1;
    if (!x->is_reg && !y->is_reg) {
        if (!x->is_dir && y->is_dir) return -1;
        if (x->is_dir && !y->is_dir) return 1;
    }
    return strcmp(walk_entry_name(x), walk_entry_name(y));
}

/* Is `ancestor` equal to or a parent directory of `path`? */
static bool path_is_ancestor(const char *ancestor, const char *path) {
    size_t n = strlen(ancestor);
    if (strncmp(ancestor, path, n) != 0) return false;
    if (ancestor[n] == '\0') return true;
    if (ancestor[n] != '/') return false;
    /* The root itself is written without a trailing slash. */
    if (n == 1 && ancestor[0] == '/') return true;
    return true;
}

/* Match the directory ignore rules used by fzf's reader.go:
   - a name without a separator matches every directory with that basename;
   - a relative path matches that exact path and paths ending in /path;
   - an absolute path is matched as a suffix, as in reader.go's ignoresSuffix.
   Files are deliberately never filtered by --walker-skip. */
static bool walk_skip_dir(const mm_walker_opts_t *opts,
                          const char *path, const char *name) {
    if (!opts || !opts->skip) return false;
    for (int i = 0; i < opts->nskip; i++) {
        const char *ignore = opts->skip[i];
        if (!ignore || !*ignore) continue;
        if (!strchr(ignore, '/')) {
            if (!strcmp(ignore, name)) return true;
            continue;
        }
        size_t path_len = strlen(path);
        size_t ignore_len = strlen(ignore);
        if (ignore[0] == '/') {
            if (path_len >= ignore_len &&
                !memcmp(path + path_len - ignore_len, ignore, ignore_len))
                return true;
        } else if (path_len == ignore_len) {
            if (!memcmp(path, ignore, ignore_len)) return true;
        } else if (path_len > ignore_len &&
                   path[path_len - ignore_len - 1] == '/' &&
                   !memcmp(path + path_len - ignore_len, ignore, ignore_len)) {
            return true;
        }
    }
    return false;
}

/* Build "path/name", omitting the "./" prefix when the walker starts at ".". */
static char *join_path(const char *path, const char *name) {
    if (path[0] == '.' && path[1] == '\0')
        return strdup(name);
    size_t plen = strlen(path);
    bool slash = plen > 0 && path[plen - 1] == '/';
    size_t nlen = strlen(name);
    char *out = malloc(plen + (slash ? 0 : 1) + nlen + 1);
    if (!out) return NULL;
    memcpy(out, path, plen);
    size_t o = plen;
    if (!slash) out[o++] = '/';
    memcpy(out + o, name, nlen);
    out[o + nlen] = 0;
    return out;
}

/* Follow a symlink to a directory, rejecting loops back to the walker root
   (or the filesystem root). Returns true when the target may be traversed. */
static bool follow_symlink_dir(const char *child, const char *root_abs) {
    char *target = realpath(child, NULL);
    if (!target) return true;
    bool ok = true;
    if (!strcmp(target, "/") || strcmp(target, root_abs) == 0)
        ok = false;
    else if (path_is_ancestor(target, root_abs))
        ok = false;
    free(target);
    return ok;
}

static void walk_dir(const char *path, const mm_walker_opts_t *opts,
                     mm_pathlist_t *pl, int out_fd, mm_walk_stream_t *stream,
                     const char *root_abs, int depth) {
    if (depth > MM_WALK_MAX_DEPTH || (stream && stream->failed)) return;
    DIR *d = opendir(path);
    if (!d) return;

    /* Collect the entries and sort them the way fastwalk does with
       SortFilesFirst: regular files first, then other non-directories, then
       directories; each group sorted by name. */
    mm_walk_entry_t *ents = NULL;
    int n = 0, cap = 0;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        const char *entry_name = de->d_name;
        if (!strcmp(entry_name, ".") || !strcmp(entry_name, "..")) continue;

        if (n == cap) {
            if (cap > INT_MAX / 2 || (size_t)(cap ? cap * 2 : 64) > SIZE_MAX / sizeof(*ents)) {
                if (pl) pl->failed = true;
                closedir(d);
                goto done;
            }
            int ncap = cap ? cap * 2 : 64;
            mm_walk_entry_t *ne = realloc(ents, (size_t)ncap * sizeof(*ne));
            if (!ne) { if (pl) pl->failed = true; closedir(d); goto done; }
            ents = ne;
            cap = ncap;
        }
        bool is_link = false, is_dir = false, link_to_dir = false;
        bool type_known = false;
#if defined(DT_UNKNOWN) && defined(DT_DIR) && defined(DT_LNK) && defined(DT_REG)
        /* Most Linux filesystems expose d_type, so ordinary entries can be
           classified without allocating a full path or calling lstat. */
        switch (de->d_type) {
            case DT_UNKNOWN:
                break;
            case DT_DIR:
                is_dir = true;
                type_known = true;
                break;
            case DT_LNK:
                is_link = true;
                type_known = true;
                break;
            default:
                type_known = true;
                break;
        }
#endif
        char *child = NULL;
        if (!type_known || is_link) {
            child = join_path(path, entry_name);
            if (!child) { if (pl) pl->failed = true; closedir(d); goto done; }
        }
        if (!type_known) {
            struct stat lst;
            if (lstat(child, &lst) == 0) {
                is_link = S_ISLNK(lst.st_mode);
                is_dir = S_ISDIR(lst.st_mode);
                type_known = true;
            }
        }
        if (is_link) {
            struct stat st;
            if (!child) child = join_path(path, entry_name);
            if (!child) { if (pl) pl->failed = true; closedir(d); goto done; }
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                link_to_dir = true;
                is_dir = opts->follow && follow_symlink_dir(child, root_abs);
            }
        }
        free(child);
        char *name_copy = strdup(entry_name);
        if (!name_copy) { if (pl) pl->failed = true; closedir(d); goto done; }
        ents[n].name = name_copy;
        ents[n].is_dir = is_dir;
        ents[n].is_reg = !is_dir;
        ents[n].link_to_dir = link_to_dir;
        n++;
    }
    closedir(d);

    if (n > 1) qsort(ents, (size_t)n, sizeof(*ents), walk_entry_cmp);

    for (int i = 0; i < n; i++) {
        if (stream && stream->failed) break;
        const char *name = walk_entry_name(&ents[i]);
        /* A symlink to a directory is not listed when it is not followed. */
        if (ents[i].link_to_dir && !ents[i].is_dir) {
            free(ents[i].name);
            ents[i].name = NULL;
            continue;
        }

        char *child = join_path(path, name);
        if (!child) {
            if (pl) pl->failed = true;
            free(ents[i].name);
            ents[i].name = NULL;
            continue;
        }

        if (ents[i].is_dir) {
            /* fzf's hidden and ignore options prune directories only;
               same-named files remain visible. */
            bool skip = (!opts->hidden && name[0] == '.') ||
                        walk_skip_dir(opts, child, name);
            if (!skip) {
                if (opts->dir) emit_dir(pl, out_fd, stream, child);
                walk_dir(child, opts, pl, out_fd, stream, root_abs, depth + 1);
            }
        } else if (opts->file) {
            emit_path(pl, out_fd, stream, child);
        }
        free(child);
        free(ents[i].name);
        ents[i].name = NULL;
    }

done:
    for (int i = 0; i < n; i++) {
        free(ents[i].name);
    }
    free(ents);
    if (stream && !stream->failed &&
        stream->len >= MM_WALK_STREAM_FLUSH_THRESHOLD)
        (void)stream_flush(stream);
}

static void root_absolute(const char *root, char *out, size_t cap) {
    char *rp = realpath(root, NULL);
    if (rp) {
        snprintf(out, cap, "%s", rp);
        free(rp);
    } else {
        snprintf(out, cap, "%s", root);
    }
}

bool mm_walk_many(const char *const *roots, int root_count,
                  const mm_walker_opts_t *opts,
                  char ***out_paths, int *out_n) {
    if (!roots || root_count < 1 || !opts || !out_paths || !out_n)
        return false;
    *out_paths = NULL;
    *out_n = 0;
    mm_pathlist_t pl = {0};
    for (int i = 0; i < root_count; i++) {
        if (!roots[i] || !*roots[i]) continue;
        char root_abs[4096];
        root_absolute(roots[i], root_abs, sizeof(root_abs));
        /* Each root is walked independently, matching reader.go's loop. */
        walk_dir(roots[i], opts, &pl, -1, NULL, root_abs, 0);
        if (pl.failed) {
            mm_walk_free(pl.items, pl.n);
            return false;
        }
    }
    *out_paths = pl.items;
    *out_n = pl.n;
    return true;
}

bool mm_walk(const char *root, const mm_walker_opts_t *opts,
             char ***out_paths, int *out_n) {
    const char *roots[] = { root };
    return mm_walk_many(roots, 1, opts, out_paths, out_n);
}

void mm_walk_stream_many(int fd, const char *const *roots, int root_count,
                         const mm_walker_opts_t *opts) {
    if (fd < 0) return;
    if (!roots || root_count < 1 || !opts) {
        close(fd);
        return;
    }
    char buffer[MM_WALK_STREAM_BUFFER_SIZE];
    mm_walk_stream_t stream = {
        .fd = fd,
        .buffer = buffer,
        .len = 0,
        .cap = sizeof(buffer),
        .failed = false
    };
    for (int i = 0; i < root_count && !stream.failed; i++) {
        if (!roots[i] || !*roots[i]) continue;
        char root_abs[4096];
        root_absolute(roots[i], root_abs, sizeof(root_abs));
        walk_dir(roots[i], opts, NULL, fd, &stream, root_abs, 0);
    }
    if (!stream.failed) (void)stream_flush(&stream);
    close(fd);
}

void mm_walk_stream(int fd, const char *root, const mm_walker_opts_t *opts) {
    const char *roots[] = { root };
    mm_walk_stream_many(fd, roots, 1, opts);
}

void mm_walk_free(char **paths, int n) {
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}
