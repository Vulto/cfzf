/* main.c - fzf-compatible command-line entry point for the C23 rewrite. */
#include "algo.h"
#include "pattern.h"
#include "result.h"
#include "filter.h"
#include "ui.h"
#include "walker.h"
#include "preview.h"
#include "history.h"
#include "color.h"
#include "tokenizer.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <locale.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/types.h>
#include <pthread.h>
#include <signal.h>

static void usage(FILE *out) {
    fprintf(out, "usage: fzf [OPTIONS]\n");
}

static bool parse_nonnegative_int(const char *s, int *out) {
    if (!s || !*s) return false;
    char *end = NULL;
    long value = strtol(s, &end, 10);
    if (end == s || *end != '\0' || value < 0 || value > INT_MAX)
        return false;
    *out = (int)value;
    return true;
}

static bool parse_cli_nonnegative(const char *option, const char *value,
                                  int *out) {
    if (parse_nonnegative_int(value, out)) return true;
    fprintf(stderr, "invalid %s value: %s\n", option,
            value ? value : "");
    return false;
}

static bool parse_cli_positive(const char *option, const char *value,
                               int *out) {
    if (parse_nonnegative_int(value, out) && *out > 0) return true;
    fprintf(stderr, "invalid %s value: %s\n", option,
            value ? value : "");
    return false;
}

static const char *required_cli_arg(int argc, char **argv, int *index,
                                    const char *option) {
    if (*index + 1 >= argc) {
        fprintf(stderr, "%s requires a value\n", option);
        return NULL;
    }
    return argv[++*index];
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
    size_t cap = 4u * 1024u * 1024u, len = 0;
    char *buf = malloc(cap);
    if (!buf) { *outlen = 0; return NULL; }
    for (;;) {
        /* Keep room for both the next read and the trailing NUL. */
        if (cap - len < 262145u) {
            if (cap > SIZE_MAX / 2) {
                free(buf);
                *outlen = 0;
                return NULL;
            }
            size_t next_cap = cap * 2;
            char *next = realloc(buf, next_cap);
            if (!next) {
                free(buf);
                *outlen = 0;
                return NULL;
            }
            buf = next;
            cap = next_cap;
        }
        size_t nread = fread(buf + len, 1, 262144u, stdin);
        len += nread;
        if (nread < 262144u) {
            if (ferror(stdin)) {
                free(buf);
                *outlen = 0;
                return NULL;
            }
            break;
        }
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
    sigset_t blocked_signals;
    (void)sigemptyset(&blocked_signals);
    (void)sigaddset(&blocked_signals, SIGPIPE);
    (void)pthread_sigmask(SIG_BLOCK, &blocked_signals, NULL);
    mm_walk_stream(a->fd, a->root, &a->opts);
    free(a->root);
    free(a);
    return NULL;
}

static bool mm_parse_label_pos(const char *s, int *pos, bool *bottom) {
    char *end = NULL;
    long v;
    if (!strncmp(s, "center", 6) && (s[6] == 0 || s[6] == ':')) {
        v = 0;
        end = (char *)s + 6;
    } else {
        v = strtol(s, &end, 10);
        if (end == s) return false;
    }
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
    const char *id_nth_spec = NULL;
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
    int input_border_shape = -1;
    bool input_border_set = false;
    const char *input_label = NULL;
    int input_label_pos = 0;
    bool input_label_bottom = false;
    const char *border_label = NULL;
    int border_label_pos = 0;
    bool border_label_bottom = false;
    const char *preview_cmd = NULL;
    const char *preview_label = NULL;
    const char *list_label = NULL;
    int list_label_pos = 0;
    bool list_label_bottom = false;
    const char *header_label = NULL;
    int header_label_pos = 0;
    bool header_label_bottom = false;
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
    bool disabled = false;
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
    const char *accept_nth_spec = NULL;
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
    bool end_options = false;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (end_options) {
            if (!query) query = a;
            else {
                fprintf(stderr, "unexpected positional argument: %s\n", a);
                return 2;
            }
            continue;
        }
        if (!strcmp(a, "--")) {
            end_options = true;
            continue;
        }
        if (a[0] != '-' && a[0] != '+') {
            if (!query) query = a;
            else {
                fprintf(stderr, "unexpected positional argument: %s\n", a);
                return 2;
            }
            continue;
        }
        #define ARG_EQ(name) (!strncmp(a, name "=", sizeof(name)))
        if (ARG_EQ("--filter")) { filter = a + sizeof("--filter=") - 1; }
        else if (!strcmp(a, "-f") || !strcmp(a, "--filter")) {
            filter = required_cli_arg(argc, argv, &i, "--filter");
            if (!filter) return 2;
        }
        else if (!strcmp(a, "--query") || !strcmp(a, "-q")) {
            query = required_cli_arg(argc, argv, &i, "--query");
            if (!query) return 2;
        }
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
        else if (!strcmp(a, "--scheme")) {
            scheme = required_cli_arg(argc, argv, &i, "--scheme");
            if (!scheme) return 2;
        }
        else if (ARG_EQ("--tiebreak")) { tiebreak = a + 11; }
        else if (!strcmp(a, "--tiebreak")) {
            tiebreak = required_cli_arg(argc, argv, &i, "--tiebreak");
            if (!tiebreak) return 2;
        }
        else if (ARG_EQ("--tail")) {
            if (!parse_cli_positive("--tail", a + 7, &tail)) return 2;
        }
        else if (!strcmp(a, "--tail")) {
            if (i + 1 >= argc) {
                (void)parse_cli_positive("--tail", NULL, &tail);
                return 2;
            }
            if (!parse_cli_positive("--tail", argv[++i], &tail)) return 2;
        }
        else if (!strcmp(a, "--no-tail")) tail = -1;
        else if (ARG_EQ("--nth")) { nth_spec = a + 6; }
        else if (!strcmp(a, "--nth")) {
            nth_spec = required_cli_arg(argc, argv, &i, "--nth");
            if (!nth_spec) return 2;
        }
        else if (ARG_EQ("--with-nth")) { with_nth_spec = a + 11; }
        else if (!strcmp(a, "--with-nth")) {
            with_nth_spec = required_cli_arg(argc, argv, &i, "--with-nth");
            if (!with_nth_spec) return 2;
        }
        else if (ARG_EQ("--delimiter")) { delim_str = a + 12; }
        else if (!strcmp(a, "--delimiter")) {
            delim_str = required_cli_arg(argc, argv, &i, "--delimiter");
            if (!delim_str) return 2;
        }
        else if (!strcmp(a, "-d")) {
            delim_str = required_cli_arg(argc, argv, &i, "--delimiter");
            if (!delim_str) return 2;
        }
        else if (!strncmp(a, "-d", 2) && a[2] != 0 && a[1] != '-') { delim_str = a + 2; }
        else if (!strcmp(a, "--literal")) literal = true;
        else if (!strcmp(a, "--no-literal")) literal = false;
        else if (!strcmp(a, "--read0") || !strcmp(a, "-0")) read0 = true;
        else if (!strcmp(a, "--no-read0") || !strcmp(a, "+0")) read0 = false;
        else if (!strcmp(a, "--print0")) print0 = true;
        else if (!strcmp(a, "--sync")) { /* batch is inherently sync */ }
        else if (!strcmp(a, "--multi") || !strcmp(a, "-m")) multi = true;
        else if (!strcmp(a, "--no-multi")) multi = false;
        else if (ARG_EQ("--multi-max")) {
            if (!parse_cli_nonnegative("--multi-max", a + 12, &multi_max)) return 2;
        }
        else if (!strcmp(a, "--multi-max")) {
            if (i + 1 >= argc) {
                (void)parse_cli_nonnegative("--multi-max", NULL, &multi_max);
                return 2;
            }
            if (!parse_cli_nonnegative("--multi-max", argv[++i], &multi_max)) return 2;
        }
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
        else if (!strcmp(a, "--walker")) {
            walker_spec = required_cli_arg(argc, argv, &i, "--walker");
            if (!walker_spec) return 2;
        }
        else if (ARG_EQ("--walker-root")) {
            walker_root = a + 14;
            if (!*walker_root) {
                fprintf(stderr, "--walker-root requires a value\\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--walker-root")) {
            walker_root = required_cli_arg(argc, argv, &i, "--walker-root");
            if (!walker_root) return 2;
        }
        else if (ARG_EQ("--walker-skip")) walker_skip = a + 14;
        else if (!strcmp(a, "--walker-skip")) {
            walker_skip = required_cli_arg(argc, argv, &i, "--walker-skip");
            if (!walker_skip) return 2;
        }
        else if (!strcmp(a, "--no-bold")) no_bold = true;
        else if (ARG_EQ("--gap")) {
            if (!parse_cli_nonnegative("--gap", a + 6, &gap)) return 2;
        }
        else if (!strcmp(a, "--gap")) {
            gap = 1;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                int value;
                if (!parse_nonnegative_int(argv[i + 1], &value)) {
                    fprintf(stderr, "invalid --gap value: %s\n", argv[i + 1]);
                    return 2;
                }
                gap = value;
                i++;
            }
        }
        else if (!strcmp(a, "--no-gap")) gap = 0;
        else if (ARG_EQ("--gap-line")) gap_line = a + 11;
        else if (!strcmp(a, "--gap-line")) {
            gap_line = NULL;
            if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+')
                gap_line = argv[++i];
        }
        else if (!strcmp(a, "--no-gap-line")) gap_line = "";
        else if (!strcmp(a, "--phony")) phony = true;
        else if (!strcmp(a, "--no-phony")) phony = false;
        else if (ARG_EQ("--footer")) footer_spec = a + 9;
        else if (ARG_EQ("--padding")) {
            if (!parse_cli_nonnegative("--padding", a + 10, &padding)) return 2;
        }
        else if (ARG_EQ("--margin")) {
            if (!parse_cli_nonnegative("--margin", a + 9, &margin)) return 2;
        }
        else if (!strcmp(a, "--margin")) {
            const char *v = required_cli_arg(argc, argv, &i, "--margin");
            if (!v || !parse_cli_nonnegative("--margin", v, &margin)) return 2;
        }
        else if (!strcmp(a, "--padding")) {
            const char *v = required_cli_arg(argc, argv, &i, "--padding");
            if (!v || !parse_cli_nonnegative("--padding", v, &padding)) return 2;
        }
        else if (ARG_EQ("--gutter")) gutter_spec = a + 9;
        else if (!strcmp(a, "--gutter")) { if (i+1<argc) gutter_spec = argv[++i]; }
        else if (ARG_EQ("--gutter-raw")) gutter_raw_spec = a + 13;
        else if (!strcmp(a, "--gutter-raw")) { if (i+1<argc) gutter_raw_spec = argv[++i]; }
        else if (!strcmp(a, "--footer")) { if (i+1<argc) footer_spec = argv[++i]; }
        else if (!strcmp(a, "--no-footer")) footer_spec = NULL;
        /* --disabled initially shows all items; search can be enabled by action. */
        else if (!strcmp(a, "--disabled")) disabled = true;
        else if (!strcmp(a, "--no-disabled")) disabled = false;
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
        else if (ARG_EQ("--scroll-off")) {
            if (!parse_cli_nonnegative("--scroll-off", a + 13, &scroll_off)) return 2;
        }
        else if (!strcmp(a, "--scroll-off")) {
            if (i + 1 >= argc) {
                (void)parse_cli_nonnegative("--scroll-off", NULL, &scroll_off);
                return 2;
            }
            if (!parse_cli_nonnegative("--scroll-off", argv[++i], &scroll_off)) return 2;
        }
        else if (ARG_EQ("--hscroll-off")) {
            if (!parse_cli_nonnegative("--hscroll-off", a + 14, &hscroll_off)) return 2;
        }
        else if (!strcmp(a, "--hscroll-off")) {
            if (i + 1 >= argc) {
                (void)parse_cli_nonnegative("--hscroll-off", NULL, &hscroll_off);
                return 2;
            }
            if (!parse_cli_nonnegative("--hscroll-off", argv[++i], &hscroll_off)) return 2;
        }
        else if (ARG_EQ("--separator")) separator_spec = a + 12;
        else if (!strcmp(a, "--separator")) { if (i+1<argc) separator_spec = argv[++i]; }
        else if (ARG_EQ("--accept-nth")) accept_nth_spec = a + 13;
        else if (!strcmp(a, "--accept-nth")) {
            accept_nth_spec = required_cli_arg(argc, argv, &i, "--accept-nth");
            if (!accept_nth_spec) return 2;
        }
        else if (!strcmp(a, "--no-scrollbar")) no_scrollbar = true;
        else if (!strcmp(a, "--scrollbar")) no_scrollbar = false;
        else if (!strcmp(a, "--no-clear")) no_clear = true;
        else if (!strcmp(a, "--no-separator")) { /* separator defaults to empty */ }
        else if (ARG_EQ("--tabstop")) {
            if (!parse_cli_positive("--tabstop", a + 10, &tabstop)) return 2;
        }
        else if (!strcmp(a, "--tabstop")) {
            if (i + 1 >= argc) {
                (void)parse_cli_positive("--tabstop", NULL, &tabstop);
                return 2;
            }
            if (!parse_cli_positive("--tabstop", argv[++i], &tabstop)) return 2;
        }
        else if (!strcmp(a, "--no-input")) no_input = true;
        else if (!strcmp(a, "--no-hscroll")) no_hscroll = true;
        else if (!strcmp(a, "--no-multi-line")) no_multi_line = true;
        else if (!strcmp(a, "--no-tty-default")) no_tty_default = true;
        else if (!strcmp(a, "--no-unicode")) no_unicode = true;
        else if (!strcmp(a, "--unicode")) no_unicode = false;
        else if (!strcmp(a, "--raw")) { raw = true; exact = true; }
        else if (!strcmp(a, "--no-raw")) { raw = false; exact = false; }
        else if (ARG_EQ("--ghost")) ghost_spec = a + 8;
        else if (!strcmp(a, "--ghost")) {
            ghost_spec = required_cli_arg(argc, argv, &i, "--ghost");
            if (!ghost_spec) return 2;
        }
        else if (!strcmp(a, "--highlight-line")) highlight_line = true;
        else if (!strcmp(a, "--no-highlight-line")) highlight_line = false;
        else if (ARG_EQ("--info-command")) info_command = a + 15;
        else if (!strcmp(a, "--info-command")) { if (i+1<argc) info_command = argv[++i]; }
        else if (!strcmp(a, "--no-info-command")) info_command = NULL;
        else if (ARG_EQ("--jump-labels")) { const char *v = a + 14; if (*v == '\0') { fprintf(stderr, "label characters required\n"); return 2; } jump_labels = v; }
        else if (!strcmp(a, "--jump-labels")) { if (i+1<argc) { const char *v = argv[++i]; if (*v == '\0') { fprintf(stderr, "label characters required\n"); return 2; } jump_labels = v; } else { fprintf(stderr, "label characters required\n"); return 2; } }
        else if (ARG_EQ("--marker-multi-line")) marker_multi_line = a + 20;
        else if (!strcmp(a, "--marker-multi-line")) {
            marker_multi_line = required_cli_arg(argc, argv, &i, "--marker-multi-line");
            if (!marker_multi_line) return 2;
        }
        else if (!strcmp(a, "--no-marker-multi-line")) marker_multi_line = NULL;
        else if (ARG_EQ("--threads")) {
            if (!parse_cli_nonnegative("--threads", a + 10, &threads)) return 2;
        }
        else if (!strcmp(a, "--threads")) {
            if (i + 1 >= argc) {
                (void)parse_cli_nonnegative("--threads", NULL, &threads);
                return 2;
            }
            if (!parse_cli_nonnegative("--threads", argv[++i], &threads)) return 2;
        }
        else if (!strcmp(a, "--track")) track = true;
        else if (!strcmp(a, "--no-track")) track = false;
        else if (!strcmp(a, "--phony")) phony = true;
        else if (!strcmp(a, "--no-phony")) phony = false;
        else if (!strcmp(a, "--ambidouble")) ambidouble = true;
        else if (!strcmp(a, "--no-ambidouble")) ambidouble = false;
        else if (!strcmp(a, "--bash")) { /* print bash integration */ return 0; }
        else if (!strcmp(a, "--black")) { /* part of the --color palette */ }
        else if (!strcmp(a, "--filepath-word")) filepath_word = true;
        else if (!strcmp(a, "--no-filepath-word")) filepath_word = false;
        else if (!strcmp(a, "--fish")) { /* print fish integration */ return 0; }
        else if (!strcmp(a, "--header-first")) header_first = true;
        else if (!strcmp(a, "--no-header-first")) header_first = false;
        else if (ARG_EQ("--list-label")) list_label = a + strlen("--list-label") + 1;
        else if (!strcmp(a, "--list-label")) {
            if (i + 1 < argc) list_label = argv[++i];
            else { fprintf(stderr, "--list-label requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--list-label-pos")) {
            if (!mm_parse_label_pos(a + strlen("--list-label-pos") + 1, &list_label_pos, &list_label_bottom)) {
                fprintf(stderr, "invalid --list-label-pos: %s\n", a + strlen("--list-label-pos") + 1);
                return 2;
            }
        }
        else if (!strcmp(a, "--list-label-pos")) {
            if (i + 1 < argc) {
                const char *value = argv[++i];
                if (!mm_parse_label_pos(value, &list_label_pos, &list_label_bottom)) {
                    fprintf(stderr, "invalid --list-label-pos: %s\n", value);
                    return 2;
                }
            } else { fprintf(stderr, "--list-label-pos requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--input-label")) input_label = a + strlen("--input-label") + 1;
        else if (!strcmp(a, "--input-label")) {
            if (i + 1 < argc) input_label = argv[++i];
            else { fprintf(stderr, "--input-label requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--input-label-pos")) {
            if (!mm_parse_label_pos(a + strlen("--input-label-pos") + 1, &input_label_pos, &input_label_bottom)) {
                fprintf(stderr, "invalid --input-label-pos: %s\n", a + strlen("--input-label-pos") + 1);
                return 2;
            }
        }
        else if (!strcmp(a, "--input-label-pos")) {
            if (i + 1 < argc) {
                const char *value = argv[++i];
                if (!mm_parse_label_pos(value, &input_label_pos, &input_label_bottom)) {
                    fprintf(stderr, "invalid --input-label-pos: %s\n", value);
                    return 2;
                }
            } else { fprintf(stderr, "--input-label-pos requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--header-label")) header_label = a + strlen("--header-label") + 1;
        else if (!strcmp(a, "--header-label")) {
            if (i + 1 < argc) header_label = argv[++i];
            else { fprintf(stderr, "--header-label requires a value\n"); return 2; }
        }
        else if (ARG_EQ("--header-label-pos")) {
            if (!mm_parse_label_pos(a + strlen("--header-label-pos") + 1, &header_label_pos, &header_label_bottom)) {
                fprintf(stderr, "invalid --header-label-pos: %s\n", a + strlen("--header-label-pos") + 1);
                return 2;
            }
        }
        else if (!strcmp(a, "--header-label-pos")) {
            if (i + 1 < argc) {
                const char *value = argv[++i];
                if (!mm_parse_label_pos(value, &header_label_pos, &header_label_bottom)) {
                    fprintf(stderr, "invalid --header-label-pos: %s\n", value);
                    return 2;
                }
            } else { fprintf(stderr, "--header-label-pos requires a value\n"); return 2; }
        }
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
        else if (ARG_EQ("--input-border")) {
            input_border_shape = mm_border_parse(a + strlen("--input-border") + 1);
            if (input_border_shape < 0) {
                fprintf(stderr, "invalid --input-border style: %s\n", a + strlen("--input-border") + 1);
                return 2;
            }
            input_border_set = true;
        }
        else if (!strcmp(a, "--input-border")) {
            input_border_shape = MM_BORDER_ROUNDED;
            input_border_set = true;
        }
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
    else if (ARG_EQ("--id-nth")) id_nth_spec = a + 9;
    else if (!strcmp(a, "--id-nth")) {
        id_nth_spec = required_cli_arg(argc, argv, &i, "--id-nth");
        if (!id_nth_spec) return 2;
    }
    else if (!strcmp(a, "--no-id-nth")) id_nth_spec = NULL;
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
        else if (!strcmp(a, "--history")) {
            history_spec = required_cli_arg(argc, argv, &i, "--history");
            if (!history_spec) return 2;
        }
        else if (!strcmp(a, "--no-history")) history_spec = NULL;
        else if (ARG_EQ("--history-size")) {
            if (!parse_cli_positive("--history-size", a + 15, &history_size)) return 2;
        }
        else if (!strcmp(a, "--history-size")) {
            if (i + 1 >= argc) {
                (void)parse_cli_positive("--history-size", NULL, &history_size);
                return 2;
            }
            if (!parse_cli_positive("--history-size", argv[++i], &history_size)) return 2;
        }
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
        else if (!strcmp(a, "--preview")) {
            preview_cmd = required_cli_arg(argc, argv, &i, "--preview");
            if (!preview_cmd) return 2;
        }
        else if (!strcmp(a, "--no-preview")) preview_cmd = NULL;
        else if (ARG_EQ("--preview-window")) preview_win_spec = a + 17;
        else if (!strcmp(a, "--preview-window")) {
            preview_win_spec = required_cli_arg(argc, argv, &i, "--preview-window");
            if (!preview_win_spec) return 2;
        }
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
        else if (!strcmp(a, "--prompt")) {
            prompt = required_cli_arg(argc, argv, &i, "--prompt");
            if (!prompt) return 2;
        }
        else if (ARG_EQ("--header")) header = a + 9;
        else if (!strcmp(a, "--header")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "--header requires a value\n");
                return 2;
            }
            header = argv[++i];
        }
        else if (!strcmp(a, "--no-header")) header = NULL;
        else if (ARG_EQ("--header-lines")) {
            if (!parse_nonnegative_int(a + 15, &header_lines)) {
                fprintf(stderr, "invalid --header-lines value: %s\n", a + 15);
                return 2;
            }
        }
        else if (!strcmp(a, "--header-lines")) {
            if (i + 1 >= argc || !parse_nonnegative_int(argv[++i], &header_lines)) {
                fprintf(stderr, "invalid --header-lines value\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--no-header-lines")) header_lines = 0;
        else if (!strcmp(a, "--ansi")) ansi = true;
        else if (!strcmp(a, "--no-ansi")) ansi = false;
        else if (ARG_EQ("--algo")) { const char *al = a + 7; int version = !strcmp(al, "v1") ? 1 : !strcmp(al, "v2") ? 2 : 0; if (!version || !mm_algo_set_version(version)) { fprintf(stderr, "invalid algorithm: %s\n", al); return 2; } }
        else if (!strcmp(a, "--algo")) { const char *al = required_cli_arg(argc, argv, &i, "--algo"); if (!al) return 2; int version = !strcmp(al, "v1") ? 1 : !strcmp(al, "v2") ? 2 : 0; if (!version || !mm_algo_set_version(version)) { fprintf(stderr, "invalid algorithm: %s\n", al); return 2; } }
        else if (!strcmp(a, "--version")) { printf("0.74.3 (c23)\n"); return 0; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { usage(stdout); return 0; }
        else if (a[0] == '-' && a[1] != 0) {
            /* fzf rejects unknown options (PLAN: no silently ignored flags). */
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
        #undef ARG_EQ
    }

    if (accept_nth_spec && !mm_accept_nth_validate(accept_nth_spec)) return 2;
    if (id_nth_spec) {
        mm_range_t *id_ranges = NULL;
        int id_range_count = 0;
        if (!mm_parse_ranges(id_nth_spec, &id_ranges, &id_range_count)) {
            fprintf(stderr, "invalid --id-nth: %s\n", id_nth_spec);
            free(id_ranges);
            return 2;
        }
        free(id_ranges);
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
                bool size_overflow = false;
                for (int i = 0; i < npaths; i++) {
                    size_t path_len = strlen(paths[i]);
                    if (total == SIZE_MAX || path_len > SIZE_MAX - total - 1) {
                        size_overflow = true;
                        break;
                    }
                    total += path_len + 1;
                }
                data = size_overflow ? NULL : malloc(total);
                if (!data) {
                    mm_walk_free(paths, npaths);
                    fprintf(stderr, "fzf: input too large or out of memory\n");
                    goto fail;
                }
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

    if (!data) {
        fprintf(stderr, "fzf: failed to read input or out of memory\n");
        if (have_walk_thread) {
            if (stream_fd >= 0 && stream_fd != STDIN_FILENO) close(stream_fd);
            pthread_join(walk_thread, NULL);
            have_walk_thread = false;
        }
        if (stream_fp) {
            pclose(stream_fp);
            stream_fp = NULL;
        }
        goto fail;
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
            if (cap > INT_MAX / 2 ||
                (size_t)cap * 2u > SIZE_MAX / sizeof(*lines)) {
                free(line);
                fprintf(stderr, "fzf: too many input records\n");
                return 2;
            }
            int next_cap = cap * 2;
            char **nl2 = realloc(lines, (size_t)next_cap * sizeof(*lines));
            if (!nl2) { free(line); fprintf(stderr, "fzf: out of memory\n"); return 2; }
            lines = nl2;
            cap = next_cap;
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

    mm_filter_set_threads(threads);

    if (!filter) {
        /* Interactive mode. */
        mm_ui_t ui;
        memset(&ui, 0, sizeof(ui));
        ui.crit = crit;
        ui.accept_nth_spec = accept_nth_spec;
        ui.id_nth_spec = id_nth_spec;
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
        ui.input_border = input_border_shape;
        ui.input_border_set = input_border_set;
        ui.input_label = input_label;
        ui.input_label_pos = input_label_pos;
        ui.input_label_bottom = input_label_bottom;
        ui.header_border_set = header_border_set;
        ui.style_full = style_full;
        ui.list_label = list_label;
        ui.list_label_pos = list_label_pos;
        ui.list_label_bottom = list_label_bottom;
        ui.header_label = header_label;
        ui.header_label_pos = header_label_pos;
        ui.header_label_bottom = header_label_bottom;
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
        ui.search_enabled = !disabled;
        ui.footer = footer_spec;
        ui.gutter = gutter_spec;
        /* The raw gutter is only used in the raw search mode, which is not
           implemented; the option is accepted like the reference does. */
        (void)gutter_raw_spec;
        ui.no_unicode = no_unicode;
        /* The horizontal scrolling of the preview and the multi-line items
           are not implemented; the options are accepted like the reference
           does. */
        ui.no_hscroll = no_hscroll;
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
        if (have_walk_thread) {
            /* Closing the read end lets a buffered walker notice early UI exit
               instead of blocking forever while the main thread joins it. */
            if (stream_fd >= 0 && stream_fd != STDIN_FILENO) {
                close(stream_fd);
                stream_fd = -1;
            }
            pthread_join(walk_thread, NULL);
        }
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
    if (!out) {
        fprintf(stderr, "fzf: failed to filter input (out of memory)\\n");
        mm_pattern_free(pat);
        mm_delimiter_free(&delim);
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
        return 2;
    }

    char osep = print0 ? '\0' : '\n';
    if (print_query) { fputs(filter, stdout); fputc(osep, stdout); }
    for (int i = 0; i < n; i++) {
        const char *line = lines[out[i]];
        if (accept_nth_spec) {
            char *accepted = mm_accept_nth_expand(accept_nth_spec, line, delim,
                                                out[i]);
            if (!accepted) {
                fprintf(stderr, "fzf: failed to format accepted item (out of memory)\\n");
                free(out);
                mm_pattern_free(pat);
                mm_delimiter_free(&delim);
                free(nth);
                if (wnth != nth) free(wnth);
                for (int j = 0; j < count; j++) free(lines[j]);
                free(lines);
                free(data);
                mm_keymap_free(&keymap);
                for (int j = 0; j < expect_n; j++) free(expect_keys[j]);
                free(expect_keys);
                for (int j = 0; j < nskip; j++) free((char *)skip_names[j]);
                free(skip_names);
                return 2;
            }
            fputs(accepted, stdout);
            free(accepted);
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
