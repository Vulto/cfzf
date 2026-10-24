/* allocfail_hook.c - link this into a test build to make the Nth allocation
   fail (MM_ALLOC_FAIL_N).  Unlike the LD_PRELOAD shim it only affects the
   program under test, which makes it usable with gdb. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#ifndef MM_ALLOC_FAIL_N
#define MM_ALLOC_FAIL_N 1
#endif

/* The allocation to fail can be given at runtime through FAILALLOC_N, so
   that a single build can be used for a whole sweep. */
static long fail_target = 0;

static long counter = 0;
static void *(*real_malloc)(size_t);
static void *(*real_calloc)(size_t, size_t);
static void *(*real_realloc)(void *, size_t);
static char *(*real_strdup)(const char *);
static char *(*real_strndup)(const char *, size_t);

static void init(void) {
    real_malloc = dlsym(RTLD_NEXT, "malloc");
    real_calloc = dlsym(RTLD_NEXT, "calloc");
    real_realloc = dlsym(RTLD_NEXT, "realloc");
    real_strdup = dlsym(RTLD_NEXT, "strdup");
    real_strndup = dlsym(RTLD_NEXT, "strndup");
}

static int should_fail(void) {
    if (!real_malloc) init();
    if (fail_target == 0) {
        const char *n = getenv("FAILALLOC_N");
        fail_target = n ? atol(n) : MM_ALLOC_FAIL_N;
        if (fail_target <= 0) fail_target = MM_ALLOC_FAIL_N;
    }
    return ++counter == fail_target;
}

void *malloc(size_t size) {
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
