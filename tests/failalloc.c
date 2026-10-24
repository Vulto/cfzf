/* failalloc.c - an LD_PRELOAD shim that fails the Nth allocation.
 *
 * Build:
 *   cc -std=c23 -O1 -g -fPIC -shared -o failalloc.so failalloc.c -ldl
 * Use:
 *   FAILALLOC_N=42 LD_PRELOAD=./failalloc.so ./fzf --filter=x < input
 *
 * FAILALLOC_N selects the allocation to fail (1-based).  FAILALLOC_LOOP
 * makes every allocation from that point on fail.  Without FAILALLOC_N the
 * shim is inert.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static char *(*real_strdup)(const char *);
static char *(*real_strndup)(const char *, size_t);

static long counter = 0;
static long fail_at = -1;
static int  in_init = 0;
static int  loop = 0;

static void init(void) {
    if (in_init) return;
    in_init = 1;
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_strdup = dlsym(RTLD_NEXT, "strdup");
    real_strndup = dlsym(RTLD_NEXT, "strndup");
    const char *n = getenv("FAILALLOC_N");
    fail_at = n ? atol(n) : -1;
    const char *l = getenv("FAILALLOC_LOOP");
    loop = l ? atoi(l) : 0;
    if (fail_at <= 0 && !loop) in_init = 0;  /* stay inert */
}

static int should_fail(void) {
    if (!real_malloc) init();
    if (fail_at <= 0 && !loop) return 0;
    counter++;
    if (loop) return counter >= fail_at;
    return counter == fail_at;
}

void *malloc(size_t size) {
    if (!real_malloc) init();
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return real_malloc(size);
}

void *calloc(size_t n, size_t size) {
    if (!real_calloc) init();
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return real_calloc(n, size);
}

void *realloc(void *p, size_t size) {
    if (!real_realloc) init();
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return real_realloc(p, size);
}

char *strdup(const char *s) {
    if (!real_strdup) init();
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return real_strdup(s);
}

char *strndup(const char *s, size_t n) {
    if (!real_strndup) init();
    if (should_fail()) { errno = ENOMEM; return NULL; }
    return real_strndup(s, n);
}
