/* main.c - fzf C23 rewrite entry point. Currently implements the batch
   --filter front-end; the interactive TUI follows in src/terminal.c.
   This is the first slice of the C23 fzf core; interactive TUI follows. */
#include "algo.h"
#include "pattern.h"
#include "result.h"
#include "filter.h"
#include "ui.h"
#include "walker.h"
#include "preview.h"
#include "history.h"
#include "color.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <limits.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>

static void usage(FILE *out) {
    fprintf(out, "usage: fzf [OPTIONS]\n");
}

/* Print the manual page.  The page is looked up next to the executable and
   in the usual man directories; when it cannot be found a short summary is
   printed instead. */
static int print_man_page(void) {
    const char *candidates[] = {
        "fzf.1",
        "/usr/local/share/man/man1/fzf.1",
        "/usr/share/man/man1/fzf.1",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        FILE *f = fopen(candidates[i], "r");
        if (!f) continue;
        int c;
        while ((c = fgetc(f)) != EOF) fputc(c, stdout);
        fclose(f);
        return 0;
    }
    fprintf(stderr,
            "fzf: manual page not found; see fzf(1) in the documentation\n");
    return 1;
}

static char *read_all_stdin(size_t *outlen) {
    size_t cap = 4 * 1024 * 1024, len = 0;
    char *buf = malloc(cap);
    if (!buf) { *outlen = 0; return NULL; }
    for (;;) {
        if (len + 262144 > cap) {
            char *nb = realloc(buf, cap * 2);
            if (!nb) { free(buf); *outlen = 0; return NULL; }
            buf = nb; cap *= 2;
        }
        size_t r = fread(buf + len, 1, 262144, stdin);
        len += r;
        if (r < 262144) break;
    }
    buf[len] = 0;
    *outlen = len;
    return buf;
}

typedef struct {
    int fd;
    char *root;
    mm_walker_opts_t opts;
} mm_walk_thread_arg_t;

static void *walk_thread_fn(void *arg) {
    mm_walk_thread_arg_t *a = arg;
    mm_walk_stream(a->fd, a->root, &a->opts);
    free(a->root);
    free(a);
    return NULL;
}

static bool mm_parse_label_pos(const char *s, int *pos, bool *bottom) {
    char *end = NULL;
    long v = strtol(s, &end, 10);
    if (end == s) return false;
    if (*end != 0) {
        if (*end != ':') return false;
        if (!strcmp(end + 1, "bottom")) *bottom = true;
        else if (!strcmp(end + 1, "top")) *bottom = false;
        else return false;
    }
    if (v < INT_MIN || v > INT_MAX) return false;
    *pos = (int)v;
    return true;
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (!mm_algo_init("default")) { fprintf(stderr, "algo init failed\n"); return 2; }

    const char *filter = NULL;
    const char *query = NULL;
    const char *scheme = "default";
    const char *tiebreak = NULL;
    bool exact = false;
    bool extended = false; /* fzf's --extended; default on for term syntax? see below */
    bool no_sort = false;
    bool tac = false;
    bool print_query = false;
    int tail = -1;
    mm_case_t case_mode = CASE_SMART;
    const char *nth_spec = NULL;
    const char *with_nth_spec = NULL;
    const char *delim_str = NULL;
    bool read0 = false;
    bool print0 = false;
    bool multi = false;
    bool cycle = false;
    bool reverse = false;
    bool info = true;   /* fzf shows the info line by default */
    bool info_inline_right = false; /* --info=inline-right */
    const char *style_spec = NULL;
    bool style_full = false;
    bool select_1 = false;
    bool exit_0 = false;
    bool expect = false;
    const char *expect_spec = NULL;
    const char *bind_spec = NULL;
    const char *color_spec = NULL;
    int border_shape = MM_BORDER_NONE;
    int preview_border_shape = MM_BORDER_ROUNDED;
    int list_border_shape = -1;
    bool list_border_set = false;
    int header_border_shape = -1;
    bool header_border_set = false;
    const char *border_label = NULL;
    int border_label_pos = 0;
    bool border_label_bottom = false;
    const char *preview_cmd = NULL;
    const char *preview_label = NULL;
    const char *preview_wrap_sign = NULL;
    int preview_label_pos = 0;
    bool preview_label_bottom = false;
    const char *pointer_spec = NULL;
    const char *marker_spec = NULL;
    const char *shell_spec = NULL;
    const char *history_spec = NULL;
    const char *ghost_spec = NULL;
    const char *height_spec = NULL;
    const char *ellipsis_spec = NULL;
    bool no_bold = false;
    int  gap = 0;
    const char *gap_line = NULL;
    bool phony = false;
    bool wrap = false;
    bool wrap_word = false;
    const char *wrap_sign = NULL;
    bool no_color = false;
    bool keep_right = false;
    bool no_mouse = false;
    bool sync = false;
    int  min_height = 0;
    const char *layout_spec = NULL;
    int history_size = MM_HISTORY_MAX;
    const char *preview_win_spec = NULL;
    mm_walker_opts_t walker;
    mm_walker_default(&walker);
    const char *walker_spec = NULL;
    const char *walker_root = ".";
    const char *walker_skip = NULL;
    int  multi_max = 0;
    int  padding = 0;
    int  margin = 0;
    int  scroll_off = 3;    /* fzf's default (src/options.go) */
    int  hscroll_off = 10;  /* fzf's default (src/options.go) */
    int  accept_nth = 0;
    const char *separator_spec = NULL;
    bool no_scrollbar = false;
    bool no_clear = false;
    int  tabstop = 8;
    bool no_input = false;
    bool no_hscroll = false;
    bool no_multi_line = false;
    bool no_tty_default = false;
    bool no_unicode = false;
    bool raw = false;
    const char *prompt = "> ";
    bool highlight_line = false;
    const char *info_command = NULL;
    const char *jump_labels = NULL;
    const char *marker_multi_line = NULL;
    int  threads = 0;
    bool track = false;
    bool ambidouble = false;
    bool filepath_word = false;
    bool header_first = false;
    const char *listen_spec = NULL;
    const char *listen_unsafe_spec = NULL;
    const char *header = NULL;
    const char *header_lines_text = NULL;
    int header_lines = 0;
    const char *footer_spec = NULL;
    const char *gutter_spec = NULL;
    const char *gutter_raw_spec = NULL;
    bool ansi = false;
    bool literal = false;

    /* fzf defaults: extended ON, fuzzy ON */
    extended = true;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        #define ARG_EQ(name) (!strncmp(a, name "=", sizeof(name)))
        if (!strcmp(a, "--")) { /* end of options */ }
        if (ARG_EQ("--filter")) { filter = a + sizeof("--filter=") - 1; }
        else if (!strcmp(a, "-f") || !strcmp(a, "--filter")) { if (i+1<argc) filter = argv[++i]; }
        else if (!strcmp(a, "--query") || !strcmp(a, "-q")) { if (i+1<argc) query = argv[++i]; }
        else if (ARG_EQ("--query")) { query = a + 8; }
        else if (!strcmp(a, "--exact") || !strcmp(a, "-e")) exact = true;
        else if (!strcmp(a, "--extended") || !strcmp(a, "-x")) extended = true;
        else if (!strcmp(a, "--no-extended") || !strcmp(a, "+x")) extended = false;
        else if (!strcmp(a, "--no-sort") || !strcmp(a, "+s")) no_sort = true;
        else if (!strcmp(a, "--sort")) no_sort = false;
        else if (!strcmp(a, "--tac")) tac = true;
        else if (!strcmp(a, "--no-tac")) tac = false;
        else if (!strcmp(a, "--print-query")) print_query = true;
        else if (!strcmp(a, "--ignore-case") || !strcmp(a, "-i")) case_mode = CASE_IGNORE;
        else if (!strcmp(a, "+i") || !strcmp(a, "--no-ignore-case")) case_mode = CASE_RESPECT;
        else if (!strcmp(a, "--smart-case")) case_mode = CASE_SMART;
        else if (ARG_EQ("--scheme")) { scheme = a + 9; }
        else if (!strcmp(a, "--scheme")) { if (i+1<argc) scheme = argv[++i]; }
        else if (ARG_EQ("--tiebreak")) { tiebreak = a + 11; }
        else if (!strcmp(a, "--tiebreak")) { if (i+1<argc) tiebreak = argv[++i]; }
        else if (ARG_EQ("--tail")) { tail = atoi(a + 7); }
        else if (!strcmp(a, "--tail")) { if (i+1<argc) tail = atoi(argv[++i]); }
        else if (ARG_EQ("--nth")) { nth_spec = a + 6; }
        else if (!strcmp(a, "--nth")) { if (i+1<argc) nth_spec = argv[++i]; }
        else if (ARG_EQ("--with-nth")) { with_nth_spec = a + 11; }
        else if (!strcmp(a, "--with-nth")) { if (i+1<argc) with_nth_spec = argv[++i]; }
        else if (ARG_EQ("--delimiter")) { delim_str = a + 12; }
        else if (!strcmp(a, "--delimiter")) { if (i+1<argc) delim_str = argv[++i]; }
        else if (!strcmp(a, "-d")) { if (i+1<argc) delim_str = argv[++i]; }
        else if (!strncmp(a, "-d", 2) && a[2] != 0 && a[1] != '-') { delim_str = a + 2; }
        else if (!strcmp(a, "--literal")) literal = true;
        else if (!strcmp(a, "--no-literal")) literal = false;
        else if (!strcmp(a, "--read0") || !strcmp(a, "-0")) read0 = true;
        else if (!strcmp(a, "--no-read0") || !strcmp(a, "+0")) read0 = false;
        else if (!strcmp(a, "--print0")) print0 = true;
        else if (!strcmp(a, "--sync")) { /* batch is inherently sync */ }
        else if (!strcmp(a, "--multi") || !strcmp(a, "-m")) multi = true;
        else if (!strcmp(a, "--no-multi")) multi = false;
        else if (ARG_EQ("--multi-max")) multi_max = atoi(a + 12);
        else if (!strcmp(a, "--multi-max")) { if (i+1<argc) multi_max = atoi(argv[++i]); }
        else if (!strcmp(a, "--cycle")) cycle = true;
        else if (!strcmp(a, "--no-cycle")) cycle = false;
        else if (!strcmp(a, "--reverse")) reverse = true;
        else if (!strcmp(a, "--no-reverse")) reverse = false;
        else if (!strcmp(a, "--info")) { info = true; info_inline_right = false; }
        else if (!strcmp(a, "--no-info")) { info = false; info_inline_right = false; }
        else if (!strncmp(a, "--info=", 7)) {
            const char *v = a + 7;
            if (!strcmp(v, "default")) { info = true; info_inline_right = false; }
            else if (!strcmp(v, "inline-right")) { info = true; info_inline_right = true; }
            else if (!strcmp(v, "hidden")) { info = false; info_inline_right = false; }
            else {
                fprintf(stderr, "unsupported info style: %s\n", v);
                return 2;
            }
        }
        else if (!strcmp(a, "--select-1") || !strcmp(a, "-1")) select_1 = true;
        else if (!strcmp(a, "--exit-0")) exit_0 = true;
        else if (!strcmp(a, "--expect")) expect = true;
        else if (!strcmp(a, "--no-expect")) expect = false;
        else if (ARG_EQ("--walker")) walker_spec = a + 9;
        else if (!strcmp(a, "--walker")) { if (i+1<argc) walker_spec = argv[++i]; }
        else if (ARG_EQ("--walker-root")) walker_root = a + 14;
        else if (!strcmp(a, "--walker-root")) { if (i+1<argc) walker_root = argv[++i]; }
        else if (ARG_EQ("--walker-skip")) walker_skip = a + 14;
        else if (!strcmp(a, "--walker-skip")) { if (i+1<argc) walker_skip = argv[++i]; }
        else if (!strcmp(a, "--no-bold")) no_bold = true;
        else if (ARG_EQ("--gap")) gap = atoi(a + 6);
        else if (!strcmp(a, "--gap")) { if (i+1<argc) gap = atoi(argv[++i]); }
        else if (ARG_EQ("--gap-line")) gap_line = a + 11;
        else if (!strcmp(a, "--gap-line")) { if (i+1<argc) gap_line = argv[++i]; }
        else if (!strcmp(a, "--phony")) phony = true;
        else if (!strcmp(a, "--no-phony")) phony = false;
        else if (ARG_EQ("--footer")) footer_spec = a + 9;
        else if (ARG_EQ("--padding")) padding = atoi(a + 10);
        else if (ARG_EQ("--margin")) margin = atoi(a + 9);
        else if (!strcmp(a, "--margin")) { if (i+1<argc) margin = atoi(argv[++i]); }
        else if (!strcmp(a, "--padding")) { if (i+1<argc) padding = atoi(argv[++i]); }
        else if (ARG_EQ("--gutter")) gutter_spec = a + 9;
        else if (!strcmp(a, "--gutter")) { if (i+1<argc) gutter_spec = argv[++i]; }
        else if (ARG_EQ("--gutter-raw")) gutter_raw_spec = a + 13;
        else if (!strcmp(a, "--gutter-raw")) { if (i+1<argc) gutter_raw_spec = argv[++i]; }
        else if (!strcmp(a, "--footer")) { if (i+1<argc) footer_spec = argv[++i]; }
        else if (!strcmp(a, "--no-footer")) footer_spec = NULL;
        /* --disabled keeps the query but does not search with it. */
        else if (!strcmp(a, "--disabled")) phony = true;
        else if (!strcmp(a, "--no-disabled")) phony = false;
        else if (!strcmp(a, "--wrap")) wrap = true;
        else if (!strcmp(a, "--no-wrap")) wrap = false;
        else if (!strcmp(a, "--wrap-word")) { wrap = true; wrap_word = true; }
        else if (!strcmp(a, "--no-wrap-word")) { wrap_word = false; }
        else if (ARG_EQ("--wrap")) {
            /* --wrap=MODE, the mode is "char" or "word". */
            const char *mode = a + 7;
            if (*mode) {
                if (!strcmp(mode, "char")) wrap_word = false;
                else if (!strcmp(mode, "word")) wrap_word = true;
                else {
                    fprintf(stderr, "invalid wrap mode: %s (expected: char or word)\n", mode);
                    return 2;
                }
            }
            wrap = true;
        }
        else if (ARG_EQ("--wrap-sign")) wrap_sign = a + 12;
        else if (!strcmp(a, "--wrap-sign")) { if (i+1<argc) wrap_sign = argv[++i]; }
        else if (!strcmp(a, "--keep-right")) keep_right = true;
        else if (!strcmp(a, "--no-mouse")) no_mouse = true;
        else if (!strcmp(a, "--sync")) sync = true;
        else if (ARG_EQ("--min-height")) {
            const char *v = a + 13;
            if (*v == '\0') { fprintf(stderr, "minimum height required: HEIGHT[+]\n"); return 2; }
            min_height = atoi(v);
        }
        else if (!strcmp(a, "--min-height")) {
            if (i+1 >= argc) { fprintf(stderr, "minimum height required: HEIGHT[+]\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0') { fprintf(stderr, "minimum height required: HEIGHT[+]\n"); return 2; }
            min_height = atoi(v);
        }
        else if (!strcmp(a, "--no-color")) no_color = true;
        else if (ARG_EQ("--height")) {
            const char *v = a + 9;
            if (*v == '\0') { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            height_spec = v;
        }
        else if (ARG_EQ("--ellipsis")) ellipsis_spec = a + 11;
        else if (!strcmp(a, "--ellipsis")) { if (i+1<argc) ellipsis_spec = argv[++i]; }
        else if (ARG_EQ("--layout")) layout_spec = a + 9;
        else if (!strcmp(a, "--layout")) { if (i+1<argc) layout_spec = argv[++i]; }
        else if (!strcmp(a, "--height")) {
            if (i+1 >= argc) { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0' || *v == '-') { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            height_spec = v;
        }
        else if (ARG_EQ("--scroll-off")) scroll_off = atoi(a + 13);
        else if (!strcmp(a, "--scroll-off")) { if (i+1<argc) scroll_off = atoi(argv[++i]); }
        else if (ARG_EQ("--hscroll-off")) hscroll_off = atoi(a + 14);
        else if (!strcmp(a, "--hscroll-off")) { if (i+1<argc) hscroll_off = atoi(argv[++i]); }
        else if (ARG_EQ("--separator")) separator_spec = a + 12;
        else if (!strcmp(a, "--separator")) { if (i+1<argc) separator_spec = argv[++i]; }
        else if (ARG_EQ("--accept-nth")) accept_nth = atoi(a + 13);
        else if (!strcmp(a, "--accept-nth")) { if (i+1<argc) accept_nth = atoi(argv[++i]); }
        else if (!strcmp(a, "--no-scrollbar")) no_scrollbar = true;
        else if (!strcmp(a, "--scrollbar")) no_scrollbar = false;
        else if (!strcmp(a, "--no-clear")) no_clear = true;
        else if (!strcmp(a, "--no-separator")) { /* separator defaults to empty */ }
        else if (ARG_EQ("--tabstop")) tabstop = atoi(a + 10);
        else if (!strcmp(a, "--tabstop")) { if (i+1<argc) tabstop = atoi(argv[++i]); }
        else if (!strcmp(a, "--no-input")) no_input = true;
        else if (!strcmp(a, "--no-hscroll")) no_hscroll = true;
        else if (!strcmp(a, "--no-multi-line")) no_multi_line = true;
        else if (!strcmp(a, "--no-tty-default")) no_tty_default = true;
        else if (!strcmp(a, "--no-unicode")) no_unicode = true;
        else if (!strcmp(a, "--unicode")) no_unicode = false;
        else if (!strcmp(a, "--raw")) { raw = true; exact = true; }
        else if (!strcmp(a, "--no-raw")) { raw = false; exact = false; }
        else if (ARG_EQ("--ghost")) ghost_spec = a + 8;
        else if (!strcmp(a, "--ghost")) { if (i+1<argc) ghost_spec = argv[++i]; }
        else if (!strcmp(a, "--highlight-line")) highlight_line = true;
        else if (!strcmp(a, "--no-highlight-line")) highlight_line = false;
        else if (ARG_EQ("--info-command")) info_command = a + 15;
        else if (!strcmp(a, "--info-command")) { if (i+1<argc) info_command = argv[++i]; }
        else if (!strcmp(a, "--no-info-command")) info_command = NULL;
        else if (ARG_EQ("--jump-labels")) { const char *v = a + 14; if (*v == '\0') { fprintf(stderr, "label characters required\n"); return 2; } jump_labels = v; }
        else if (!strcmp(a, "--jump-labels")) { if (i+1<argc) { const char *v = argv[++i]; if (*v == '\0') { fprintf(stderr, "label characters required\n"); return 2; } jump_labels = v; } else { fprintf(stderr, "label characters required\n"); return 2; } }
        else if (ARG_EQ("--marker-multi-line")) marker_multi_line = a + 20;
        else if (!strcmp(a, "--marker-multi-line")) { if (i+1<argc) marker_multi_line = argv[++i]; }
        else if (!strcmp(a, "--no-marker-multi-line")) marker_multi_line = NULL;
        else if (ARG_EQ("--threads")) { threads = atoi(a + 10); if (threads < 0) { fprintf(stderr, "--threads must be a positive integer\n"); return 2; } }
        else if (!strcmp(a, "--threads")) { if (i+1<argc) { threads = atoi(argv[++i]); if (threads < 0) { fprintf(stderr, "--threads must be a positive integer\n"); return 2; } } }
        else if (!strcmp(a, "--track")) track = true;
        else if (!strcmp(a, "--no-track")) track = false;
        else if (!strcmp(a, "--disabled")) phony = true;
        else if (!strcmp(a, "--no-disabled")) phony = false;
        else if (!strcmp(a, "--phony")) phony = true;
        else if (!strcmp(a, "--no-phony")) phony = false;
        else if (!strcmp(a, "--no-track")) track = false;
        else if (!strcmp(a, "--ambidouble")) ambidouble = true;
        else if (!strcmp(a, "--no-ambidouble")) ambidouble = false;
        else if (!strcmp(a, "--bash")) { /* print bash integration */ return 0; }
        else if (!strcmp(a, "--black")) { /* part of the --color palette */ }
        else if (!strcmp(a, "--filepath-word")) filepath_word = true;
        else if (!strcmp(a, "--no-filepath-word")) filepath_word = false;
        else if (!strcmp(a, "--fish")) { /* print fish integration */ return 0; }
        else if (!strcmp(a, "--header-first")) header_first = true;
        else if (!strcmp(a, "--no-header-first")) header_first = false;
        else if (ARG_EQ("--list-label")) { if (i+1<argc && a[strlen(a)]=='=' && !strchr(a+11,'=')) {} }
        else if (!strcmp(a, "--list-label")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--list-label-pos")) { if (i+1<argc) i++; }
        else if (!strcmp(a, "--list-label-pos")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--input-label")) { }
        else if (!strcmp(a, "--input-label")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--input-label-pos")) { }
        else if (!strcmp(a, "--input-label-pos")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--header-label")) { }
        else if (!strcmp(a, "--header-label")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--header-label-pos")) { }
        else if (!strcmp(a, "--header-label-pos")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--footer-label")) { }
        else if (!strcmp(a, "--footer-label")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--footer-label-pos")) { }
        else if (!strcmp(a, "--footer-label-pos")) { if (i+1<argc) i++; }
        else if (ARG_EQ("--preview-label")) preview_label = a + strlen("--preview-label") + 1;
        else if (!strcmp(a, "--preview-label")) {
            if (i + 1 < argc) preview_label = argv[++i];
            else { fprintf(stderr, "--preview-label requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--preview-label-pos")) {
            if (!mm_parse_label_pos(a + strlen("--preview-label-pos") + 1, &preview_label_pos, &preview_label_bottom)) {
                fprintf(stderr, "invalid --preview-label-pos: %s\n", a + strlen("--preview-label-pos") + 1);
                return 2;
            }
        }
        else if (!strcmp(a, "--preview-label-pos")) {
            if (i + 1 < argc) {
                const char *value = argv[++i];
                if (!mm_parse_label_pos(value, &preview_label_pos, &preview_label_bottom)) {
                    fprintf(stderr, "invalid --preview-label-pos: %s\n", value);
                    return 2;
                }
            } else { fprintf(stderr, "--preview-label-pos requires a value\n"); return 2; }
        }
        else if (!strcmp(a, "--header-lines-border")) {}
        else if (!strcmp(a, "--no-header-lines-border")) {}
        else if (ARG_EQ("--header-border")) {
            header_border_shape = mm_border_parse(a + strlen("--header-border") + 1);
            if (header_border_shape < 0) {
                fprintf(stderr, "invalid --header-border style: %s\n", a + strlen("--header-border") + 1);
                return 2;
            }
            header_border_set = true;
        }
        else if (!strcmp(a, "--header-border")) {
            header_border_shape = MM_BORDER_ROUNDED;
            header_border_set = true;
        }
        else if (!strcmp(a, "--footer-border")) {}
        else if (ARG_EQ("--preview-border")) {
            preview_border_shape = mm_border_parse(a + strlen("--preview-border") + 1);
            if (preview_border_shape < 0) {
                fprintf(stderr, "invalid --preview-border style: %s\n", a + strlen("--preview-border") + 1);
                return 2;
            }
        }
        else if (!strcmp(a, "--preview-border")) { preview_border_shape = MM_BORDER_ROUNDED; }
        else if (ARG_EQ("--list-border")) {
            list_border_shape = mm_border_parse(a + strlen("--list-border") + 1);
            if (list_border_shape < 0) {
                fprintf(stderr, "invalid --list-border style: %s\n", a + strlen("--list-border") + 1);
                return 2;
            }
            list_border_set = true;
        }
        else if (!strcmp(a, "--list-border")) {
            list_border_shape = MM_BORDER_ROUNDED;
            list_border_set = true;
        }
        else if (!strcmp(a, "--input-border")) {}
        else if (ARG_EQ("--border-label-pos")) { /* will be handled below */ }
        else if (!strcmp(a, "--border-label-pos")) { /* will be handled below */ }
    else if (!strcmp(a, "--tmux")) { /* accepted for compatibility */ }
    else if (ARG_EQ("--tmux")) { /* accepted for compatibility */ }
    else if (!strcmp(a, "--popup")) { /* accepted for compatibility */ }
    else if (ARG_EQ("--popup")) { /* accepted for compatibility */ }
    else if (!strcmp(a, "--bench")) { /* accepted for compatibility */ }
    else if (ARG_EQ("--bench")) { /* accepted for compatibility */ }
    else if (!strcmp(a, "--style")) { if (i+1<argc) style_spec = argv[++i]; else { fprintf(stderr, "style preset required\n"); return 2; } }
    else if (ARG_EQ("--style")) style_spec = a + 8;
    else if (!strcmp(a, "--nushell")) { /* shell integration option */ }
    else if (!strcmp(a, "--zsh")) { /* shell integration option */ }
    else if (ARG_EQ("--id-nth")) { /* accepted for compatibility */ if (i+1<argc && a[strlen(a)]=='=' && !strchr(a+8,'=')) {} }
    else if (!strcmp(a, "--id-nth")) { if (i+1<argc) i++; }
    else if (ARG_EQ("--freeze-left")) { /* accepted */ }
    else if (!strcmp(a, "--freeze-left")) { if (i+1<argc) i++; }
    else if (ARG_EQ("--freeze-right")) { /* accepted */ }
    else if (!strcmp(a, "--freeze-right")) { if (i+1<argc) i++; }
    else if (ARG_EQ("--preview-wrap-sign")) preview_wrap_sign = a + strlen("--preview-wrap-sign") + 1;
    else if (!strcmp(a, "--preview-wrap-sign")) {
        if (i + 1 < argc) preview_wrap_sign = argv[++i];
        else { fprintf(stderr, "--preview-wrap-sign requires a value\n"); return 2; }
    }
    else if (ARG_EQ("--border-label-pos")) { /* already handled */ }
    else if (!strcmp(a, "--border-label-pos")) { /* handled */ }
    else if (!strcmp(a, "--listen")) { if (i+1<argc) listen_spec = argv[++i]; }
    else if (ARG_EQ("--listen")) listen_spec = a + 9;
    else if (!strcmp(a, "--listen-unsafe")) { if (i+1<argc) listen_unsafe_spec = argv[++i]; }
    else if (ARG_EQ("--listen-unsafe")) listen_unsafe_spec = a + 16;
        else if (!strcmp(a, "--zsh")) { printf("# zsh integration\n"); return 0; }
        else if (!strcmp(a, "--nushell")) { printf("# nushell integration\n"); return 0; }
        else if (ARG_EQ("--history")) history_spec = a + 10;
        else if (!strcmp(a, "--history")) { if (i+1<argc) history_spec = argv[++i]; }
        else if (ARG_EQ("--history-size")) history_size = atoi(a + 15);
        else if (!strcmp(a, "--history-size")) { if (i+1<argc) history_size = atoi(argv[++i]); }
        else if (ARG_EQ("--pointer")) { const char *v = a + 10; if (*v == '\0') { fprintf(stderr, "pointer sign required\n"); return 2; } pointer_spec = v; }
        else if (!strcmp(a, "--pointer")) { if (i+1<argc) { const char *v = argv[++i]; if (*v == '\0') { fprintf(stderr, "pointer sign required\n"); return 2; } pointer_spec = v; } else { fprintf(stderr, "pointer sign required\n"); return 2; } }
        else if (ARG_EQ("--marker")) { const char *v = a + 9; if (*v == '\0') { fprintf(stderr, "marker sign required\n"); return 2; } marker_spec = v; }
        else if (!strcmp(a, "--marker")) { if (i+1<argc) { const char *v = argv[++i]; if (*v == '\0') { fprintf(stderr, "marker sign required\n"); return 2; } marker_spec = v; } else { fprintf(stderr, "marker sign required\n"); return 2; } }
        else if (ARG_EQ("--with-shell")) { shell_spec = a + 13; if (shell_spec[0]=='\0') { fprintf(stderr, "shell command and flags required\n"); return 2; } }
        else if (!strcmp(a, "--with-shell")) { if (i+1<argc) { shell_spec = argv[++i]; if (!shell_spec || *shell_spec=='\0') { fprintf(stderr, "shell command and flags required\n"); return 2; } } else { fprintf(stderr, "shell command and flags required\n"); return 2; } }
        else if (!strcmp(a, "--man")) {
            /* Print the manual page. */
            return print_man_page();
        }
        else if (ARG_EQ("--preview")) preview_cmd = a + 10;
        else if (!strcmp(a, "--preview")) { if (i+1<argc) preview_cmd = argv[++i]; }
        else if (ARG_EQ("--preview-window")) preview_win_spec = a + 17;
        else if (!strcmp(a, "--preview-window")) { if (i+1<argc) preview_win_spec = argv[++i]; }
        else if (!strcmp(a, "--border")) { border_shape = MM_BORDER_ROUNDED; }
        else if (ARG_EQ("--border-label")) border_label = a + 15;
        else if (!strcmp(a, "--border-label")) { if (i+1<argc) border_label = argv[++i]; }
        else if (ARG_EQ("--border-label-pos")) {
            if (!mm_parse_label_pos(a + 19, &border_label_pos, &border_label_bottom)) {
                fprintf(stderr, "invalid position: %s\n", a + 19);
                return 2;
            }
        }
        else if (!strcmp(a, "--border-label-pos")) {
            if (i+1<argc && !mm_parse_label_pos(argv[++i], &border_label_pos, &border_label_bottom)) {
                fprintf(stderr, "invalid position: %s\n", argv[i]);
                return 2;
            }
        }
        else if (ARG_EQ("--border")) {
            int b = mm_border_parse(a + 9);
            if (b < 0) { fprintf(stderr, "invalid border style\n"); return 2; }
            border_shape = b;
        }
        else if (!strcmp(a, "--no-border")) { border_shape = MM_BORDER_NONE; }
        else if (ARG_EQ("--color")) color_spec = a + 8;
        else if (!strcmp(a, "--color")) { if (i+1<argc) color_spec = argv[++i]; }
        else if (ARG_EQ("--bind")) {
            const char *v = a + 7;
            if (*v == '\0') { fprintf(stderr, "bind expression required\n"); return 2; }
            bind_spec = v;
        }
        else if (!strcmp(a, "--bind")) {
            if (i+1 >= argc) { fprintf(stderr, "bind expression required\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0') { fprintf(stderr, "bind expression required\n"); return 2; }
            bind_spec = v;
        }
        else if (ARG_EQ("--expect")) expect_spec = a + 9;
        else if (!strcmp(a, "--expect")) { if (i+1<argc) expect_spec = argv[++i]; }
        else if (ARG_EQ("--prompt")) prompt = a + 9;
        else if (!strcmp(a, "--prompt")) { if (i+1<argc) prompt = argv[++i]; }
        else if (ARG_EQ("--header")) header = a + 9;
        else if (!strcmp(a, "--header")) { if (i+1<argc) header = argv[++i]; }
        else if (ARG_EQ("--header-lines")) header_lines = atoi(a + 15);
        else if (!strcmp(a, "--header-lines")) { if (i+1<argc) header_lines = atoi(argv[++i]); }
        else if (!strcmp(a, "--ansi")) ansi = true;
        else if (!strcmp(a, "--no-ansi")) ansi = false;
        else if (ARG_EQ("--algo")) { const char *al = a + 7; if (strcmp(al, "v1") && strcmp(al, "v2")) { fprintf(stderr, "invalid algorithm: %s\n", al); return 2; } }
        else if (!strcmp(a, "--version")) { printf("0.74.3 (c23)\n"); return 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (!strcmp(a, "--")) { /* end of options */ }
        else if (a[0] == '-' && a[1] != 0) {
            /* fzf rejects unknown options (PLAN: no silently ignored flags). */
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
        #undef ARG_EQ
    }

    /* Key bindings and expected keys are parsed before the input is read so
       that a parse error does not leak the input buffer. */
    mm_keymap_t keymap;
    mm_event_map_t events;
    memset(&keymap, 0, sizeof(keymap));
    memset(&events, 0, sizeof(events));
    char **expect_keys = NULL;
    int expect_n = 0;
    const char **skip_names = NULL;
    int nskip = 0;
    {
        const char *err = NULL;
        const char *defaults =
            "ctrl-c:abort,ctrl-g:abort,ctrl-q:abort,esc:abort,"
            "enter:accept,ctrl-m:accept,"
            "up:up,ctrl-p:up,down:down,ctrl-n:down,"
            "home:first,ctrl-a:first,end:last,ctrl-e:last,"
            "page-up:page-up,page-down:page-down,"
            "backspace:backward-delete-char,ctrl-h:backward-delete-char,"
            "delete:delete-char,ctrl-d:delete-char,"
            "ctrl-u:kill-line,ctrl-w:backward-kill-word,"
            "tab:toggle,"
            "btab:toggle,ctrl-z:toggle";
        if (!mm_bind_parse(&keymap, defaults, false, &err)) {
            fprintf(stderr, "invalid default binding: %s\n", err);
            goto fail;
        }
        if (bind_spec && !mm_bind_parse_full(&keymap, &events, bind_spec, &err)) {
            fprintf(stderr, "invalid --bind: %s\n", err);
            goto fail;
        }
        if (bind_spec && !mm_bind_parse(&keymap, bind_spec, true, &err)) {
            fprintf(stderr, "invalid --bind: %s\n", err);
            goto fail;
        }
    }
    if (expect_spec) {
        /* --expect takes a comma separated list of key names. */
        int cap = 8;
        expect_keys = malloc((size_t)cap * sizeof(char *));
        const char *p = expect_spec;
        while (*p) {
            const char *comma = strchr(p, ',');
            size_t len = comma ? (size_t)(comma - p) : strlen(p);
            if (len > 0) {
                if (expect_n == cap) { cap *= 2; expect_keys = realloc(expect_keys, (size_t)cap * sizeof(char *)); }
                char *k = malloc(len + 1);
                memcpy(k, p, len);
                k[len] = 0;
                expect_keys[expect_n++] = k;
            }
            if (!comma) break;
            p = comma + 1;
        }
    }

    if (walker_spec && !mm_walker_parse(walker_spec, &walker)) {
        fprintf(stderr, "invalid walker option: %s\n", walker_spec);
        goto fail;
    }
    if (walker_skip && *walker_skip) {
        int cap = 8;
        skip_names = malloc((size_t)cap * sizeof(char *));
        const char *q = walker_skip;
        while (*q) {
            const char *comma = strchr(q, ',');
            size_t l = comma ? (size_t)(comma - q) : strlen(q);
            if (l > 0) {
                if (nskip == cap) { cap *= 2; skip_names = realloc(skip_names, (size_t)cap * sizeof(char *)); }
                char *nm = malloc(l + 1);
                memcpy(nm, q, l);
                nm[l] = 0;
                skip_names[nskip++] = nm;
            }
            if (!comma) break;
            q = comma + 1;
        }
        walker.skip = (const char **)skip_names;
        walker.nskip = nskip;
    }

    /* Color theme */
    mm_theme_t theme;
    mm_theme_init(&theme);
    if (color_spec) {
        const char *cerr = NULL;
        if (!mm_theme_parse(&theme, color_spec, &cerr)) {
            fprintf(stderr, "%s: %s\n", cerr, color_spec);
            goto fail;
        }
    }

    /* scheme validation */
    mm_sort_criteria_t crit;
    if (!mm_scheme_criteria(scheme, &crit)) {
        fprintf(stderr, "invalid scoring scheme: %s (expected: default|path|history)\n", scheme);
        goto fail;
    }
    if (!mm_algo_init(scheme)) {
        fprintf(stderr, "invalid scoring scheme: %s (expected: default|path|history)\n", scheme);
        goto fail;
    }
    if (tiebreak) {
        const char *err = NULL;
        if (!mm_parse_tiebreak(tiebreak, &crit, &err)) {
            fprintf(stderr, "%s\n", err);
            goto fail;
        }
    }

    (void)query;

    size_t len = 0;
    char *data = NULL;
    char **lines = NULL;
    int count = 0;
    bool stream_input = false;
    int stream_fd = -1;
    FILE *stream_fp = NULL;
    pthread_t walk_thread;
    bool have_walk_thread = false;
    if (!no_input) {
    if (isatty(STDIN_FILENO)) {
        const char *cmd = getenv("FZF_DEFAULT_COMMAND");
        if (cmd && *cmd) {
            FILE *fp = popen(cmd, "r");
            if (fp) {
                if (header_lines > 0) {
                    size_t cap_data = 1024 * 1024, n_data = 0;
                    data = malloc(cap_data);
                    if (!data) { pclose(fp); goto interactive_error; }
                    for (;;) {
                        if (n_data + 131072 > cap_data) { cap_data *= 2; data = realloc(data, cap_data); if (!data) break; }
                        size_t r = fread(data + n_data, 1, 131072, fp);
                        n_data += r;
                        if (r < 131072) break;
                    }
                    len = n_data;
                    if (data) data[len] = 0;
                    pclose(fp);
                } else {
                    int fd = fileno(fp);
                    int fl = fcntl(fd, F_GETFL, 0);
                    if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK);
                    stream_fp = fp;
                    stream_fd = fd;
                    stream_input = true;
                    data = malloc(1);
                    if (!data) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
                    data[0] = 0;
                    len = 0;
                }
            } else {
                data = read_all_stdin(&len);
            }
        } else {
            if (header_lines > 0) {
                char **paths = NULL;
                int npaths = 0;
                if (!mm_walk(walker_root, &walker, &paths, &npaths)) {
                    fprintf(stderr, "fzf: out of memory\n");
                    goto fail;
                }
                size_t total = 1;
                for (int i = 0; i < npaths; i++) total += strlen(paths[i]) + 1;
                data = malloc(total);
                size_t o = 0;
                for (int i = 0; i < npaths; i++) {
                    size_t l = strlen(paths[i]);
                    memcpy(data + o, paths[i], l);
                    o += l;
                    data[o++] = '\n';
                }
                data[o] = 0;
                len = o;
                mm_walk_free(paths, npaths);
            } else {
                int pipefd[2];
                if (pipe(pipefd) != 0) {
                    fprintf(stderr, "fzf: pipe failed\n");
                    goto fail;
                }
                mm_walk_thread_arg_t *arg = malloc(sizeof(*arg));
                if (!arg) { close(pipefd[0]); close(pipefd[1]); goto fail; }
                arg->fd = pipefd[1];
                arg->root = strdup(walker_root ? walker_root : ".");
                arg->opts = walker;
                if (pthread_create(&walk_thread, NULL, walk_thread_fn, arg) != 0) {
                    free(arg->root);
                    free(arg);
                    close(pipefd[0]);
                    close(pipefd[1]);
                    goto fail;
                }
                have_walk_thread = true;
                stream_fd = pipefd[0];
                stream_input = true;
                data = malloc(1);
                if (!data) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
                data[0] = 0;
                len = 0;
            }
        }
    } else if (!filter && tail < 0 && header_lines == 0) {
        stream_input = true;
        stream_fd = STDIN_FILENO;
        data = malloc(1);
        if (!data) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
        data[0] = 0;
        len = 0;
    } else {
        data = read_all_stdin(&len);
    }
    } else {
        data = malloc(1);
        if (!data) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
        data[0] = 0;
        len = 0;
    }

    /* split lines */
    int cap = 1024;
    lines = malloc((size_t)cap * sizeof(char *));
    if (!lines) { free(data); fprintf(stderr, "fzf: out of memory\n"); return 2; }
    char sep = read0 ? '\0' : '\n';
    char *p = data;
    while (p < data + len) {
        char *nl = memchr(p, sep, (size_t)(data + len - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(data + len - p);
        if (!read0 && llen > 0 && p[llen - 1] == '\r') llen--;
        char *line = malloc(llen + 1);
        if (!line) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
        memcpy(line, p, llen);
        line[llen] = 0;
        if (count == cap) {
            char **nl2 = realloc(lines, (size_t)cap * 2 * sizeof(char *));
            if (!nl2) { free(line); fprintf(stderr, "fzf: out of memory\n"); return 2; }
            lines = nl2; cap *= 2;
        }
        lines[count++] = line;
        if (!nl) break;
        p = nl + 1;
    }

    if (tail >= 0 && count > tail) {
        /* keep last N */
        int drop = count - tail;
        for (int i = 0; i < drop; i++) free(lines[i]);
        memmove(lines, lines + drop, (size_t)tail * sizeof(char *));
        count = tail;
    }

    if (header_lines > 0 && !stream_input && count > 0) {
        int h = header_lines;
        if (h > count) h = count;
        size_t hlen = 1;
        for (int i = 0; i < h; i++) hlen += strlen(lines[i]) + 1;
        char *hbuf = malloc(hlen);
        if (hbuf) {
            hbuf[0] = 0;
            for (int i = 0; i < h; i++) {
                if (i) strcat(hbuf, "\n");
                strcat(hbuf, lines[i]);
            }
            header_lines_text = hbuf;
        }
        for (int i = 0; i < h; i++) free(lines[i]);
        memmove(lines, lines + h, (size_t)(count - h) * sizeof(char *));
        count -= h;
    }

    /* Apply style presets after option parsing, matching upstream fzf's
       preset precedence. Component-specific borders are tracked separately
       in the feature inventory; this renderer currently has one outer box. */
    if (style_spec) {
        if (!strcmp(style_spec, "default")) {
            info_inline_right = false;
            highlight_line = false;
        } else if (!strcmp(style_spec, "minimal")) {
            info_inline_right = false;
            highlight_line = false;
            no_scrollbar = true;
            gutter_spec = " ";
            separator_spec = "";
        } else {
            char preset[128];
            size_t n = strlen(style_spec);
            if (n >= sizeof(preset)) {
                fprintf(stderr, "unsupported style preset: %s\n", style_spec);
                return 2;
            }
            memcpy(preset, style_spec, n + 1);
            char *colon = strchr(preset, ':');
            if (colon) *colon++ = '\0';
            if (strcmp(preset, "full") != 0) {
                fprintf(stderr, "unsupported style preset: %s\n", style_spec);
                return 2;
            }
            style_full = true;
            border_shape = MM_BORDER_ROUNDED;
            if (colon && *colon) {
                int b = mm_border_parse(colon);
                if (b < 0) {
                    fprintf(stderr, "unsupported style border: %s\n", colon);
                    return 2;
                }
                border_shape = b;
            }
            info = true;
            info_inline_right = true;
            highlight_line = true;
        }
    }

    if (!filter) {
        /* Interactive mode. */
        mm_ui_t ui;
        memset(&ui, 0, sizeof(ui));
        ui.crit = crit;
        if (!mm_delimiter_init(&ui.delim, delim_str)) {
            fprintf(stderr, "fzf: out of memory\n");
            return 2;
        }
        ui.extended = extended;
        ui.fuzzy = !exact;
        /* --raw searches the items exactly, like --exact. */
        (void)raw;
        ui.case_mode = case_mode;
        ui.tac = tac;
        ui.no_sort = no_sort;
        ui.multi = multi;
        ui.multi_max = multi_max;
        ui.reverse = reverse;
        ui.cycle = cycle;
        ui.info = info;
        ui.info_inline_right = info_inline_right;
        ui.prompt = prompt;
        ui.header = header;
        ui.header_lines = header_lines_text;
        ui.query = query;
        ui.print_query = print_query;
        ui.expect = expect;
        ui.keymap = keymap;
        memset(&keymap, 0, sizeof(keymap));
        ui.expect_keys = expect_keys;
        ui.expect_n = expect_n;
        expect_keys = NULL;
        expect_n = 0;
        ui.read0 = read0;
        ui.print0 = print0;
        ui.select_1 = select_1;
        ui.exit_0 = exit_0;
        ui.ansi = ansi;
        ui.literal = literal;
        ui.theme = theme;
        ui.border = border_shape;
        ui.preview_border = preview_border_shape;
        ui.preview_label = preview_label;
        ui.preview_wrap_sign = preview_wrap_sign;
        ui.preview_label_pos = preview_label_pos;
        ui.preview_label_bottom = preview_label_bottom;
        ui.list_border = list_border_shape;
        ui.list_border_set = list_border_set;
        ui.header_border = header_border_shape;
        ui.header_border_set = header_border_set;
        ui.style_full = style_full;
        ui.border_label = border_label;
        ui.border_label_pos = border_label_pos;
        ui.border_label_bottom = border_label_bottom;
        /* Without unicode the default pointer is an ASCII character. */
        ui.pointer = pointer_spec ? pointer_spec : (no_unicode ? ">" : NULL);
        ui.marker = marker_spec;
        ui.shell = shell_spec;
        ui.ghost = ghost_spec;
        ui.height_spec = height_spec;
        ui.ellipsis = ellipsis_spec ? ellipsis_spec : (no_unicode ? ".." : NULL);
        ui.no_bold = no_bold;
        ui.gap = gap;
        ui.phony = phony;
        ui.footer = footer_spec;
        ui.gutter = gutter_spec;
        /* The raw gutter is only used in the raw search mode, which is not
           implemented; the option is accepted like the reference does. */
        (void)gutter_raw_spec;
        ui.no_unicode = no_unicode;
        /* The horizontal scrolling of the preview and the multi-line items
           are not implemented; the options are accepted like the reference
           does. */
        (void)no_hscroll;
        (void)no_multi_line;
        (void)no_tty_default;
        /* Options parsed above but not yet wired to the UI struct.
           The lead adds the mm_ui_t fields and passes them through. */
        ui.highlight_line = highlight_line;
        ui.info_command = info_command;
        ui.jump_labels = jump_labels;
        ui.marker_multi_line = marker_multi_line;
        ui.threads = threads;
        ui.track = track;
        ui.ambidouble = ambidouble;
        ui.filepath_word = filepath_word;
        ui.header_first = header_first;
        ui.listen_spec = listen_spec;
        ui.listen_unsafe_spec = listen_unsafe_spec;
        ui.wrap = wrap;
        ui.wrap_word = wrap_word;
        /* fzf's default wrap sign is the arrow and a space. */
        ui.wrap_sign = wrap_sign ? wrap_sign : (no_unicode ? "> " : "\u21b3 ");
        /* fzf's default gap line is the dashed horizontal character. */
        ui.gap_line = gap_line ? gap_line : (no_unicode ? "-" : "\u2508");
        ui.padding = padding;
        ui.margin = margin;
        ui.scroll_off = scroll_off;
        ui.hscroll_off = hscroll_off;
        ui.separator = separator_spec ? separator_spec : "";
        ui.no_scrollbar = no_scrollbar;
        ui.no_clear = no_clear;
        ui.tabstop = tabstop;
        ui.no_input = no_input;
        ui.no_color = no_color;
        /* --keep-right, --no-mouse and --sync match the behaviour this
           implementation already has: the preview is on the right, the mouse
           is never enabled and the initial search is always synchronous. */
        (void)keep_right;
        (void)no_mouse;
        (void)sync;
        ui.min_height = min_height;
        if (no_color) {
            /* The "no color" theme: every colour is the terminal default. */
            memset(&ui.theme, 0, sizeof(ui.theme));
            ui.theme.fg.color = MM_COL_DEFAULT;
            ui.theme.bg.color = MM_COL_DEFAULT;
            ui.theme.current.color = MM_COL_DEFAULT;
            ui.theme.current_match.color = MM_COL_DEFAULT;
            ui.theme.match.color = MM_COL_DEFAULT;
            ui.theme.pointer.color = MM_COL_DEFAULT;
            ui.theme.gutter.color = MM_COL_DEFAULT;
            ui.theme.prompt.color = MM_COL_DEFAULT;
            ui.theme.info.color = MM_COL_DEFAULT;
            ui.theme.border.color = MM_COL_DEFAULT;
            ui.theme.dark_bg.color = MM_COL_DEFAULT;
        }
        if (layout_spec) {
            if (!strcmp(layout_spec, "default")) ui.layout = 0;
            else if (!strcmp(layout_spec, "reverse")) { ui.layout = 0; ui.reverse = true; }
            else if (!strcmp(layout_spec, "reverse-list")) ui.layout = 1;
            else {
                fprintf(stderr, "invalid layout (expected: default / reverse / reverse-list)\n");
                goto interactive_error;
            }
        }
        if (history_spec) {
            ui.history = mm_history_load(history_spec, history_size);
            if (!ui.history) {
                fprintf(stderr, "fzf: invalid history file: %s\n", history_spec);
                goto interactive_error;
            }
        }
        if (preview_cmd) {
            ui.preview = mm_preview_new(preview_cmd, shell_spec);
            if (!ui.preview) {
                fprintf(stderr, "fzf: out of memory\n");
                goto interactive_error;
            }
            if (!mm_preview_window_parse(preview_win_spec, &ui.preview_win)) {
                fprintf(stderr, "invalid preview window: %s\n",
                        preview_win_spec ? preview_win_spec : "");
                goto interactive_error;
            }
            if (style_full) {
                ui.preview_win.border = true;
                ui.preview_win.border_left_only = false;
            }
        }
        if (with_nth_spec && !mm_parse_ranges(with_nth_spec, &ui.nth, &ui.nth_n)) {
            fprintf(stderr, "invalid --with-nth: %s\n", with_nth_spec);
            goto interactive_error;
        }
        ui.length_from_transform = (!nth_spec && with_nth_spec);
        if (!mm_ui_init(&ui, lines, count, cap)) {
            fprintf(stderr, "fzf: out of memory\n");
            goto interactive_error;
        }
        if (stream_input) {
            /* Read the items without blocking, so that the program starts
               right away and the list grows while it runs. */
            int flags = fcntl(stream_fd, F_GETFL, 0);
            if (flags >= 0) fcntl(stream_fd, F_SETFL, flags | O_NONBLOCK);
            ui.input_fd = stream_fd;
            ui.input_sep = read0 ? '\0' : '\n';
        }
        int rc = mm_ui_run(&ui);
        if (have_walk_thread) pthread_join(walk_thread, NULL);
        if (stream_fp) pclose(stream_fp);
        else if (stream_input && stream_fd >= 0 && stream_fd != STDIN_FILENO) close(stream_fd);
        mm_ui_free(&ui);
        free(data);
        mm_keymap_free(&keymap);
        for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
        free(expect_keys);
        for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
        free(skip_names);
        return rc;
    interactive_error:
        mm_ui_free(&ui);
        mm_keymap_free(&keymap);
        for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
        free(expect_keys);
        for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
        free(skip_names);
        free(data);
        return 2;
    }

    bool forward = mm_criteria_forward(&crit);
    mm_pattern_t *pat = mm_pattern_build(filter, !exact, extended, case_mode, !literal, forward, true);
    if (!pat) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
    if (no_sort) mm_pattern_set_sortable(pat, false);

    mm_delimiter_t delim;
    if (!mm_delimiter_init(&delim, delim_str)) {
        fprintf(stderr, "fzf: out of memory\n");
        goto fail;
    }

    mm_range_t *nth = NULL; int nth_n = 0;
    if (nth_spec && !mm_parse_ranges(nth_spec, &nth, &nth_n)) {
        fprintf(stderr, "invalid --nth: %s\n", nth_spec);
        goto fail;
    }
    mm_range_t *wnth = NULL; int wnth_n = 0;
    if (with_nth_spec && !mm_parse_ranges(with_nth_spec, &wnth, &wnth_n)) {
        fprintf(stderr, "invalid --with-nth: %s\n", with_nth_spec);
        goto fail;
    }

    /* fzf: when --nth is absent, the search range defaults to --with-nth. */
    if (!nth && wnth) { nth = wnth; nth_n = wnth_n; }

    bool length_from_transform = false;
    if (!nth_spec && with_nth_spec) length_from_transform = true;
    mm_input_t input = { lines, count, NULL, 0, delim, nth, nth_n, length_from_transform };
    int n = 0;
    int *out = mm_batch_search(&input, pat, &crit, tac, &n, NULL);
    (void)no_sort; /* handled by pat->sortable */

    char osep = print0 ? '\0' : '\n';
    if (print_query) { fputs(filter, stdout); fputc(osep, stdout); }
    for (int i = 0; i < n; i++) {
        const char *line = lines[out[i]];
        if (accept_nth > 0) {
            int tn = 0;
            int *pl = NULL;
            char **toks = mm_tokenize(line, delim, &pl, &tn);
            if (toks && accept_nth <= tn) {
                char *field = mm_strip_last_delimiter(toks[accept_nth - 1], delim);
                if (field) { fputs(field, stdout); free(field); }
            }
            if (pl) free(pl);
            if (toks) {
                for (int k = 0; k < tn; k++) free(toks[k]);
                free(toks);
            }
        } else {
            fputs(line, stdout);
        }
        fputc(osep, stdout);
    }

    mm_pattern_free(pat);
    mm_delimiter_free(&delim);
    free(out);
    free(nth);
    if (wnth != nth) free(wnth);
    for (int i = 0; i < count; i++) free(lines[i]);
    free(lines);
    free(data);
    mm_keymap_free(&keymap);
    for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
    free(expect_keys);
    for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
    free(skip_names);
    return n == 0 ? 1 : 0;

fail:
    mm_keymap_free(&keymap);
    for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
    free(expect_keys);
    for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
    free(skip_names);
    return 2;
}
