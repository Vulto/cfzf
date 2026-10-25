/* util.c - see util.h */
#define _XOPEN_SOURCE 700
#include "util.h"

size_t mm_utf8_decode(const char *s, size_t rem, wchar_t *out) {
    if (rem == 0) { *out = 0; return 0; }
    unsigned char c0 = (unsigned char)s[0];
    if (c0 < 0x80) { *out = (wchar_t)c0; return 1; }
    size_t need;
    wchar_t cp;
    if ((c0 & 0xE0) == 0xC0) { need = 2; cp = c0 & 0x1F; }
    else if ((c0 & 0xF0) == 0xE0) { need = 3; cp = c0 & 0x0F; }
    else if ((c0 & 0xF8) == 0xF0) { need = 4; cp = c0 & 0x07; }
    else { *out = 0xFFFD; return 1; }
    if (rem < need) { *out = 0xFFFD; return 1; }
    for (size_t i = 1; i < need; i++) {
        unsigned char ci = (unsigned char)s[i];
        if ((ci & 0xC0) != 0x80) { *out = 0xFFFD; return 1; }
        cp = (cp << 6) | (ci & 0x3F);
    }
    /* Reject overlong/surrogate/out-of-range. */
    if ((need == 2 && cp < 0x80) || (need == 3 && cp < 0x800) ||
        (need == 4 && cp < 0x10000) || cp > 0x10FFFF ||
        (cp >= 0xD800 && cp <= 0xDFFF)) {
        *out = 0xFFFD;
        return 1;
    }
    *out = cp;
    return need;
}

int mm_utf8_count(const char *s) {
    int n = 0;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if ((*p & 0xC0) != 0x80) n++;
    return n;
}

bool mm_is_ascii(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (*p >= 0x80) return false;
    return true;
}

int mm_runewidth(wchar_t c) {
    if (c == 0) return 0;
    if (c < 32 || (c >= 0x7F && c < 0xA0)) return 1; /* control -> 1 for layout */
    int w = wcwidth(c);
    return w < 0 ? 1 : w;
}

int mchars_init(mchars_t *c, char *bytes) {
    c->bytes = bytes;
    c->runes = NULL;
    c->decoded = 0;
    c->borrowed = 0;
    /* One pass over the bytes: length, code point count and the ASCII flag. */
    int raw = 0, n = 0, ascii = 1;
    for (const unsigned char *p = (const unsigned char *)bytes; *p; p++) {
        raw++;
        if ((*p & 0xC0) != 0x80) n++;
        if (*p >= 0x80) ascii = 0;
    }
    c->raw_bytes = raw;
    c->ascii = ascii;
    c->length = ascii ? raw : n;
    return 1;
}

void mchars_free(mchars_t *c) {
    if (!c->borrowed) free(c->runes);
    c->runes = NULL;
    c->decoded = 0;
    c->borrowed = 0;
}

int mchars_decode_into(mchars_t *c, wchar_t *buf, int buflen) {
    if (c->decoded) return 1;
    if (buflen < c->length + 1) return 0;
    const char *p = c->bytes;
    size_t rem = (size_t)c->raw_bytes;
    int i = 0;
    if (c->ascii) {
        /* Pure ASCII: the code points are the bytes themselves. */
        for (; rem > 0; rem--, p++) buf[i++] = (wchar_t)(unsigned char)*p;
    } else {
        while (rem > 0) {
            wchar_t cp;
            size_t used = mm_utf8_decode(p, rem, &cp);
            if (used == 0) break;
            buf[i++] = cp;
            p += used;
            rem -= used;
        }
    }
    buf[i] = 0;
    c->runes = buf;
    c->decoded = 1;
    c->borrowed = 1;
    c->length = i;
    return 1;
}

int mchars_decode(mchars_t *c) {
    if (c->decoded) return 1;
    if (mm_is_ascii(c->bytes)) {
        /* ASCII: decode into runes lazily for uniform access if requested. */
    }
    int n = c->length;
    wchar_t *runes = malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!runes) return 0;
    const char *p = c->bytes;
    size_t rem = (size_t)c->raw_bytes;
    int i = 0;
    if (c->ascii) {
        for (; rem > 0; rem--, p++) runes[i++] = (wchar_t)(unsigned char)*p;
    } else {
        while (rem > 0) {
            wchar_t cp;
            size_t used = mm_utf8_decode(p, rem, &cp);
            runes[i++] = cp;
            p += used;
            rem -= used;
        }
    }
    runes[i] = 0;
    c->runes = runes;
    c->decoded = 1;
    return 1;
}

wchar_t mchars_at(mchars_t *c, int i) {
    if (i < 0 || i >= c->length) return 0;
    if (c->decoded) return c->runes[i];
    if (c->ascii) return (wchar_t)(unsigned char)c->bytes[i];
    if (!mchars_decode(c)) return 0;
    return c->runes[i];
}
