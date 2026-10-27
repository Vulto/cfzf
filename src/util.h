/* util.h - shared primitives: dynamic arrays, rune/char handling, widths. */
#ifndef MMENU_UTIL_H
#define MMENU_UTIL_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

/* ---- small helpers ---- */

static inline int mmin(int a, int b) { return a < b ? a : b; }
static inline int mmax(int a, int b) { return a > b ? a : b; }
static inline int mmax3(int a, int b, int c) { int m = a > b ? a : b; return m > c ? m : c; }

/* Growable typed array. Element storage is malloc'd; use ARRAY_FREE to release. */
#define ARRAY_DECL(type, name) \
    struct { type *items; int count; int cap; } name

static inline int mm_next_capacity(int current, int initial, size_t element_size) {
    int next;
    if (current < initial) next = initial;
    else next = current < (1 << 14) ? current * 2 : current + (current >> 2);
    if (next <= current) return 0;
    if ((size_t)next > SIZE_MAX / (element_size ? element_size : 1)) return 0;
    return next;
}

/* ---- Chars: a string that may be stored as bytes or as decoded runes ---- */

typedef struct {
    char *bytes;      /* NUL-terminated UTF-8 when not decoded */
    wchar_t *runes;   /* decoded code points, when decoded != 0 */
    int length;       /* number of runes (or bytes when ascii) */
    int decoded;      /* 1 when runes holds the decoded form */
    int raw_bytes;    /* byte length when not decoded */
    int borrowed;     /* 1 when runes points at a caller-owned buffer */
    int ascii;        /* 1 when the bytes are pure ASCII */
} mchars_t;

/* Initialize an mchars_t from a NUL-terminated UTF-8 string (borrowed bytes,
   lazily decoded). Returns 0 on allocation failure. */
int mchars_init(mchars_t *c, char *bytes);
/* Free any decoded storage (borrowed bytes are never freed). */
void mchars_free(mchars_t *c);
/* Decode bytes into runes on demand. Returns 0 on allocation failure. */
int mchars_decode(mchars_t *c);
/* Decode the bytes into the caller-provided buffer (which must hold at
   least c->length + 1 runes) instead of allocating one.  The buffer stays
   owned by the caller; mchars_free() will not release it.  Returns 0 on
   failure. */
int mchars_decode_into(mchars_t *c, wchar_t *buf, int buflen);
/* Code point at index i (decodes on demand). */
wchar_t mchars_at(mchars_t *c, int i);

/* ---- UTF-8 helpers ---- */

/* Decode one UTF-8 code point at s (length rem). Returns bytes consumed,
   sets *out. Invalid input yields bytes consumed 1 with U+FFFD. */
size_t mm_utf8_decode(const char *s, size_t rem, wchar_t *out);
/* Number of code points in a NUL-terminated UTF-8 string. */
int mm_utf8_count(const char *s);
/* True if the whole NUL-terminated string is ASCII. */
bool mm_is_ascii(const char *s);

/* Terminal display width of a code point (0 for combining, 1 or 2 otherwise). */
int mm_runewidth(wchar_t c);

/* Quote an argument for a POSIX shell command. Caller owns the result. */
char *mm_shell_quote(const char *s);

#endif /* MMENU_UTIL_H */
