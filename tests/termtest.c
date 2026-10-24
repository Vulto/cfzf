/* termtest.c - exercise the terminal renderer under a controlling tty.
   Prints a box and some colored text, then exits. Used by tests/pty-smoke.sh. */
#define _POSIX_C_SOURCE 200809L
#include "terminal.h"
#include "tui.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>

int main(void) {
    int ttyout = open("/dev/tty", O_WRONLY);
    int ttyin = open("/dev/tty", O_RDONLY);
    if (ttyout < 0 || ttyin < 0) { fprintf(stderr, "no tty\n"); return 1; }

    mm_renderer_t r;
    if (!mm_renderer_init(&r, ttyin, ttyout, false, 8, true, false)) {
        fprintf(stderr, "init failed\n");
        return 1;
    }
    r.colored = true;
    mm_renderer_csi(&r, "J");
    mm_window_t w;
    mm_window_init(&w, &r, 0, 0, mm_renderer_max_x(&r), 3, MM_WIN_LIST,
                   MM_BORDER_NONE, false);
    mm_window_move(&w, 0, 0);
    mm_window_cprint(&w, MM_COL_RED, MM_COL_DEFAULT, MM_BOLD, "hello");
    mm_window_cprint(&w, MM_COL_GREEN, MM_COL_DEFAULT, 0, " world");
    mm_window_move(&w, 1, 0);
    mm_window_fill(&w, "line two\nline three");
    mm_window_finish_fill(&w);
    mm_renderer_flush(&r);
    mm_renderer_clear(&r);
    mm_renderer_close(&r);
    close(ttyin);
    close(ttyout);
    printf("TERMTEST_OK\n");
    return 0;
}
