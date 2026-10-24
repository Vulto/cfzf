/* ansiprobe.c - verify mm_next_ansi_escape_sequence against fzf's Go
   nextAnsiEscapeSequence results (see tests/gen-ansi-probe.sh).
   Input: TSV lines "hexstring\tstart,end start,end ..." on stdin. */
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

int main(void) {
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    int total = 0, pass = 0;
    while ((n = getline(&line, &cap, stdin)) != -1) {
        if (n > 0 && line[n - 1] == '\n') line[--n] = 0;
        if (n == 0) continue;
        char *tab = strchr(line, '\t');
        if (!tab) continue;
        *tab = 0;
        const char *hex = line;
        const char *spec = tab + 1;

        size_t hlen = strlen(hex);
        char *bytes = malloc(hlen / 2 + 1);
        size_t blen = 0;
        for (size_t i = 0; i + 1 < hlen; i += 2) {
            int hi = hexval(hex[i]), lo = hexval(hex[i + 1]);
            if (hi < 0 || lo < 0) break;
            bytes[blen++] = (char)((hi << 4) | lo);
        }

        /* expected sequences */
        int exp[256][2], nexp = 0;
        const char *p = spec;
        while (*p) {
            int a, b;
            if (sscanf(p, "%d,%d", &a, &b) == 2) {
                exp[nexp][0] = a; exp[nexp][1] = b; nexp++;
            }
            while (*p && *p != ' ') p++;
            while (*p == ' ') p++;
        }

        /* compute with our parser: offsets are relative to remaining substring,
           so we must track absolute base to match Go's rem-relative indices. */
        int got[256][2], ngot = 0;
        size_t base = 0;
        while (base < blen) {
            int s, e;
            mm_next_ansi_escape_sequence(bytes + base, blen - base, &s, &e);
            if (s == -1) break;
            got[ngot][0] = s;
            got[ngot][1] = e;
            ngot++;
            base += (size_t)e;
        }

        total++;
        int ok = (ngot == nexp);
        if (ok) {
            for (int i = 0; i < ngot; i++)
                if (got[i][0] != exp[i][0] || got[i][1] != exp[i][1]) { ok = 0; break; }
        }
        if (ok) {
            pass++;
        } else {
            fprintf(stderr, "FAIL hex=%s\n  exp:", hex);
            for (int i = 0; i < nexp; i++) fprintf(stderr, " %d,%d", exp[i][0], exp[i][1]);
            fprintf(stderr, "\n  got:");
            for (int i = 0; i < ngot; i++) fprintf(stderr, " %d,%d", got[i][0], got[i][1]);
            fprintf(stderr, "\n");
        }
        free(bytes);
    }
    printf("%d/%d ansi sequences matched\n", pass, total);
    free(line);
    return pass == total ? 0 : 1;
}
