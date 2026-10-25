/* walker.c - recursive directory traversal (fzf's default file listing). */
#define _POSIX_C_SOURCE 200809L
#include "walker.h"

#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Guard against symlink loops. */
#define MM_WALK_MAX_DEPTH 256

typedef struct {
    char *path;
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
    opts->file = true;
    opts->dir = false;
    opts->follow = true;
    opts->hidden = true;
    static const char *default_skip[] = { ".git", "node_modules" };
    opts->skip = default_skip;
    opts->nskip = 2;
}

bool mm_walker_parse(const char *spec, mm_walker_opts_t *opts) {
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
        int cap = pl->cap ? pl->cap * 2 : 64;
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
    const char *slash = strrchr(entry->path, '/');
    return slash ? slash + 1 : entry->path;
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
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, "..")) continue;
        if (n == cap) {
            int ncap = cap ? cap * 2 : 64;
            mm_walk_entry_t *ne = realloc(ents, (size_t)ncap * sizeof(*ne));
            if (!ne) { if (pl) pl->failed = true; closedir(d); goto done; }
            ents = ne;
            cap = ncap;
        }
        char *child = join_path(path, de->d_name);
        if (!child) { if (pl) pl->failed = true; closedir(d); goto done; }
        bool is_link = false, is_dir = false, link_to_dir = false;
        bool type_known = false;
#if defined(DT_UNKNOWN) && defined(DT_DIR) && defined(DT_LNK) && defined(DT_REG)
        /* On filesystems that populate d_type, avoid one lstat syscall per
           ordinary entry. DT_UNKNOWN and non-supporting platforms retain the
           portable lstat fallback below. */
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
                /* Known non-directory, non-symlink entries need no stat. */
                type_known = true;
                break;
        }
#endif
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
            if (stat(child, &st) == 0 && S_ISDIR(st.st_mode)) {
                link_to_dir = true;
                is_dir = opts->follow && follow_symlink_dir(child, root_abs);
            }
        }
        /* The basename is already stored at the end of child. Reuse it for
           sorting/filtering instead of allocating a second copy per entry. */
        ents[n].path = child;
        ents[n].is_dir = is_dir;
        ents[n].is_reg = !is_dir;
        ents[n].link_to_dir = link_to_dir;
        n++;
    }
    closedir(d);

    qsort(ents, (size_t)n, sizeof(*ents), walk_entry_cmp);

    for (int i = 0; i < n; i++) {
        if (stream && stream->failed) break;
        const char *name = walk_entry_name(&ents[i]);
        char *child = ents[i].path;
        /* fzf's "hidden" walker option only controls whether hidden
           directories are traversed; hidden files are always listed. */
        bool skip = !opts->hidden && ents[i].is_dir && name[0] == '.';
        for (int k = 0; !skip && opts->skip && k < opts->nskip; k++) {
            if (!strcmp(opts->skip[k], name)) skip = true;
        }
        /* A symlink to a directory is not listed when it is not followed,
           and a loop back to the walker root is discarded. */
        if (ents[i].link_to_dir && !ents[i].is_dir) skip = true;
        if (!skip) {
            if (ents[i].is_dir) {
                if (opts->dir) emit_dir(pl, out_fd, stream, child);
                walk_dir(child, opts, pl, out_fd, stream, root_abs, depth + 1);
            } else if (opts->file) {
                emit_path(pl, out_fd, stream, child);
            }
        }
        free(ents[i].path);
        ents[i].path = NULL;
    }

done:
    for (int i = 0; i < n; i++) {
        free(ents[i].path);
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

bool mm_walk(const char *root, const mm_walker_opts_t *opts,
             char ***out_paths, int *out_n) {
    mm_pathlist_t pl = {0};
    char root_abs[4096];
    root_absolute(root, root_abs, sizeof(root_abs));
    /* The root itself is not emitted; only its contents. */
    walk_dir(root, opts, &pl, -1, NULL, root_abs, 0);
    if (pl.failed) {
        mm_walk_free(pl.items, pl.n);
        return false;
    }
    *out_paths = pl.items;
    *out_n = pl.n;
    return true;
}

void mm_walk_stream(int fd, const char *root, const mm_walker_opts_t *opts) {
    char root_abs[4096];
    char buffer[MM_WALK_STREAM_BUFFER_SIZE];
    mm_walk_stream_t stream = {
        .fd = fd,
        .buffer = buffer,
        .len = 0,
        .cap = sizeof(buffer),
        .failed = false
    };
    root_absolute(root, root_abs, sizeof(root_abs));
    walk_dir(root, opts, NULL, fd, &stream, root_abs, 0);
    if (!stream.failed) (void)stream_flush(&stream);
    close(fd);
}

void mm_walk_free(char **paths, int n) {
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}
