/* ansicolor.c - differential test of mm_extract_color vs fzf extractColor.
   Input TSV (stdin): hex(in)\thex(out)\toffsets\tstate
   offsets: "b,e,fg,bg,ul,attr,lbg;..."  state: "fg,bg,ul,attr,lbg" */
#define _POSIX_C_SOURCE 200809L
#include "ansi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int hexval(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
static size_t unhex(const char *h, char **out) {
    size_t n = strlen(h), len = 0;
    char *b = malloc(n / 2 + 1);
    for (size_t i = 0; i + 1 < n; i += 2) {
        int hi = hexval(h[i]), lo = hexval(h[i + 1]);
        if (hi < 0 || lo < 0) break;
        b[len++] = (char)((hi << 4) | lo);
    }
    b[len] = 0;
    *out = b;
    return len;
}
static char *tohex(const char *b, size_t n) {
    static char *buf = NULL;
    static size_t cap = 0;
    if (n * 2 + 1 > cap) { cap = n * 2 + 64; buf = realloc(buf, cap); }
    static const char *H = "0123456789abcdef";
    for (size_t i = 0; i < n; i++) {
        buf[i * 2] = H[(unsigned char)b[i] >> 4];
        buf[i * 2 + 1] = H[(unsigned char)b[i] & 15];
    }
    buf[n * 2] = 0;
    return buf;
}

int main(void) {
    char *line = NULL; size_t cap = 0; ssize_t n;
    int total = 0, pass = 0;
    while ((n = getline(&line, &cap, stdin)) != -1) {
        if (n && line[n - 1] == '\n') line[--n] = 0;
        char *fields[4] = { "", "", "", "" };
        int nf = 0;
        char *q = line;
        while (nf < 4) {
            fields[nf++] = q;
            char *t = strchr(q, '\t');
            if (!t) break;
            *t = 0;
            q = t + 1;
        }
        char *hin = fields[0];
        char *hout = fields[1];
        char *offspec = fields[2];
        char *stspec = fields[3];
        if (!hin) continue;

        char *inb; size_t inlen = unhex(hin, &inb);
        char *outb; size_t outlen = unhex(hout, &outb);

        char *trimmed; size_t trimlen;
        mm_ansi_offsets_t offs;
        mm_ansi_state_t st;
        bool have_st;
        mm_extract_color(inb, inlen, NULL, NULL, NULL, &trimmed, &trimlen, &offs, &have_st, &st);

        total++;
        int ok = 1;
        char got[4096]; got[0] = 0;
        if (trimlen != outlen || memcmp(trimmed, outb, outlen) != 0) ok = 0;
        if (ok) {
            /* compare offsets */
            size_t g = 0;
            for (int i = 0; i < offs.len; i++) {
                g += (size_t)snprintf(got + g, sizeof(got) - g, "%s%d,%d,%d,%d,%d,%d,%d",
                    i ? ";" : "", offs.ptr[i].offset[0], offs.ptr[i].offset[1],
                    offs.ptr[i].color.fg, offs.ptr[i].color.bg, offs.ptr[i].color.ul,
                    offs.ptr[i].color.attr, offs.ptr[i].color.lbg);
            }
            if (strcmp(got, offspec) != 0) ok = 0;
            /* compare state */
            char gs[128];
            if (!have_st) {
                if (strcmp(stspec, "-2,-2,-2,-2,-2") != 0) ok = 0;
            } else {
                snprintf(gs, sizeof(gs), "%d,%d,%d,%d,%d", st.fg, st.bg, st.ul, st.attr, st.lbg);
                if (strcmp(gs, stspec) != 0) ok = 0;
            }
        }
        if (ok) pass++;
        else {
            fprintf(stderr, "FAIL in=%s\n  expout=%s gotout=%s\n  expoff=%s goto=%s\n  expst=%s have=%d\n",
                    hin, hout, tohex(trimmed, trimlen), offspec, got, stspec, have_st);
        }
        free(inb); free(outb); free(trimmed);
        mm_ansi_offsets_free(&offs);
    }
    printf("%d/%d ansi colors matched\n", pass, total);
    free(line);
    return pass == total ? 0 : 1;
}
