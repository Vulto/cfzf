/* walker.c - recursive directory traversal (fzf's default file listing).
   The walk matches fzf's fastwalk + reader.go rules:
   - regular files, then other non-directories, then directories (per directory)
   - hidden files are always listed; hidden directories are skipped when hidden
     is off
   - directory symlinks are followed only when follow is on, and never when the
     target is "/", an ancestor of a walker root, or the same inode as any
     directory already on the path (fastwalk shouldTraverse)
   - several workers read directories at once. The set of paths is what the
     tests compare; discovery order across workers is not. */
#define _POSIX_C_SOURCE 200809L
#include "walker.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* Sibling symlink cycles are not ancestors of each other. fzf's shouldTraverse
   does not reject those either; the cap only stops a loop fzf would also
   walk forever. Real trees are far shallower. */
#define MM_WALK_MAX_DEPTH 256
#define MM_WALK_CHAIN_MAX (MM_WALK_MAX_DEPTH + 128)
#define MM_WALK_STREAM_BUFFER_SIZE (64u * 1024u)

typedef struct {
    dev_t dev;
    ino_t ino;
} mm_walk_id;

typedef struct {
    char *path;
    mm_walk_id *chain;
    int chain_len;
    int depth;
} mm_job;

typedef struct {
    uint32_t off;
    uint32_t len;
    uint8_t sort_cls; /* 0 regular, 1 other non-dir, 2 directory */
    uint8_t emit_file;
    uint8_t emit_dir;
    uint8_t descend;
} mm_ent;

typedef struct {
    const char **base;
    int nbase;
    const char **full;
    int nfull;
    const char **suffix;
    int nsuffix;
    char **owned;
    int nowned;
} mm_skip_set;

typedef struct {
    char **items;
    int n;
    int cap;
    bool failed;
} mm_pathlist_t;

typedef struct {
    int fd;
    char *buffer;
    size_t len;
    size_t cap;
    bool failed;
} mm_walk_stream_t;

typedef struct {
    char *buf;
    size_t len;
    size_t cap;
    char **items;
    int nitems;
    int cap_items;
} mm_walk_local;

typedef struct {
    const mm_walker_opts_t *opts;
    mm_skip_set skip;
    char root_abs[4096];
    mm_walk_id base[128];
    int base_len;

    mm_job *q;
    int head, tail, count, cap;
    int in_flight;
    bool stop;
    atomic_int failed;
    pthread_mutex_t mu;
    pthread_cond_t cv;

    mm_pathlist_t *pl;
    mm_walk_stream_t *stream;
    pthread_mutex_t out_mu;
} mm_walk_state;

static _Thread_local const char *tl_names;

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

static int walk_worker_count(void) {
    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 4) n = 4;
    if (n > 32) n = 32;
    return (int)n;
}

static bool id_in(mm_walk_id id, const mm_walk_id *chain, int n) {
    for (int i = 0; i < n; i++) {
        if (chain[i].dev == id.dev && chain[i].ino == id.ino) return true;
    }
    return false;
}

/* `ancestor` equals `path` or is a parent directory of it. A shared prefix
   without a path boundary (`/home` vs `/home2`) is not an ancestor. */
static bool path_is_ancestor(const char *ancestor, const char *path) {
    size_t n = strlen(ancestor);
    if (n == 0 || strncmp(ancestor, path, n) != 0) return false;
    if (path[n] == '\0') return true;
    if (n == 1 && ancestor[0] == '/') return true;
    return path[n] == '/';
}

static char *join_path(const char *path, const char *name) {
    if (path[0] == '.' && path[1] == '\0')
        return strdup(name);
    size_t plen = strlen(path);
    bool slash = plen > 0 && path[plen - 1] == '/';
    size_t nlen = strlen(name);
    if (plen > SIZE_MAX - nlen - 2) return NULL;
    char *out = malloc(plen + (slash ? 0 : 1) + nlen + 1);
    if (!out) return NULL;
    memcpy(out, path, plen);
    size_t o = plen;
    if (!slash) out[o++] = '/';
    memcpy(out + o, name, nlen);
    out[o + nlen] = 0;
    return out;
}

static void root_absolute(const char *root, char *out, size_t cap) {
    char *rp = realpath(root, NULL);
    if (rp) {
        snprintf(out, cap, "%s", rp);
        free(rp);
    } else if (cap) {
        snprintf(out, cap, "%s", root);
    }
}

static void build_base_chain(mm_walk_state *st, const char *root) {
    struct stat stt;
    st->base_len = 0;
    if (stat(root, &stt) != 0) return;
    st->base[st->base_len++] = (mm_walk_id){ stt.st_dev, stt.st_ino };
    char *rp = realpath(root, NULL);
    if (!rp) return;
    char *cur = rp;
    while (st->base_len < (int)(sizeof(st->base) / sizeof(st->base[0]))) {
        if (strcmp(cur, "/") == 0) break;
        char *slash = strrchr(cur, '/');
        if (!slash) break;
        if (slash == cur) {
            if (stat("/", &stt) == 0)
                st->base[st->base_len++] = (mm_walk_id){ stt.st_dev, stt.st_ino };
            break;
        }
        *slash = '\0';
        if (stat(cur, &stt) != 0) break;
        st->base[st->base_len++] = (mm_walk_id){ stt.st_dev, stt.st_ino };
    }
    free(rp);
}

static bool skip_prepare(mm_skip_set *sk, const mm_walker_opts_t *opts) {
    memset(sk, 0, sizeof(*sk));
    int n = opts->nskip;
    if (n <= 0 || !opts->skip) return true;
    sk->base = calloc((size_t)n, sizeof(*sk->base));
    sk->full = calloc((size_t)n, sizeof(*sk->full));
    sk->suffix = calloc((size_t)n * 2, sizeof(*sk->suffix));
    sk->owned = calloc((size_t)n, sizeof(*sk->owned));
    if (!sk->base || !sk->full || !sk->suffix || !sk->owned) return false;
    for (int i = 0; i < n; i++) {
        const char *s = opts->skip[i];
        if (!s || !*s) continue;
        if (strchr(s, '/')) {
            if (s[0] == '/') {
                sk->suffix[sk->nsuffix++] = s;
            } else {
                size_t len = strlen(s);
                char *suf = malloc(len + 2);
                if (!suf) return false;
                suf[0] = '/';
                memcpy(suf + 1, s, len + 1);
                sk->full[sk->nfull++] = s;
                sk->owned[sk->nowned++] = suf;
                sk->suffix[sk->nsuffix++] = suf;
            }
        } else {
            sk->base[sk->nbase++] = s;
        }
    }
    return true;
}

static void skip_free(mm_skip_set *sk) {
    for (int i = 0; i < sk->nowned; i++) free(sk->owned[i]);
    free(sk->base);
    free(sk->full);
    free(sk->suffix);
    free(sk->owned);
    memset(sk, 0, sizeof(*sk));
}

static bool skip_base(const mm_skip_set *sk, const char *name) {
    for (int i = 0; i < sk->nbase; i++) {
        if (strcmp(sk->base[i], name) == 0) return true;
    }
    return false;
}

static bool skip_path(const mm_skip_set *sk, const char *path) {
    for (int i = 0; i < sk->nfull; i++) {
        if (strcmp(sk->full[i], path) == 0) return true;
    }
    size_t n = strlen(path);
    for (int i = 0; i < sk->nsuffix; i++) {
        size_t m = strlen(sk->suffix[i]);
        if (m <= n && memcmp(path + (n - m), sk->suffix[i], m) == 0) return true;
    }
    return false;
}

static bool plist_push(mm_pathlist_t *pl, char *path) {
    if (pl->n == pl->cap) {
        if (pl->cap > INT_MAX / 2) { pl->failed = true; return false; }
        int cap = pl->cap ? pl->cap * 2 : 64;
        char **items = realloc(pl->items, (size_t)cap * sizeof(*items));
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

static bool stream_write_raw(mm_walk_stream_t *stream, const char *data, size_t len) {
    while (len > 0) {
        if (stream->len == stream->cap && !stream_flush(stream)) return false;
        size_t room = stream->cap - stream->len;
        size_t n = len < room ? len : room;
        memcpy(stream->buffer + stream->len, data, n);
        stream->len += n;
        data += n;
        len -= n;
    }
    return true;
}

static void mark_failed(mm_walk_state *st) {
    atomic_store(&st->failed, 1);
    pthread_mutex_lock(&st->mu);
    st->stop = true;
    pthread_cond_broadcast(&st->cv);
    pthread_mutex_unlock(&st->mu);
}

static bool queue_push(mm_walk_state *st, mm_job job) {
    if (st->count == st->cap) {
        int ncap = st->cap ? st->cap * 2 : 64;
        if (ncap < st->cap) return false;
        mm_job *nq = malloc((size_t)ncap * sizeof(*nq));
        if (!nq) return false;
        for (int i = 0; i < st->count; i++)
            nq[i] = st->q[(st->head + i) % (st->cap ? st->cap : 1)];
        free(st->q);
        st->q = nq;
        st->head = 0;
        st->tail = st->count;
        st->cap = ncap;
    }
    st->q[st->tail] = job;
    st->tail = (st->tail + 1) % st->cap;
    st->count++;
    return true;
}

static bool enqueue(mm_walk_state *st, char *path, mm_walk_id *chain,
                    int chain_len, int depth) {
    pthread_mutex_lock(&st->mu);
    if (atomic_load(&st->failed) || st->stop) {
        pthread_mutex_unlock(&st->mu);
        free(path);
        free(chain);
        return false;
    }
    mm_job job = { path, chain, chain_len, depth };
    if (!queue_push(st, job)) {
        atomic_store(&st->failed, 1);
        st->stop = true;
        pthread_cond_broadcast(&st->cv);
        pthread_mutex_unlock(&st->mu);
        free(path);
        free(chain);
        return false;
    }
    st->in_flight++;
    pthread_cond_signal(&st->cv);
    pthread_mutex_unlock(&st->mu);
    return true;
}

static bool pop_job(mm_walk_state *st, mm_job *job) {
    pthread_mutex_lock(&st->mu);
    while (st->count == 0 && !st->stop)
        pthread_cond_wait(&st->cv, &st->mu);
    if (st->count == 0) {
        pthread_mutex_unlock(&st->mu);
        return false;
    }
    *job = st->q[st->head];
    st->head = (st->head + 1) % st->cap;
    st->count--;
    pthread_mutex_unlock(&st->mu);
    return true;
}

static void finish_job(mm_walk_state *st) {
    pthread_mutex_lock(&st->mu);
    st->in_flight--;
    if (st->in_flight == 0) {
        st->stop = true;
        pthread_cond_broadcast(&st->cv);
    }
    pthread_mutex_unlock(&st->mu);
}

static bool local_flush(mm_walk_state *st, mm_walk_local *loc) {
    if (loc->len == 0 && loc->nitems == 0) return !atomic_load(&st->failed);
    bool ok = true;
    pthread_mutex_lock(&st->out_mu);
    if (st->stream && loc->len) {
        ok = stream_write_raw(st->stream, loc->buf, loc->len);
        if (!ok) st->stream->failed = true;
    }
    if (ok && st->pl && loc->nitems) {
        for (int i = 0; i < loc->nitems; i++) {
            if (!plist_push(st->pl, loc->items[i])) {
                ok = false;
                free(loc->items[i]);
                for (int j = i + 1; j < loc->nitems; j++) free(loc->items[j]);
                break;
            }
            loc->items[i] = NULL;
        }
    }
    pthread_mutex_unlock(&st->out_mu);
    loc->len = 0;
    loc->nitems = 0;
    if (!ok) mark_failed(st);
    return ok;
}

static bool local_add(mm_walk_state *st, mm_walk_local *loc,
                      const char *dir, const char *name, bool slash) {
    if (atomic_load(&st->failed)) return false;
    bool dot = dir[0] == '.' && dir[1] == '\0';
    size_t dlen = dot ? 0 : strlen(dir);
    bool sep = !dot && (dlen == 0 || dir[dlen - 1] != '/');
    size_t nlen = strlen(name);
    size_t body = dlen + (sep ? 1u : 0u) + nlen + (slash ? 1u : 0u);

    if (st->pl) {
        if (loc->nitems == loc->cap_items) {
            if (!local_flush(st, loc)) return false;
        }
        if (loc->nitems == loc->cap_items) {
            int ncap = loc->cap_items ? loc->cap_items * 2 : 64;
            char **ni = realloc(loc->items, (size_t)ncap * sizeof(*ni));
            if (!ni) { mark_failed(st); return false; }
            loc->items = ni;
            loc->cap_items = ncap;
        }
        char *s = malloc(body + 1);
        if (!s) { mark_failed(st); return false; }
        size_t o = 0;
        if (!dot) {
            memcpy(s + o, dir, dlen);
            o += dlen;
            if (sep) s[o++] = '/';
        }
        memcpy(s + o, name, nlen);
        o += nlen;
        if (slash) s[o++] = '/';
        s[o] = '\0';
        loc->items[loc->nitems++] = s;
        return true;
    }

    size_t total = body + 1;
    if (!loc->buf || loc->len + total > loc->cap) {
        if (!local_flush(st, loc)) return false;
    }
    if (!loc->buf || total > loc->cap) {
        char *s = malloc(total);
        if (!s) { mark_failed(st); return false; }
        size_t o = 0;
        if (!dot) {
            memcpy(s + o, dir, dlen);
            o += dlen;
            if (sep) s[o++] = '/';
        }
        memcpy(s + o, name, nlen);
        o += nlen;
        if (slash) s[o++] = '/';
        s[o++] = '\n';
        pthread_mutex_lock(&st->out_mu);
        bool ok = st->stream && stream_write_raw(st->stream, s, total);
        if (!ok && st->stream) st->stream->failed = true;
        pthread_mutex_unlock(&st->out_mu);
        free(s);
        if (!ok) mark_failed(st);
        return ok;
    }
    char *p = loc->buf + loc->len;
    size_t o = 0;
    if (!dot) {
        memcpy(p + o, dir, dlen);
        o += dlen;
        if (sep) p[o++] = '/';
    }
    memcpy(p + o, name, nlen);
    o += nlen;
    if (slash) p[o++] = '/';
    p[o++] = '\n';
    loc->len += o;
    return true;
}

static int ent_cmp(const void *a, const void *b) {
    const mm_ent *x = a;
    const mm_ent *y = b;
    if (x->sort_cls != y->sort_cls)
        return x->sort_cls < y->sort_cls ? -1 : 1;
    return strcmp(tl_names + x->off, tl_names + y->off);
}

static bool ent_push(mm_ent **ents, int *n, int *cap, char **arena, size_t *alen,
                     size_t *acap, const char *name, uint8_t cls, uint8_t file,
                     uint8_t dir, uint8_t descend) {
    if (*n == *cap) {
        if (*cap > INT_MAX / 2) return false;
        int ncap = *cap ? *cap * 2 : 64;
        mm_ent *ne = realloc(*ents, (size_t)ncap * sizeof(*ne));
        if (!ne) return false;
        *ents = ne;
        *cap = ncap;
    }
    size_t nlen = strlen(name);
    if (*alen > SIZE_MAX - nlen - 1) return false;
    if (*alen + nlen + 1 > *acap) {
        size_t ncap = *acap ? *acap * 2 : 4096;
        while (ncap < *alen + nlen + 1) {
            if (ncap > SIZE_MAX / 2) return false;
            ncap *= 2;
        }
        char *na = realloc(*arena, ncap);
        if (!na) return false;
        *arena = na;
        *acap = ncap;
    }
    mm_ent *e = &(*ents)[*n];
    e->off = (uint32_t)*alen;
    e->len = (uint32_t)nlen;
    e->sort_cls = cls;
    e->emit_file = file;
    e->emit_dir = dir;
    e->descend = descend;
    memcpy(*arena + *alen, name, nlen + 1);
    *alen += nlen + 1;
    (*n)++;
    return true;
}

/* fzf returns SkipDir for a symlink whose EvalSymlinks target is "/" or an
   ancestor of a walker root, so the link is neither listed nor descended.
   A link that only fails shouldTraverse (the target inode is already on the
   path) is still listed as a directory when --walker includes dir. */
static bool string_root_ancestor(const char *child, const char *root_abs) {
    char *target = realpath(child, NULL);
    if (!target) return false;
    bool anc = path_is_ancestor(target, root_abs);
    free(target);
    return anc;
}

static bool collect_entry(mm_walk_state *st, int dfd, const char *dirpath,
                          const char *name, unsigned char d_type, int depth,
                          const mm_walk_id *check, int check_len,
                          mm_ent **ents, int *n, int *cap,
                          char **arena, size_t *alen, size_t *acap) {
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0')))
        return true;

    bool is_link = false, is_dir = false, is_reg = false, known = false;
#if defined(DT_UNKNOWN) && defined(DT_DIR) && defined(DT_LNK) && defined(DT_REG)
    switch (d_type) {
    case DT_UNKNOWN: break;
    case DT_DIR: is_dir = true; known = true; break;
    case DT_LNK: is_link = true; known = true; break;
    case DT_REG: is_reg = true; known = true; break;
    default: known = true; break;
    }
#else
    (void)d_type;
#endif
    if (!known && dfd >= 0) {
        struct stat lst;
        if (fstatat(dfd, name, &lst, AT_SYMLINK_NOFOLLOW) == 0) {
            is_link = S_ISLNK(lst.st_mode);
            is_dir = S_ISDIR(lst.st_mode);
            is_reg = S_ISREG(lst.st_mode);
            known = true;
        }
    }
    if (!known) return true;

    const mm_walker_opts_t *opts = st->opts;
    if (is_link) {
        struct stat stt;
        bool dir_target = dfd >= 0 && fstatat(dfd, name, &stt, 0) == 0 &&
                          S_ISDIR(stt.st_mode);
        if (!dir_target) {
            if (!opts->file) return true;
            return ent_push(ents, n, cap, arena, alen, acap, name, 1, 1, 0, 0);
        }
        if (!opts->follow) return true;
        mm_walk_id id = { stt.st_dev, stt.st_ino };
        bool root_anc = id_in(id, check, st->base_len);
        bool cycle = id_in(id, check, check_len);
        if (root_anc) return true;
        if (!opts->hidden && name[0] == '.') return true;
        if (skip_base(&st->skip, name)) return true;
        bool need_path = st->skip.nfull || st->skip.nsuffix || !cycle;
        if (need_path) {
            char *child = join_path(dirpath, name);
            if (!child) return false;
            bool banned = skip_path(&st->skip, child) ||
                          (!cycle && string_root_ancestor(child, st->root_abs));
            free(child);
            if (banned) return true;
        }
        uint8_t descend = (!cycle && depth < MM_WALK_MAX_DEPTH) ? 1 : 0;
        return ent_push(ents, n, cap, arena, alen, acap, name, 2,
                        0, opts->dir ? 1 : 0, descend);
    }

    if (is_dir) {
        if (!opts->hidden && name[0] == '.') return true;
        if (skip_base(&st->skip, name)) return true;
        if (st->skip.nfull || st->skip.nsuffix) {
            char *child = join_path(dirpath, name);
            if (!child) return false;
            bool banned = skip_path(&st->skip, child);
            free(child);
            if (banned) return true;
        }
        uint8_t descend = depth < MM_WALK_MAX_DEPTH ? 1 : 0;
        return ent_push(ents, n, cap, arena, alen, acap, name, 2,
                        0, opts->dir ? 1 : 0, descend);
    }

    if (!opts->file) return true;
    return ent_push(ents, n, cap, arena, alen, acap, name,
                    is_reg ? 0 : 1, 1, 0, 0);
}

static void walk_one(mm_walk_state *st, const mm_job *job, mm_walk_local *loc) {
    if (atomic_load(&st->failed) || job->depth > MM_WALK_MAX_DEPTH) return;
    DIR *d = opendir(job->path);
    if (!d) return;
    int dfd = dirfd(d);

    mm_walk_id check[MM_WALK_CHAIN_MAX];
    int check_len = job->chain_len;
    if (check_len > MM_WALK_CHAIN_MAX) check_len = MM_WALK_CHAIN_MAX;
    if (job->chain && check_len > 0)
        memcpy(check, job->chain, (size_t)check_len * sizeof(*check));
    if (dfd >= 0) {
        struct stat stt;
        if (fstat(dfd, &stt) == 0) {
            mm_walk_id here = { stt.st_dev, stt.st_ino };
            if (!id_in(here, check, check_len) && check_len < MM_WALK_CHAIN_MAX)
                check[check_len++] = here;
        }
    }

    mm_ent *ents = NULL;
    char *arena = NULL;
    int n = 0, cap = 0;
    size_t alen = 0, acap = 0;
    bool failed = false;
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (atomic_load(&st->failed)) { failed = true; break; }
        unsigned char dt = 0;
#if defined(DT_UNKNOWN)
        dt = de->d_type;
#endif
        if (!collect_entry(st, dfd, job->path, de->d_name, dt, job->depth,
                           check, check_len, &ents, &n, &cap, &arena, &alen, &acap)) {
            failed = true;
            break;
        }
    }
    closedir(d);
    if (failed) {
        mark_failed(st);
        free(ents);
        free(arena);
        return;
    }
    if (n > 1) {
        tl_names = arena;
        qsort(ents, (size_t)n, sizeof(*ents), ent_cmp);
    }

    mm_walk_id *chain_copy = NULL;
    bool chain_ready = false;
    for (int i = 0; i < n; i++) {
        if (atomic_load(&st->failed)) break;
        const char *name = arena + ents[i].off;
        if (ents[i].emit_file) {
            if (!local_add(st, loc, job->path, name, false)) break;
        } else if (ents[i].emit_dir) {
            if (!local_add(st, loc, job->path, name, true)) break;
        }
        if (!ents[i].descend) continue;
        char *child = join_path(job->path, name);
        if (!child) { mark_failed(st); break; }
        if (!chain_ready) {
            if (check_len > 0) {
                chain_copy = malloc((size_t)check_len * sizeof(*chain_copy));
                if (!chain_copy) {
                    free(child);
                    mark_failed(st);
                    break;
                }
                memcpy(chain_copy, check, (size_t)check_len * sizeof(*check));
            }
            chain_ready = true;
        }
        mm_walk_id *chain = NULL;
        if (check_len > 0) {
            chain = malloc((size_t)check_len * sizeof(*chain));
            if (!chain) {
                free(child);
                mark_failed(st);
                break;
            }
            memcpy(chain, chain_copy, (size_t)check_len * sizeof(*chain));
        }
        if (!enqueue(st, child, chain, check_len, job->depth + 1)) {
            /* enqueue frees child and chain when it rejects them. */
            break;
        }
    }
    free(chain_copy);
    free(ents);
    free(arena);
}

static void *walk_worker(void *arg) {
    mm_walk_state *st = arg;
    sigset_t blocked;
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked, NULL);

    mm_walk_local loc = {0};
    loc.buf = malloc(MM_WALK_STREAM_BUFFER_SIZE);
    loc.cap = loc.buf ? MM_WALK_STREAM_BUFFER_SIZE : 0;
    for (;;) {
        mm_job job;
        if (!pop_job(st, &job)) break;
        if (!atomic_load(&st->failed)) walk_one(st, &job, &loc);
        free(job.path);
        free(job.chain);
        finish_job(st);
    }
    (void)local_flush(st, &loc);
    free(loc.buf);
    free(loc.items);
    return NULL;
}

static void queue_drain(mm_walk_state *st) {
    while (st->count > 0) {
        mm_job job = st->q[st->head];
        st->head = (st->head + 1) % st->cap;
        st->count--;
        free(job.path);
        free(job.chain);
    }
}

static void state_close(mm_walk_state *st);

static bool walk_execute(mm_walk_state *st, const char *root) {
    mm_walk_id *chain = NULL;
    if (st->base_len > 0) {
        chain = malloc((size_t)st->base_len * sizeof(*chain));
        if (!chain) { mark_failed(st); return false; }
        memcpy(chain, st->base, (size_t)st->base_len * sizeof(*chain));
    }
    char *root_copy = strdup(root);
    if (!root_copy || !enqueue(st, root_copy, chain, st->base_len, 0)) {
        if (!root_copy) {
            free(chain);
            mark_failed(st);
        }
        return false;
    }

    int nworkers = walk_worker_count();
    pthread_t *threads = calloc((size_t)nworkers, sizeof(*threads));
    int started = 0;
    if (threads) {
        for (int i = 0; i < nworkers; i++) {
            if (pthread_create(&threads[i], NULL, walk_worker, st) != 0) break;
            started++;
        }
    }
    if (started == 0) {
        walk_worker(st);
    } else {
        for (int i = 0; i < started; i++) pthread_join(threads[i], NULL);
    }
    free(threads);
    queue_drain(st);
    return !atomic_load(&st->failed) && !(st->pl && st->pl->failed) &&
           !(st->stream && st->stream->failed);
}

static bool state_open(mm_walk_state *st, const char *root, const mm_walker_opts_t *opts) {
    memset(st, 0, sizeof(*st));
    st->opts = opts;
    atomic_init(&st->failed, 0);
    if (pthread_mutex_init(&st->mu, NULL) != 0) return false;
    if (pthread_mutex_init(&st->out_mu, NULL) != 0) {
        pthread_mutex_destroy(&st->mu);
        return false;
    }
    if (pthread_cond_init(&st->cv, NULL) != 0) {
        pthread_mutex_destroy(&st->out_mu);
        pthread_mutex_destroy(&st->mu);
        return false;
    }
    if (!skip_prepare(&st->skip, opts)) {
        state_close(st);
        return false;
    }
    root_absolute(root, st->root_abs, sizeof(st->root_abs));
    build_base_chain(st, root);
    return true;
}

static void state_close(mm_walk_state *st) {
    queue_drain(st);
    free(st->q);
    skip_free(&st->skip);
    pthread_cond_destroy(&st->cv);
    pthread_mutex_destroy(&st->out_mu);
    pthread_mutex_destroy(&st->mu);
}

bool mm_walk(const char *root, const mm_walker_opts_t *opts,
             char ***out_paths, int *out_n) {
    if (!root || !*root || !opts || !out_paths || !out_n) return false;
    *out_paths = NULL;
    *out_n = 0;
    mm_pathlist_t pl = {0};
    mm_walk_state st;
    if (!state_open(&st, root, opts)) return false;
    st.pl = &pl;
    bool ok = walk_execute(&st, root);
    state_close(&st);
    if (!ok || pl.failed) {
        mm_walk_free(pl.items, pl.n);
        return false;
    }
    *out_paths = pl.items;
    *out_n = pl.n;
    return true;
}

void mm_walk_stream(int fd, const char *root, const mm_walker_opts_t *opts) {
    if (fd < 0) return;
    if (!root || !*root || !opts) {
        close(fd);
        return;
    }
    char buffer[MM_WALK_STREAM_BUFFER_SIZE];
    mm_walk_stream_t stream = {
        .fd = fd, .buffer = buffer, .len = 0,
        .cap = sizeof(buffer), .failed = false
    };
    mm_walk_state st;
    if (!state_open(&st, root, opts)) {
        close(fd);
        return;
    }
    st.stream = &stream;
    (void)walk_execute(&st, root);
    if (!stream.failed) (void)stream_flush(&stream);
    state_close(&st);
    close(fd);
}

void mm_walk_free(char **paths, int n) {
    for (int i = 0; i < n; i++) free(paths[i]);
    free(paths);
}
