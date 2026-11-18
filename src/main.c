/* main.c - fzf-compatible command-line entry point for the C23 rewrite. */
#include "algo.h"
#include "pattern.h"
#include "result.h"
#include "filter.h"
#include "ui.h"
#include "walker.h"
#include "preview.h"
#include "history.h"
#include "tmux.h"
#include "color.h"
#include "tokenizer.h"
#include "ansi.h"
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
#include <ctype.h>
#include <errno.h>
#include <time.h>
#include <sys/wait.h>

static void usage(FILE *out) {
    static const char *const text[] = {
        "fzf is an interactive filter program for any kind of list.\n\nIt implements a \"fuzzy\" matching algorithm, so you can quickly type in patterns\nwith omitted characters and still get the results you want.\n\nProject URL: https://github.com/junegunn/fzf\nAuthor: Junegunn Choi <junegunn.c@gmail.com>\n\n* See man page for more information: fzf --man\n\nUsage: fzf [options]\n\n  SEARCH\n    -e, --exact              Enable exact-match\n    +x, --no-extended        Disable extended-search mode\n    -i, --ignore-case        Case-insensitive match\n    +i, --no-ignore-case     Case-sensitive match\n        --smart-case         Smart-case match (default)\n    --scheme=SCHEME          Scoring scheme [default|path|history]\n    -n, --nth=N[,..]         Comma-separated list of field index expressions\n                             for limiting search scope. Each can be a non-zero\n                             integer or a range expression ([BEGIN]..[END]).\n    --with-nth=N[,..]        Transform the presentation of each line using\n                             field index expressions\n    --accept-nth=N[,..]      Define which fields to print on accept\n    -d, --delimiter=STR      Field delimiter regex (default: AWK-style)\n    +s, --no-sort            Do not sort the result\n    --literal                Do not normalize latin script letters\n    --tail=NUM               Maximum number of items to keep in memory\n    --disabled               Do not perform search\n    --tiebreak=CRI[,..]      Comma-separated list of sort criteria to apply\n                             when the scores are tied\n                             [length|chunk|pathname|begin|end|index] (default: length)\n\n  INPUT/OUTPUT\n    --read0                  Read input delimited by ASCII NUL characters\n    --print0                 Print output delimited by ASCII NUL characters\n    --ansi                   Enable processing of ANSI color codes\n    --sync                   Synchronous search for multi-staged f",
        "iltering\n\n  GLOBAL STYLE\n    --style=PRESET           Apply a style preset [default|minimal|full[:BORDER_STYLE]\n    --color=COLSPEC          Base scheme (dark|light|base16|bw) and/or custom colors\n    --no-color               Disable colors\n    --no-bold                Do not use bold text\n\n  DISPLAY MODE\n    --height=[~][-]HEIGHT[%] Display fzf window below the cursor with the given\n                             height instead of using fullscreen.\n                             A negative value is calculated as the terminal height\n                             minus the given value.\n                             If prefixed with '~', fzf will determine the height\n                             according to the input size.\n    --min-height=HEIGHT[+]   Minimum height when --height is given as a percentage.\n                             Add '+' to automatically increase the value\n                             according to the other layout options (default: 10+).\n    --popup[=OPTS]           Start fzf in a floating pane (requires tmux 3.3+ or Zellij 0.44+)\n                             [center|top|bottom|left|right][,SIZE[%]][,SIZE[%]]\n                             [,border-native] (default: center,50%)\n    --tmux[=OPTS]            Alias for --popup\n\n  LAYOUT\n    --layout=LAYOUT          Choose layout: [default|reverse|reverse-list]\n    --margin=MARGIN          Screen margin (TRBL | TB,RL | T,RL,B | T,R,B,L)\n    --padding=PADDING        Padding inside border (TRBL | TB,RL | T,RL,B | T,R,B,L)\n    --border[=STYLE]         Draw border around the finder\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|line|none] (default: rounded)\n    --border-label=LABEL     Label to print on the border\n    --border-label-pos=COL   Position of the border label\n                             [POSITIVE_INTEGER: columns from left|\n                              NEGATIVE_I",
        "NTEGER: columns from right][:bottom]\n                             (default: 0 or center)\n\n  LIST SECTION\n    -m, --multi[=MAX]        Enable multi-select with tab/shift-tab\n    --highlight-line         Highlight the whole current line\n    --cycle                  Enable cyclic scroll\n    --wrap[=MODE]            Enable line wrap (char|word, default: char)\n    --wrap-sign=STR          Indicator for wrapped lines\n    --no-multi-line          Disable multi-line display of items when using --read0\n    --raw                    Enable raw mode (show non-matching items)\n    --track                  Track the current selection when the result is updated\n    --id-nth=N[,..]          Define item identity fields for cross-reload operations\n    --tac                    Reverse the order of the input\n    --gap[=N]                Render empty lines between each item\n    --gap-line[=STR]         Draw horizontal line on each gap using the string\n                             (default: '┈' or '-')\n    --freeze-left=N          Number of fields to freeze on the left\n    --freeze-right=N         Number of fields to freeze on the right\n    --keep-right             Keep the right end of the line visible on overflow\n    --scroll-off=LINES       Number of screen lines to keep above or below when\n                             scrolling to the top or to the bottom (default: 0)\n    --no-hscroll             Disable horizontal scroll\n    --hscroll-off=COLS       Number of screen columns to keep to the right of the\n                             highlighted substring (default: 10)\n    --jump-labels=CHARS      Label characters for jump mode\n    --gutter=CHAR            Character used for the gutter column (default: '▌')\n    --gutter-raw=CHAR        Character used for the gutter column in raw mode (default: '▖')\n    --pointer=STR            Pointer to the current line (default: '▌' or '>')\n    --marker=STR             Multi-select marker (default: '┃' or '>')\n    --marker",
        "-multi-line=STR  Multi-select marker for multi-line entries;\n                             3 elements for top, middle, and bottom (default: '╻┃╹')\n    --ellipsis=STR           Ellipsis to show when line is truncated (default: '··')\n    --tabstop=SPACES         Number of spaces for a tab character (default: 8)\n    --scrollbar[=C1[C2]]     Scrollbar character(s)\n                             (each for list section and preview window)\n    --no-scrollbar           Hide scrollbar\n    --list-border[=STYLE]    Draw border around the list section\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|none] (default: rounded)\n    --list-label=LABEL       Label to print on the list border\n    --list-label-pos=COL     Position of the list label\n                             [POSITIVE_INTEGER: columns from left|\n                              NEGATIVE_INTEGER: columns from right][:bottom]\n                             (default: 0 or center)\n\n  INPUT SECTION\n    --no-input               Disable and hide the input section\n    --prompt=STR             Input prompt (default: '> ')\n    --info=STYLE             Finder info style\n                             [default|right|hidden|inline[-right][:PREFIX]]\n    --info-command=COMMAND   Command to generate info line\n    --separator=STR          Draw horizontal separator on info line using the string\n                             (default: '─' or '-')\n    --no-separator           Hide info line separator\n    --ghost=TEXT             Ghost text to display when the input is empty\n    --filepath-word          Make word-wise movements respect path separators\n    --input-border[=STYLE]   Draw border around the input section\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|line|none] (default: rounded)\n    --input-label=LABEL",
        "      Label to print on the input border\n    --input-label-pos=COL    Position of the input label\n                             [POSITIVE_INTEGER: columns from left|\n                              NEGATIVE_INTEGER: columns from right][:bottom]\n                             (default: 0 or center)\n\n  PREVIEW WINDOW\n    --preview=COMMAND        Command to preview highlighted line ({})\n    --preview-window=OPT     Preview window layout (default: right:50%)\n                             [up|down|left|right|next][,SIZE[%]]\n                             [,[no]wrap[-word]][,[no]cycle][,[no]follow][,[no]info]\n                             [,[no]hidden][,border-STYLE]\n                             [,+SCROLL[OFFSETS][/DENOM]][,~HEADER_LINES]\n                             [,default][,<SIZE_THRESHOLD(ALTERNATIVE_LAYOUT)]\n    --preview-border[=STYLE] Short for --preview-window=border-STYLE\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|line|none] (default: rounded)\n    --preview-label=LABEL\n    --preview-label-pos=N    Same as --border-label and --border-label-pos,\n                             but for preview window\n    --preview-wrap-sign=STR  Indicator for wrapped lines in the preview window\n\n  HEADER\n    --header=STR             String to print as header\n    --header-lines=N         The first N lines of the input are treated as header\n    --header-first           Print header before the prompt line\n    --header-border[=STYLE]  Draw border around the header section\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|line|inline|none] (default: rounded)\n    --header-lines-border[=STYLE]\n                             Display header from --header-lines with a separate border.\n                             Pass 'none' to still separate it but without a border.",
        "\n                             Pass 'inline' to embed it inside the list frame.\n    --header-label=LABEL     Label to print on the header border\n    --header-label-pos=COL   Position of the header label\n                             [POSITIVE_INTEGER: columns from left|\n                              NEGATIVE_INTEGER: columns from right][:bottom]\n                             (default: 0 or center)\n\n  FOOTER\n    --footer=STR             String to print as footer\n    --footer-border[=STYLE]  Draw border around the footer section\n                             [rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|\n                              top|bottom|left|right|line|inline|none] (default: line)\n    --footer-label=LABEL     Label to print on the footer border\n    --footer-label-pos=COL   Position of the footer label\n                             [POSITIVE_INTEGER: columns from left|\n                              NEGATIVE_INTEGER: columns from right][:bottom]\n                             (default: 0 or center)\n\n  SCRIPTING\n    -q, --query=STR          Start the finder with the given query\n    -1, --select-1           Automatically select the only match\n    -0, --exit-0             Exit immediately when there's no match\n    -f, --filter=STR         Print matches for the initial query and exit\n    --print-query            Print query as the first line\n    --expect=KEYS            Comma-separated list of keys to complete fzf\n\n  KEY/EVENT BINDING\n    --bind=BINDINGS          Custom key/event bindings\n\n  ADVANCED\n    --with-shell=STR         Shell command and flags to start child processes with\n    --listen[=[ADDR:]PORT]   Start HTTP server to receive actions via TCP\n                             (To allow remote process execution, use --listen-unsafe)\n    --listen=SOCKET_PATH     Start HTTP server to receive actions via Unix domain socket\n                             (Path should end with .sock)\n\n  DIRECTORY TRAVERSAL        ",
        "(Only used when $FZF_DEFAULT_COMMAND is not set)\n    --walker=OPTS            [file][,dir][,follow][,hidden] (default: file,follow,hidden)\n    --walker-root=DIR [...]  List of directories to walk (default: .)\n    --walker-skip=DIRS       Comma-separated list of directory names to skip\n                             (default: .git,node_modules)\n\n  HISTORY\n    --history=FILE           File to store fzf search history (*not* shell command history)\n    --history-size=N         Maximum number of entries to keep in the file (default: 1000)\n\n  SHELL INTEGRATION\n    --bash                   Print script to set up Bash shell integration\n\n  HELP\n    --version                Display version information and exit\n    --help                   Show this message\n    --man                    Show man page\n\n  ENVIRONMENT VARIABLES\n    FZF_DEFAULT_COMMAND      Default command to use when input is tty\n    FZF_DEFAULT_OPTS         Default options (e.g. '--layout=reverse --info=inline')\n    FZF_DEFAULT_OPTS_FILE    Location of the file to read default options from\n    FZF_API_KEY              X-API-Key header for HTTP server (--listen)\n\n",
    };
    for (size_t i = 0; i < sizeof(text) / sizeof(text[0]); i++) {
        fputs(text[i], out);
    }
}

/* fzf nextInt + the post-check for --freeze-left / --freeze-right.
   Invalid text is "number of fields required"; a negative value is reported
   on its own. "-0" is zero. */
static int parse_freeze_count(const char *value, int *out) {
    if (!value || !*value) return 1;
    const char *p = value;
    int sign = 1;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1;
        p++;
    }
    if (!*p) return 1;
    long n = 0;
    for (; *p; p++) {
        if (*p < '0' || *p > '9') return 1;
        if (n > (INT_MAX - (*p - '0')) / 10) return 1;
        n = n * 10 + (*p - '0');
    }
    if (sign < 0 && n > 0) return 2;
    *out = (int)n;
    return 0;
}

static bool take_freeze_count(int argc, char **argv, int *index,
                              const char *arg, const char *name, int *out) {
    const char *value = NULL;
    size_t nlen = strlen(name);
    if (strlen(arg) > nlen && arg[nlen] == '=') value = arg + nlen + 1;
    else if (*index + 1 < argc) value = argv[++*index];
    int rc = parse_freeze_count(value, out);
    if (rc == 0) return true;
    if (rc == 2)
        fprintf(stderr, "number of fields to freeze must be a non-negative integer\n");
    else
        fprintf(stderr, "number of fields required\n");
    return false;
}

/* Go time.ParseDuration, used by --bench. Units: ns, us, µs, μs, ms, s, m, h. */
static bool parse_go_duration(const char *s, int64_t *out_ns) {
    if (!s) return false;
    bool neg = false;
    if (*s == '+' || *s == '-') {
        neg = *s == '-';
        s++;
    }
    if (strcmp(s, "0") == 0) { *out_ns = 0; return true; }
    if (!*s) return false;
    uint64_t total = 0;
    while (*s) {
        if (*s != '.' && (*s < '0' || *s > '9')) return false;
        uint64_t v = 0;
        bool pre = false;
        while (*s >= '0' && *s <= '9') {
            pre = true;
            if (v > UINT64_MAX / 10) return false;
            v = v * 10 + (uint64_t)(*s - '0');
            s++;
        }
        uint64_t frac = 0;
        double scale = 1;
        bool post = false;
        if (*s == '.') {
            s++;
            while (*s >= '0' && *s <= '9') {
                post = true;
                if (frac <= (UINT64_MAX - 9) / 10) {
                    frac = frac * 10 + (uint64_t)(*s - '0');
                    scale *= 10.0;
                }
                s++;
            }
        }
        if (!pre && !post) return false;
        const char *unit_s = s;
        if ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xB5) s += 2;
        else if ((unsigned char)s[0] == 0xCE && (unsigned char)s[1] == 0xBC) s += 2;
        while (*s && *s != '.' && !(*s >= '0' && *s <= '9')) s++;
        size_t ulen = (size_t)(s - unit_s);
        uint64_t unit = 0;
        if (ulen == 2 && unit_s[0] == 'n' && unit_s[1] == 's') unit = 1;
        else if (ulen == 2 && unit_s[0] == 'u' && unit_s[1] == 's') unit = 1000;
        else if (ulen == 2 && unit_s[0] == 'm' && unit_s[1] == 's') unit = 1000000ULL;
        else if (ulen == 1 && unit_s[0] == 's') unit = 1000000000ULL;
        else if (ulen == 1 && unit_s[0] == 'm') unit = 60000000000ULL;
        else if (ulen == 1 && unit_s[0] == 'h') unit = 3600000000000ULL;
        else if (ulen == 3 && (unsigned char)unit_s[0] == 0xC2 && unit_s[2] == 's')
            unit = 1000;
        else if (ulen == 3 && (unsigned char)unit_s[0] == 0xCE && unit_s[2] == 's')
            unit = 1000;
        else return false;
        if (unit != 0 && v > UINT64_MAX / unit) return false;
        uint64_t add = v * unit;
        if (frac > 0 && unit > 0)
            add += (uint64_t)((double)frac * ((double)unit / scale));
        if (total > UINT64_MAX - add) return false;
        total += add;
        if (total > (uint64_t)INT64_MAX) return false;
    }
    *out_ns = neg ? -(int64_t)total : (int64_t)total;
    return true;
}

static char *read_whole_file(const char *path, size_t *out_len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long sz = ftell(fp);
    if (sz < 0) { fclose(fp); return NULL; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(fp); return NULL; }
    size_t n = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[n] = 0;
    if (out_len) *out_len = n;
    return buf;
}

static void print_labeled_script(const char *label, const char *text, size_t n) {
    size_t begin = 0, end = n;
    while (begin < end && isspace((unsigned char)text[begin])) begin++;
    while (end > begin && isspace((unsigned char)text[end - 1])) end--;
    printf("### %s ###\n", label);
    if (end > begin) fwrite(text + begin, 1, end - begin, stdout);
    printf("\n### end: %s ###\n", label);
}

/* fzf printScript: the scripts live next to the binary, under shell/. */
static int print_bash_integration(void) {
    char exe[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n < 0) {
        fprintf(stderr, "fzf: failed to read bash integration\n");
        return 2;
    }
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) {
        fprintf(stderr, "fzf: failed to read bash integration\n");
        return 2;
    }
    *slash = 0;
    const char *files[][2] = {
        { "key-bindings.bash", "shell/key-bindings.bash" },
        { "completion.bash", "shell/completion.bash" },
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        char path[PATH_MAX];
        int wrote = snprintf(path, sizeof(path), "%s/%s", exe, files[i][1]);
        if (wrote < 0 || (size_t)wrote >= sizeof(path)) {
            fprintf(stderr, "fzf: failed to read bash integration\n");
            return 2;
        }
        size_t len = 0;
        char *text = read_whole_file(path, &len);
        if (!text) {
            fprintf(stderr, "fzf: failed to read bash integration\n");
            return 2;
        }
        print_labeled_script(files[i][0], text, len);
        free(text);
    }
    return 0;
}

static void theme_set_nocolor(mm_theme_t *theme) {
    mm_color_attr_t *fields = (mm_color_attr_t *)theme;
    size_t n = sizeof(*theme) / sizeof(*fields);
    for (size_t i = 0; i < n; i++) {
        fields[i].color = MM_COL_DEFAULT;
        fields[i].attr = MM_ATTR_UNDEFINED;
    }
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

static bool parse_cli_signed(const char *option, const char *value,
                                int *out) {
    if (!value || !*value) {
        fprintf(stderr, "invalid %s value: %s\n", option,
                value ? value : "");
        return false;
    }
    char *end = NULL;
    long parsed = strtol(value, &end, 10);
    if (end == value || *end != '\0' ||
        parsed < INT_MIN || parsed > INT_MAX) {
        fprintf(stderr, "invalid %s value: %s\n", option, value);
        return false;
    }
    *out = (int)parsed;
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
        "cfzf.1",
        "/usr/local/share/man/man1/fzf.1",
        "/usr/share/man/man1/fzf.1",
        "/usr/local/share/man/man1/cfzf.1",
        "/usr/share/man/man1/cfzf.1",
        NULL
    };
    for (int i = 0; candidates[i]; i++) {
        if (access(candidates[i], R_OK) != 0) continue;

        pid_t child = fork();
        if (child == 0) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                (void)dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
            char relative[PATH_MAX];
            const char *argument = candidates[i];
            if (argument[0] != '/') {
                int written = snprintf(relative, sizeof(relative), "./%s", argument);
                if (written > 0 && (size_t)written < sizeof(relative))
                    argument = relative;
            }
            execlp("man", "man", argument, (char *)NULL);
            _exit(127);
        }
        if (child > 0) {
            int status = 0;
            pid_t waited;
            do {
                waited = waitpid(child, &status, 0);
            } while (waited < 0 && errno == EINTR);
            if (waited == child && WIFEXITED(status) &&
                WEXITSTATUS(status) == 0) return 0;
        }

        /* Match fzf's fallback when man is unavailable or fails. */
        FILE *f = fopen(candidates[i], "r");
        if (!f) continue;
        int ch;
        while ((ch = fgetc(f)) != EOF) fputc(ch, stdout);
        fclose(f);
        return 0;
    }
    fprintf(stderr,
            "fzf: manual page not found; see fzf(1) in the documentation\n");
    return 1;
}

/* Remove terminal control sequences from a record for --ansi matching.
   The original record is retained separately for output, matching fzf's
   distinction between searchable text and the line returned to the caller. */
static char *strip_ansi_record(const char *line) {
    if (!line) return NULL;
    size_t len = strlen(line);
    char *out = malloc(len + 1);
    if (!out) return NULL;

    size_t pos = 0, used = 0;
    while (pos < len) {
        int start = -1, end = -1;
        mm_next_ansi_escape_sequence(line + pos, len - pos, &start, &end);
        if (start < 0 || end <= start) {
            size_t rest = len - pos;
            memcpy(out + used, line + pos, rest);
            used += rest;
            break;
        }
        size_t plain = (size_t)start;
        if (plain) {
            memcpy(out + used, line + pos, plain);
            used += plain;
        }
        pos += (size_t)end;
    }
    out[used] = '\0';
    return out;
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
    /* Drop the read-ahead slack. Batch mode aliases its lines into this
       buffer, so the slack would stay resident for the whole filter. */
    if (len + 1 < cap) {
        char *shrunk = realloc(buf, len + 1);
        if (shrunk) buf = shrunk;
    }
    *outlen = len;
    return buf;
}

/* owned is false when each line points into the input buffer. */
static void release_lines(char **lines, int count, bool owned) {
    if (owned && lines) {
        for (int i = 0; i < count; i++) free(lines[i]);
    }
    free(lines);
}

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} mm_cli_words_t;

static char **mm_cli_default_words;
static size_t mm_cli_default_word_count;
static char **mm_cli_expanded_argv;

static void mm_cli_defaults_cleanup(void) {
    for (size_t i = 0; i < mm_cli_default_word_count; i++)
        free(mm_cli_default_words[i]);
    free(mm_cli_default_words);
    free(mm_cli_expanded_argv);
    mm_cli_default_words = NULL;
    mm_cli_default_word_count = 0;
    mm_cli_expanded_argv = NULL;
}

static void mm_cli_words_free(mm_cli_words_t *words) {
    if (!words) return;
    for (size_t i = 0; i < words->count; i++) free(words->items[i]);
    free(words->items);
    memset(words, 0, sizeof(*words));
}

static bool mm_cli_words_push(mm_cli_words_t *words, char *word) {
    if (words->count == words->capacity) {
        size_t next = words->capacity ? words->capacity * 2 : 8;
        if (next < words->capacity || next > SIZE_MAX / sizeof(*words->items))
            return false;
        char **grown = realloc(words->items, next * sizeof(*words->items));
        if (!grown) return false;
        words->items = grown;
        words->capacity = next;
    }
    words->items[words->count++] = word;
    return true;
}

static bool mm_cli_word_append(char **word, size_t *length, size_t *capacity,
                               char ch) {
    if (*capacity == 0 || *length >= *capacity - 1) {
        size_t next = *capacity ? *capacity * 2 : 32;
        if (next < *capacity || next > SIZE_MAX - 1) return false;
        char *grown = realloc(*word, next);
        if (!grown) return false;
        *word = grown;
        *capacity = next;
    }
    (*word)[(*length)++] = ch;
    return true;
}

/* Parse the quoting and escaping used by fzf's default-option strings.
   Shell expansion is intentionally not performed. */
static bool mm_parse_shell_words(const char *input, mm_cli_words_t *words,
                                 const char **error) {
    char *word = NULL;
    size_t length = 0, capacity = 0;
    char quote = 0;
    bool escaped = false, in_word = false, comment = false;

    if (error) *error = NULL;
    if (!input || !words) return false;
    for (size_t i = 0;; i++) {
        char ch = input[i];
        if (comment) {
            if (ch == '\0') break;
            if (ch == '\n') comment = false;
            else continue;
        }
        if (escaped) {
            if (ch == '\0') {
                if (error) *error = "trailing escape";
                goto fail;
            }
            if (!mm_cli_word_append(&word, &length, &capacity, ch))
                goto oom;
            escaped = false;
            in_word = true;
            continue;
        }
        if (quote == '\'') {
            if (ch == '\0') {
                if (error) *error = "unclosed single quote";
                goto fail;
            }
            if (ch == '\'') quote = 0;
            else if (!mm_cli_word_append(&word, &length, &capacity, ch))
                goto oom;
            in_word = true;
            continue;
        }
        if (quote == '"') {
            if (ch == '\0') {
                if (error) *error = "unclosed double quote";
                goto fail;
            }
            if (ch == '"') quote = 0;
            else if (ch == '\\') escaped = true;
            else if (!mm_cli_word_append(&word, &length, &capacity, ch))
                goto oom;
            in_word = true;
            continue;
        }
        if (ch == '\0' || isspace((unsigned char)ch)) {
            if (in_word) {
                if (!word) {
                    word = malloc(1);
                    if (!word) goto oom;
                }
                word[length] = '\0';
                if (!mm_cli_words_push(words, word)) goto oom;
                word = NULL;
                length = capacity = 0;
                in_word = false;
            }
            if (ch == '\0') break;
            continue;
        }
        if (ch == '#' && !in_word) {
            comment = true;
            continue;
        }
        if (ch == '\\') {
            escaped = true;
            in_word = true;
            continue;
        }
        if (ch == '\'' || ch == '"') {
            quote = ch;
            in_word = true;
            continue;
        }
        if (!mm_cli_word_append(&word, &length, &capacity, ch)) goto oom;
        in_word = true;
    }
    free(word);
    return true;

oom:
    if (error) *error = "out of memory";
fail:
    free(word);
    return false;
}

static char *mm_read_text_file(const char *path, const char **error) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        if (error) *error = "cannot open options file";
        return NULL;
    }
    size_t capacity = 4096, length = 0;
    char *text = malloc(capacity);
    if (!text) {
        fclose(file);
        if (error) *error = "out of memory";
        return NULL;
    }
    for (;;) {
        if (capacity - length < 4097) {
            if (capacity > SIZE_MAX / 2) {
                free(text);
                fclose(file);
                if (error) *error = "options file too large";
                return NULL;
            }
            size_t next_capacity = capacity * 2;
            char *grown = realloc(text, next_capacity);
            if (!grown) {
                free(text);
                fclose(file);
                if (error) *error = "out of memory";
                return NULL;
            }
            text = grown;
            capacity = next_capacity;
        }
        size_t n = fread(text + length, 1, 4096, file);
        length += n;
        if (n < 4096) {
            if (ferror(file)) {
                free(text);
                fclose(file);
                if (error) *error = "failed reading options file";
                return NULL;
            }
            break;
        }
    }
    fclose(file);
    text[length] = '\0';
    return text;
}

static bool mm_expand_default_options(int *argc, char ***argv) {
    mm_cli_words_t words = {0};
    const char *error = NULL;
    const char *path = getenv("FZF_DEFAULT_OPTS_FILE");
    if (path && *path) {
        char *contents = mm_read_text_file(path, &error);
        if (!contents) {
            fprintf(stderr, "fzf: $FZF_DEFAULT_OPTS_FILE (%s): %s\n",
                    path, error ? error : "read failed");
            return false;
        }
        bool ok = mm_parse_shell_words(contents, &words, &error);
        free(contents);
        if (!ok) {
            fprintf(stderr, "fzf: %s: %s\n", path,
                    error ? error : "invalid options");
            mm_cli_words_free(&words);
            return false;
        }
    }

    const char *environment = getenv("FZF_DEFAULT_OPTS");
    if (environment && *environment &&
        !mm_parse_shell_words(environment, &words, &error)) {
        fprintf(stderr, "fzf: $FZF_DEFAULT_OPTS: %s\n",
                error ? error : "invalid options");
        mm_cli_words_free(&words);
        return false;
    }
    if (words.count == 0) {
        mm_cli_words_free(&words);
        return true;
    }
    if (words.count > (size_t)(INT_MAX - *argc) ||
        words.count + (size_t)*argc > SIZE_MAX / sizeof(char *) - 1) {
        fprintf(stderr, "fzf: too many default options\n");
        mm_cli_words_free(&words);
        return false;
    }

    size_t new_argc = words.count + (size_t)*argc;
    char **expanded = malloc((new_argc + 1) * sizeof(*expanded));
    if (!expanded) {
        fprintf(stderr, "fzf: out of memory\n");
        mm_cli_words_free(&words);
        return false;
    }
    expanded[0] = (*argv)[0];
    for (size_t i = 0; i < words.count; i++)
        expanded[i + 1] = words.items[i];
    for (int i = 1; i < *argc; i++)
        expanded[words.count + (size_t)i] = (*argv)[i];
    expanded[new_argc] = NULL;

    mm_cli_default_words = words.items;
    mm_cli_default_word_count = words.count;
    mm_cli_expanded_argv = expanded;
    if (atexit(mm_cli_defaults_cleanup) != 0) {
        mm_cli_defaults_cleanup();
        fprintf(stderr, "fzf: failed to register option cleanup\n");
        return false;
    }
    *argc = (int)new_argc;
    *argv = expanded;
    return true;
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

/* Later --bind options add to the earlier ones. The same key in a later
   spec replaces the earlier action when the map is parsed with replace. */
static bool bind_spec_append(char **spec, const char *value) {
    size_t n = strlen(value);
    size_t old = *spec ? strlen(*spec) : 0;
    size_t extra = old ? 1 : 0;
    char *next = realloc(*spec, old + extra + n + 1);
    if (!next) return false;
    if (extra) next[old] = ',';
    memcpy(next + old + extra, value, n + 1);
    *spec = next;
    return true;
}

static bool mm_parse_info_style(const char *value, bool *info,
                                  bool *inline_right, bool *inline_info,
                                  const char **prefix) {
    if (!value || !info || !inline_right || !inline_info) return false;
    if (prefix) *prefix = NULL;
    if (!strcmp(value, "hidden")) {
        *info = false;
        *inline_right = false;
        *inline_info = false;
        return true;
    }
    /* inline-right is checked before inline; the shorter name is a prefix. */
    if (!strcmp(value, "inline-right") ||
        !strncmp(value, "inline-right:", 13)) {
        *info = true;
        *inline_right = true;
        *inline_info = false;
        if (prefix && value[12] == ':') *prefix = value + 13;
        return true;
    }
    if (!strcmp(value, "inline") || !strncmp(value, "inline:", 7)) {
        *info = true;
        *inline_right = false;
        *inline_info = true;
        if (prefix && value[6] == ':') *prefix = value + 7;
        return true;
    }
    if (!strcmp(value, "default") || !strcmp(value, "right")) {
        *info = true;
        *inline_right = false;
        *inline_info = false;
        return true;
    }
    return false;
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

static const char border_style_error[] =
    "invalid border style (expected: rounded|sharp|bold|block|thinblock|double|dashed|horizontal|vertical|top|bottom|left|right|line|inline|none)";

/* fzf splits a label position on commas and colons and lowercases it. */
static bool mm_parse_label_pos_fzf(const char *s, int *pos, bool *bottom,
                                   const char **err) {
    static char emsg[160];
    char buf[128];
    size_t n = strlen(s);
    if (n >= sizeof(buf)) {
        snprintf(emsg, sizeof(emsg), "not a valid integer: %s", s);
        *err = emsg;
        return false;
    }
    for (size_t i = 0; i < n; i++)
        buf[i] = (char)tolower((unsigned char)s[i]);
    buf[n] = 0;
    *pos = 0;
    *bottom = false;
    char *cursor = buf;
    if (*cursor == 0) {
        snprintf(emsg, sizeof(emsg), "not a valid integer: %s", s);
        *err = emsg;
        return false;
    }
    while (*cursor) {
        char *start = cursor;
        while (*cursor && *cursor != ',' && *cursor != ':') cursor++;
        char saved = *cursor;
        *cursor = 0;
        if (!strcmp(start, "center")) *pos = 0;
        else if (!strcmp(start, "bottom")) *bottom = true;
        else if (!strcmp(start, "top")) *bottom = false;
        else {
            char *end = NULL;
            long v = strtol(start, &end, 10);
            if (end == start || (end && *end) || v < INT_MIN || v > INT_MAX) {
                snprintf(emsg, sizeof(emsg), "not a valid integer: %s", start);
                *err = emsg;
                return false;
            }
            *pos = (int)v;
        }
        if (!saved) break;
        cursor++;
    }
    return true;
}

/* 1 = parsed, 0 = not this option, -1 = this option with a bad value.
   A bare option selects the rounded default. A following argument is
   consumed only when it does not look like another flag. */
static int take_component_border(int argc, char **argv, int *i, const char *a,
                                 const char *name, int *shape) {
    size_t nlen = strlen(name);
    const char *spec = NULL;
    bool has = false;
    if (strncmp(a, name, nlen) == 0 && a[nlen] == '=') {
        spec = a + nlen + 1;
        has = true;
    } else if (!strcmp(a, name)) {
        if (*i + 1 < argc && argv[*i + 1][0] != '-' && argv[*i + 1][0] != '+') {
            spec = argv[++*i];
            has = true;
        }
    } else {
        return 0;
    }
    if (!has) {
        *shape = MM_BORDER_ROUNDED;
        return 1;
    }
    int b = mm_border_parse_component(spec);
    if (b < 0 || !spec || !*spec) {
        fprintf(stderr, "%s\n", border_style_error);
        return -1;
    }
    *shape = b;
    return 1;
}


int main(int argc, char **argv) {
    setlocale(LC_ALL, "");
    if (!mm_expand_default_options(&argc, &argv)) return 2;
    if (!mm_algo_init("default")) { fprintf(stderr, "algo init failed\n"); return 2; }

    const char *filter = NULL;
    const char *query = NULL;
    const char *scheme = "default";
    bool scheme_explicit = false;
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
    bool info_inline = false;       /* --info=inline */
    bool info_inline_right = false; /* --info=inline-right */
    const char *info_prefix = NULL;
    const char *style_spec = NULL;
    bool style_full = false;
    bool select_1 = false;
    bool exit_0 = false;
    bool expect = false;
    const char *expect_spec = NULL;
    char *bind_spec = NULL;
    const char *color_spec = NULL;
    int border_shape = -1;          /* -1 until --border; fzf's BorderUndefined */
    int popup_border = -1;          /* border as --tmux sees it, before style */
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
    const char *footer_label = NULL;
    int footer_label_pos = 0;
    bool footer_label_bottom = false;
    int footer_border = MM_BORDER_LINE;
    bool footer_border_set = false;
    int header_lines_border = MM_BORDER_NONE;
    bool header_lines_border_set = false;
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
    bool color_base_nocolor = false;
    bool show_bash = false;
    int64_t bench_ns = 0;
    bool keep_right = false;
    bool no_mouse = false;
    bool sync = false;
    int  min_height = 0;
    bool min_height_set = false;
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
    bool margin_default = true;
    int  height_index = 0;
    mm_popup_opts_t popup = {0};
    bool force_tty_in = false;
    const char *proxy_script = NULL;
    int  scroll_off = 3;    /* fzf's default (src/options.go) */
    int  hscroll_off = 10;  /* fzf's default (src/options.go) */
    int  freeze_left = 0;
    int  freeze_right = 0;
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
    int header_lines = 0;
    const char *footer_spec = NULL;
    const char *gutter_spec = NULL;
    const char *gutter_raw_spec = NULL;
    bool ansi = false;
    bool literal = false;

    /* fzf defaults: extended ON, fuzzy ON */
    extended = true;
    bool show_help = false;
    bool show_version = false;
    bool show_man = false;

    {
        const char *no_color_env = getenv("NO_COLOR");
        if (no_color_env && *no_color_env) {
            no_color = true;
            color_base_nocolor = true;
        }
    }
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (!strcmp(a, "--")) {
            /* fzf ignores a bare "--": it does not end option parsing. */
            continue;
        }
        if (a[0] != '-' && a[0] != '+') {
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
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
        else if (!strcmp(a, "--extended-exact")) { exact = true; extended = true; }
        else if (!strcmp(a, "--no-exact") || !strcmp(a, "+e")) exact = false;
        else if (!strcmp(a, "--extended") || !strcmp(a, "-x")) extended = true;
        else if (!strcmp(a, "--no-extended") || !strcmp(a, "+x")) extended = false;
        else if (!strcmp(a, "--no-sort") || !strcmp(a, "+s")) no_sort = true;
        else if (ARG_EQ("--sort")) {
            int sort_value;
            if (!parse_cli_signed("--sort", a + 7, &sort_value)) return 2;
            no_sort = sort_value <= 0;
        }
        else if (ARG_EQ("-s")) {
            int sort_value;
            if (!parse_cli_signed("-s", a + 3, &sort_value)) return 2;
            no_sort = sort_value <= 0;
        }
        else if (!strcmp(a, "--sort") || !strcmp(a, "-s")) {
            no_sort = false;
            if (i + 1 < argc && argv[i + 1][0] >= '0' &&
                argv[i + 1][0] <= '9') {
                int sort_value;
                if (!parse_cli_nonnegative("--sort", argv[++i], &sort_value))
                    return 2;
                no_sort = sort_value == 0;
            }
        }
        else if (!strcmp(a, "--tac")) tac = true;
        else if (!strcmp(a, "--no-tac")) tac = false;
        else if (!strcmp(a, "--print-query")) print_query = true;
        else if (!strcmp(a, "--no-print-query")) print_query = false;
        else if (!strcmp(a, "--ignore-case") || !strcmp(a, "-i")) case_mode = CASE_IGNORE;
        else if (!strcmp(a, "+i") || !strcmp(a, "--no-ignore-case")) case_mode = CASE_RESPECT;
        else if (!strcmp(a, "--smart-case")) case_mode = CASE_SMART;
        else if (ARG_EQ("--scheme")) { scheme = a + 9; scheme_explicit = true; }
        else if (!strcmp(a, "--scheme")) {
            scheme = required_cli_arg(argc, argv, &i, "--scheme");
            if (!scheme) return 2;
            scheme_explicit = true;
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
        else if (!strncmp(a, "-n", 2) && a[2] != 0 && a[1] != '-') { nth_spec = a + 2; }
        else if (!strcmp(a, "-n")) {
            nth_spec = required_cli_arg(argc, argv, &i, "-n");
            if (!nth_spec) return 2;
        }
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
        else if (!strcmp(a, "--read0")) read0 = true;
        else if (!strcmp(a, "--no-read0")) read0 = false;
        else if (!strcmp(a, "--print0")) print0 = true;
        else if (!strcmp(a, "--no-print0")) print0 = false;
        else if (!strcmp(a, "--sync")) { /* batch is inherently sync */ }
        else if (!strcmp(a, "--multi") || !strcmp(a, "-m")) multi = true;
        else if (!strcmp(a, "--no-multi") || !strcmp(a, "+m")) multi = false;
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
        else if (!strcmp(a, "--info")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "info style required\n");
                return 2;
            }
            const char *v = argv[++i];
            if (!mm_parse_info_style(v, &info, &info_inline_right, &info_inline,
                                     &info_prefix)) {
                fprintf(stderr, "invalid info style (expected: default|right|hidden|inline[-right][:PREFIX])\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--no-info")) {
            info = false; info_inline_right = false; info_inline = false;
        }
        else if (ARG_EQ("--info")) {
            const char *v = a + 7;
            if (!mm_parse_info_style(v, &info, &info_inline_right, &info_inline,
                                     &info_prefix)) {
                fprintf(stderr, "invalid info style (expected: default|right|hidden|inline[-right][:PREFIX])\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--inline-info")) {
            info = true; info_inline_right = false; info_inline = true;
            info_prefix = NULL;
        }
        else if (!strcmp(a, "--no-inline-info")) {
            info = true; info_inline_right = false; info_inline = false;
        }
        else if (!strcmp(a, "--select-1") || !strcmp(a, "-1")) select_1 = true;
        else if (!strcmp(a, "--no-select-1") || !strcmp(a, "+1")) select_1 = false;
        else if (!strcmp(a, "--exit-0") || !strcmp(a, "-0")) exit_0 = true;
        else if (!strcmp(a, "--no-exit-0") || !strcmp(a, "+0")) exit_0 = false;
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
        else if (!strcmp(a, "--no-phony") || !strcmp(a, "--enabled")) phony = false;
        else if (ARG_EQ("--footer")) footer_spec = a + 9;
        else if (ARG_EQ("--padding")) padding = atoi(a + 10);
        else if (ARG_EQ("--margin")) {
            margin = atoi(a + 9);
            margin_default = mm_margin_is_default(a + 9);
        }
        else if (!strcmp(a, "--margin")) {
            const char *v = required_cli_arg(argc, argv, &i, "--margin");
            if (!v) return 2;
            margin = atoi(v);
            margin_default = mm_margin_is_default(v);
        }
        else if (!strcmp(a, "--no-margin")) {
            margin = 0;
            margin_default = true;
        }
        else if (!strcmp(a, "--padding")) {
            const char *v = required_cli_arg(argc, argv, &i, "--padding");
            if (!v) return 2;
            padding = atoi(v);
        }
        else if (ARG_EQ("--gutter")) gutter_spec = a + 9;
        else if (!strcmp(a, "--gutter")) { if (i+1<argc) gutter_spec = argv[++i]; }
        else if (ARG_EQ("--gutter-raw")) gutter_raw_spec = a + 13;
        else if (!strcmp(a, "--gutter-raw")) { if (i+1<argc) gutter_raw_spec = argv[++i]; }
        else if (!strcmp(a, "--footer")) { if (i+1<argc) footer_spec = argv[++i]; }
        else if (!strcmp(a, "--no-footer")) footer_spec = NULL;
        /* --disabled initially shows all items; search can be enabled by action. */
        else if (!strcmp(a, "--disabled")) disabled = true;
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
            /* A trailing '+' grows the floor by the border, input, and header. */
            bool plus = v[strlen(v) - 1] == '+';
            if (v[0] < '0' || v[0] > '9') {
                fprintf(stderr, "minimum height must be a non-negative integer\n");
                return 2;
            }
            min_height = atoi(v);
            if (plus) min_height = -min_height;
            min_height_set = true;
        }
        else if (!strcmp(a, "--min-height")) {
            if (i+1 >= argc) { fprintf(stderr, "minimum height required: HEIGHT[+]\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0') { fprintf(stderr, "minimum height required: HEIGHT[+]\n"); return 2; }
            bool plus = v[strlen(v) - 1] == '+';
            if (v[0] < '0' || v[0] > '9') {
                fprintf(stderr, "minimum height must be a non-negative integer\n");
                return 2;
            }
            min_height = atoi(v);
            if (plus) min_height = -min_height;
            min_height_set = true;
        }
        else if (!strcmp(a, "--no-color") || !strcmp(a, "+c")) {
            no_color = true;
            color_base_nocolor = true;
            color_spec = NULL;
        }
        else if (!strcmp(a, "--no-256") || !strcmp(a, "+2")) { /* UI palette option */ }
        else if (ARG_EQ("--height")) {
            height_index = i - 1;
            const char *v = a + 9;
            if (*v == '\0') { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            height_spec = v;
        }
        else if (ARG_EQ("--ellipsis")) ellipsis_spec = a + 11;
        else if (!strcmp(a, "--ellipsis")) { if (i+1<argc) ellipsis_spec = argv[++i]; }
        else if (ARG_EQ("--layout")) layout_spec = a + 9;
        else if (!strcmp(a, "--layout")) { if (i+1<argc) layout_spec = argv[++i]; }
        else if (!strcmp(a, "--height")) {
            height_index = i - 1;
            if (i+1 >= argc) { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0' || *v == '-') { fprintf(stderr, "height required: [~][-]HEIGHT[%%]\n"); return 2; }
            height_spec = v;
        }
        else if (!strcmp(a, "--no-height")) {
            height_spec = NULL;
            height_index = 0;
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
        else if (!strcmp(a, "--raw")) { raw = true; }
        else if (!strcmp(a, "--no-raw")) { raw = false; }
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
        else if (!strcmp(a, "--bash")) show_bash = true;
        else if (!strcmp(a, "--black")) { /* part of the --color palette */ }
        else if (!strcmp(a, "--filepath-word")) filepath_word = true;
        else if (!strcmp(a, "--no-filepath-word")) filepath_word = false;
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
        else if (ARG_EQ("--footer-label")) footer_label = a + strlen("--footer-label=");
        else if (!strcmp(a, "--footer-label")) {
            if (i + 1 >= argc) { fprintf(stderr, "footer label required\n"); return 2; }
            footer_label = argv[++i];
        }
        else if (!strcmp(a, "--no-footer-label")) footer_label = NULL;
        else if (ARG_EQ("--footer-label-pos") || !strcmp(a, "--footer-label-pos")) {
            const char *value;
            if (a[strlen("--footer-label-pos")] == '=') value = a + strlen("--footer-label-pos=");
            else if (i + 1 < argc) value = argv[++i];
            else {
                fprintf(stderr, "footer label position required (positive or negative integer or 'center')\n");
                return 2;
            }
            const char *err = NULL;
            if (!mm_parse_label_pos_fzf(value, &footer_label_pos, &footer_label_bottom, &err)) {
                fprintf(stderr, "%s\n", err ? err : "not a valid integer");
                return 2;
            }
        }
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
        else if (!strcmp(a, "--no-header-lines-border")) header_lines_border_set = false;
        else if (!strncmp(a, "--header-lines-border", strlen("--header-lines-border"))) {
            int rc = take_component_border(argc, argv, &i, a, "--header-lines-border", &header_lines_border);
            if (rc < 0) return 2;
            if (rc > 0) header_lines_border_set = true;
            else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
        }
        else if (!strcmp(a, "--no-header-border")) {
            header_border_shape = MM_BORDER_NONE;
            header_border_set = true;
        }
        else if (!strncmp(a, "--header-border", strlen("--header-border"))) {
            int rc = take_component_border(argc, argv, &i, a, "--header-border", &header_border_shape);
            if (rc < 0) return 2;
            if (rc > 0) header_border_set = true;
            else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
        }
        else if (!strcmp(a, "--no-footer-border")) {
            footer_border = MM_BORDER_NONE;
            footer_border_set = true;
        }
        else if (!strncmp(a, "--footer-border", strlen("--footer-border"))) {
            int rc = take_component_border(argc, argv, &i, a, "--footer-border", &footer_border);
            if (rc < 0) return 2;
            if (rc > 0) footer_border_set = true;
            else { fprintf(stderr, "unknown option: %s\n", a); return 2; }
        }
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
        else if (!strcmp(a, "--tmux") || !strcmp(a, "--popup")) {
            const char *value = NULL;
            if (i + 1 < argc && argv[i + 1][0] != '-' && argv[i + 1][0] != '+')
                value = argv[++i];
            if (!mm_popup_parse(value, i - (value ? 2 : 1), &popup)) return 2;
        }
        else if (ARG_EQ("--tmux") || ARG_EQ("--popup")) {
            const char *eq = strchr(a, '=');
            if (!mm_popup_parse(eq ? eq + 1 : "", i - 1, &popup)) return 2;
        }
        else if (!strcmp(a, "--no-tmux") || !strcmp(a, "--no-popup")) {
            popup.set = false;
        }
        else if (!strcmp(a, "--force-tty-in")) force_tty_in = true;
        else if (!strcmp(a, "--no-force-tty-in")) force_tty_in = false;
    else if (!strcmp(a, "--bench") || ARG_EQ("--bench")) {
        const char *value = NULL;
        if (a[strlen("--bench")] == '=') value = a + strlen("--bench") + 1;
        else if (i + 1 < argc) value = argv[++i];
        if (!value) {
            fprintf(stderr, "duration required (e.g. 3s, 500ms)\n");
            return 2;
        }
        if (!parse_go_duration(value, &bench_ns)) {
            fprintf(stderr, "invalid duration for --bench: %s\n", value);
            return 2;
        }
    }
    else if (!strcmp(a, "--style")) { if (i+1<argc) style_spec = argv[++i]; else { fprintf(stderr, "style preset required\n"); return 2; } }
    else if (ARG_EQ("--style")) style_spec = a + 8;
    else if (ARG_EQ("--id-nth")) id_nth_spec = a + 9;
    else if (!strcmp(a, "--id-nth")) {
        id_nth_spec = required_cli_arg(argc, argv, &i, "--id-nth");
        if (!id_nth_spec) return 2;
    }
    else if (!strcmp(a, "--no-id-nth")) id_nth_spec = NULL;
    else if (ARG_EQ("--freeze-left") || !strcmp(a, "--freeze-left")) {
        if (!take_freeze_count(argc, argv, &i, a, "--freeze-left", &freeze_left))
            return 2;
    }
    else if (ARG_EQ("--freeze-right") || !strcmp(a, "--freeze-right")) {
        if (!take_freeze_count(argc, argv, &i, a, "--freeze-right", &freeze_right))
            return 2;
    }
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
        else if (!strcmp(a, "--man")) { show_man = true; show_help = false; show_version = false; }
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
        else if (!strcmp(a, "--border")) {
            border_shape = MM_BORDER_ROUNDED;
            popup_border = MM_BORDER_ROUNDED;
        }
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
            const char *spec = a + 9;
            int b = mm_border_parse(spec);
            if (b < 0) { fprintf(stderr, "invalid border style\n"); return 2; }
            border_shape = b;
            /* The outer parser folds "line" into a top edge. fzf still
               treats that name as BorderLine when deciding the popup. */
            if (!strcmp(spec, "line")) popup_border = MM_BORDER_LINE;
            else if (!strcmp(spec, "inline")) popup_border = MM_BORDER_INLINE;
            else popup_border = b;
        }
        else if (!strcmp(a, "--no-border")) {
            border_shape = MM_BORDER_NONE;
            popup_border = MM_BORDER_NONE;
        }
        else if (ARG_EQ("--color")) { color_spec = a + 8; no_color = false; }
        else if (!strcmp(a, "--color")) {
            if (i + 1 < argc) { color_spec = argv[++i]; no_color = false; }
        }
        else if (ARG_EQ("--bind")) {
            const char *v = a + 7;
            if (*v == '\0') { fprintf(stderr, "bind expression required\n"); return 2; }
            if (!bind_spec_append(&bind_spec, v)) {
                fprintf(stderr, "fzf: out of memory\n");
                return 2;
            }
        }
        else if (!strcmp(a, "--bind")) {
            if (i+1 >= argc) { fprintf(stderr, "bind expression required\n"); return 2; }
            const char *v = argv[++i];
            if (*v == '\0') { fprintf(stderr, "bind expression required\n"); return 2; }
            if (!bind_spec_append(&bind_spec, v)) {
                fprintf(stderr, "fzf: out of memory\n");
                return 2;
            }
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
        else if (!strcmp(a, "--version")) { show_version = true; show_help = false; }
        else if (!strcmp(a, "--help") || !strcmp(a, "-h")) { show_help = true; show_version = false; }
        else if (!strcmp(a, "--no-winpty") ||
                 !strcmp(a, "--no-keep-right") ||
                 !strcmp(a, "--hscroll") ||
                 !strcmp(a, "--no-sync") ||
                 !strcmp(a, "--async") ||
                 !strcmp(a, "--no-preview-border") ||
                 !strcmp(a, "--no-padding") ||
                 !strcmp(a, "--no-list-border") ||
                 !strcmp(a, "--no-list-label") ||
                 !strcmp(a, "--no-header-label") ||
                 !strcmp(a, "--no-footer-border") ||
                 !strcmp(a, "--no-footer-label") ||
                 !strcmp(a, "--no-input-border") ||
                 !strcmp(a, "--no-input-label") ||
                 !strcmp(a, "--no-border-label") ||
                 !strcmp(a, "--no-preview-label") ||
                 !strcmp(a, "--no-listen") ||
                 !strcmp(a, "--no-listen-unsafe") ||
                 !strcmp(a, "--no-black") ||
                 !strcmp(a, "--bold") ||
                 !strcmp(a, "--multi-line") ||
                 !strcmp(a, "--clear")) {
            /* UI-only flags do not change batch filtering semantics. */
        }
        else if (!strcmp(a, "--proxy-script")) {
            proxy_script = required_cli_arg(argc, argv, &i, "--proxy-script");
            if (!proxy_script) return 2;
        }
        else if (ARG_EQ("--proxy-script")) {
            proxy_script = a + sizeof("--proxy-script=") - 1;
        }
        else if (!strcmp(a, "--tty-default") ||
                 !strcmp(a, "--toggle-sort") ||
                 !strcmp(a, "--profile-cpu") ||
                 !strcmp(a, "--profile-mem") ||
                 !strcmp(a, "--profile-block") ||
                 !strcmp(a, "--profile-mutex")) {
            if (!required_cli_arg(argc, argv, &i, a)) return 2;
        }
        else if (ARG_EQ("--tty-default") || ARG_EQ("--proxy-script") ||
                 ARG_EQ("--toggle-sort") || ARG_EQ("--profile-cpu") ||
                 ARG_EQ("--profile-mem") || ARG_EQ("--profile-block") ||
                 ARG_EQ("--profile-mutex")) {
            /* Values are parsed for CLI compatibility; these facilities are
               not used by the batch filter implementation. */
        }
        else if ((a[0] == '-' || a[0] == '+') && a[1] != 0) {
            /* fzf rejects unknown options (PLAN: no silently ignored flags). */
            fprintf(stderr, "unknown option: %s\n", a);
            return 2;
        }
        #undef ARG_EQ
    }

    if (show_help) { usage(stdout); return 0; }
    if (show_version) { printf("0.74 (devel)\n"); return 0; }
    if (show_man) return print_man_page();
    if (show_bash) return print_bash_integration();

    if (accept_nth_spec && !mm_accept_nth_validate(accept_nth_spec)) return 2;
    if (id_nth_spec) {
        mm_range_t *id_ranges = NULL;
        int id_range_count = 0;
        if (!mm_parse_ranges(id_nth_spec, &id_ranges, &id_range_count)) {
            fprintf(stderr, "invalid format: %s\n", id_nth_spec);
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
            "up:up,down:down,"
            "home:beginning-of-line,ctrl-a:beginning-of-line,end:end-of-line,ctrl-e:end-of-line,"
            "page-up:page-up,page-down:page-down,"
            "backspace:backward-delete-char,ctrl-h:backward-delete-char,"
            "delete:delete-char,ctrl-d:delete-char/eof,"
            "ctrl-u:unix-line-discard,ctrl-w:unix-word-rubout,ctrl-y:yank,"
            "tab:toggle-down,btab:toggle-up,"
            "ctrl-n:down-match,ctrl-p:up-match,"
            "ctrl-z:toggle";
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
        free(bind_spec);
        bind_spec = NULL;
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
    if (color_spec && color_base_nocolor) theme_set_nocolor(&theme);
    if (color_spec) {
        const char *cerr = NULL;
        if (!mm_theme_parse(&theme, color_spec, &cerr)) {
            fprintf(stderr, "%s: %s\n", cerr, color_spec);
            goto fail;
        }
    }

    /* fzf uses the path scheme when the list comes from the tty (the walker
       or $FZF_DEFAULT_COMMAND), unless the user set --scheme or --tiebreak,
       or start:reload / start:transform will replace that list. */
    if (!scheme_explicit && !tiebreak && !no_input && isatty(STDIN_FILENO)) {
        bool start_swap = false;
        for (int ei = 0; ei < events.n; ei++) {
            const char *nm = events.items[ei].name;
            mm_action_t act;
            if (!nm || strcmp(nm, "start") != 0) continue;
            act = events.items[ei].action;
            if (act == MM_ACT_RELOAD || act == MM_ACT_RELOAD_SYNC ||
                act == MM_ACT_TRANSFORM) {
                start_swap = true;
                break;
            }
        }
        if (!start_swap) scheme = "path";
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

    if (!filter && popup.set) {
        int raw_argc = argc - (int)mm_cli_default_word_count;
        if (raw_argc < 1) raw_argc = 1;
        char **rawv = malloc(((size_t)raw_argc + 1) * sizeof(char *));
        if (!rawv) {
            fprintf(stderr, "fzf: out of memory\n");
            goto fail;
        }
        rawv[0] = argv[0];
        for (int k = 1; k < raw_argc; k++)
            rawv[k] = argv[mm_cli_default_word_count + (size_t)k];
        rawv[raw_argc] = NULL;
        int popup_code = 0;
        bool launched = mm_popup_launch(&popup, height_index, margin_default,
                                        popup_border, border_label, shell_spec,
                                        force_tty_in, raw_argc, rawv, &popup_code);
        free(rawv);
        if (launched) {
            mm_keymap_free(&keymap);
            mm_event_map_free(&events);
            for (int k = 0; k < expect_n; k++) free(expect_keys[k]);
            free(expect_keys);
            for (int k = 0; k < nskip; k++) free((char *)skip_names[k]);
            free(skip_names);
            return popup_code;
        }
    }

    size_t len = 0;
    char *data = NULL;
    char **lines = NULL;
    size_t *line_lens = NULL;
    int count = 0;
    /* --filter prints from the input buffer. Interactive mode owns a copy
       because the UI frees each line. */
    bool lines_owned = filter == NULL;
    bool stream_input = false;
    int stream_fd = -1;
    FILE *stream_fp = NULL;
    pthread_t walk_thread;
    bool have_walk_thread = false;
    struct timespec ingest_start = {0}, ingest_end = {0};
    clock_gettime(CLOCK_MONOTONIC, &ingest_start);
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
#ifdef F_SETPIPE_SZ
                /* The walker runs ahead of the UI. A larger pipe keeps the
                   workers from blocking on the default 64 KiB capacity. */
                (void)fcntl(pipefd[0], F_SETPIPE_SZ, 1 << 20);
#endif
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
    line_lens = malloc((size_t)cap * sizeof(*line_lens));
    if (!lines || !line_lens) { free(lines); free(line_lens); free(data); fprintf(stderr, "fzf: out of memory\n"); return 2; }
    char sep = read0 ? '\0' : '\n';
    char *p = data;
    while (p < data + len) {
        char *nl = memchr(p, sep, (size_t)(data + len - p));
        size_t llen = nl ? (size_t)(nl - p) : (size_t)(data + len - p);
        char *line;
        if (lines_owned) {
            line = malloc(llen + 1);
            if (!line) { fprintf(stderr, "fzf: out of memory\n"); return 2; }
            memcpy(line, p, llen);
            line[llen] = 0;
        } else {
            if (nl) *nl = '\0';
            line = p;
        }
        if (count == cap) {
            if (cap > INT_MAX / 2 ||
                (size_t)cap * 2u > SIZE_MAX / sizeof(*lines)) {
                free(line);
                fprintf(stderr, "fzf: too many input records\n");
                return 2;
            }
            int next_cap = cap * 2;
            char **nl2 = realloc(lines, (size_t)next_cap * sizeof(*lines));
            if (!nl2) { free(line); free(line_lens); fprintf(stderr, "fzf: out of memory\n"); return 2; }
            lines = nl2;
            size_t *nl3 = realloc(line_lens, (size_t)next_cap * sizeof(*line_lens));
            if (!nl3) { free(line); fprintf(stderr, "fzf: out of memory\n"); return 2; }
            line_lens = nl3;
            cap = next_cap;
        }
        lines[count++] = line;
        line_lens[count - 1] = llen;
        if (!nl) break;
        p = nl + 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &ingest_end);

    if (tail >= 0 && count > tail) {
        /* keep last N */
        int drop = count - tail;
        if (lines_owned) {
            for (int i = 0; i < drop; i++) free(lines[i]);
        }
        memmove(line_lens, line_lens + drop, (size_t)tail * sizeof(*line_lens));
        memmove(lines, lines + drop, (size_t)tail * sizeof(char *));
        count = tail;
    }

    /* Apply style presets after option parsing, matching upstream fzf's
       preset precedence. Component-specific borders are tracked separately
       in the feature inventory; this renderer currently has one outer box. */
    if (style_spec) {
        if (!strcmp(style_spec, "default")) {
            info_inline_right = false;
            info_inline = false;
            highlight_line = false;
        } else if (!strcmp(style_spec, "minimal")) {
            info_inline_right = false;
            info_inline = false;
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
            int component_border = MM_BORDER_ROUNDED;
            if (colon && *colon) {
                int b = mm_border_parse(colon);
                int c = mm_border_parse_component(colon);
                if (b < 0 || c < 0) {
                    fprintf(stderr, "unsupported style border: %s\n", colon);
                    return 2;
                }
                border_shape = b;
                component_border = c;
                /* fzf's full preset sets the outer border only for "line". */
                if (!strcmp(colon, "line")) popup_border = MM_BORDER_LINE;
            }
            /* --style=full gives the footer the preset border unless a
               footer border option already selected one. */
            if (!footer_border_set) {
                footer_border = component_border;
                footer_border_set = true;
            }
            info = true;
            info_inline_right = true;
            info_inline = false;
            highlight_line = true;
        }
    }

    mm_filter_set_threads(threads);

    if (!filter) {
        /* Interactive mode. */
        mm_ui_t ui;
        memset(&ui, 0, sizeof(ui));
        /* A negative descriptor means all input is already available. A
           nonnegative descriptor is installed only for asynchronous streams. */
        ui.input_fd = -1;
        ui.crit = crit;
        ui.accept_nth_spec = accept_nth_spec;
        ui.id_nth_spec = id_nth_spec;
        if (!mm_delimiter_init(&ui.delim, delim_str)) {
            fprintf(stderr, "fzf: out of memory\n");
            mm_keymap_free(&keymap);
            mm_event_map_free(&events);
            return 2;
        }
        ui.extended = extended;
        ui.fuzzy = !exact;
        ui.raw = raw;
        ui.case_mode = case_mode;
        ui.tac = tac;
        ui.no_sort = no_sort;
        ui.multi = multi;
        ui.multi_max = multi_max;
        ui.reverse = reverse;
        ui.cycle = cycle;
        ui.info = info;
        ui.info_inline = info_inline;
        ui.info_inline_right = info_inline_right;
        ui.info_prefix = info_prefix;
        ui.prompt = prompt;
        ui.header = header;
        ui.header_line_count = header_lines;
        ui.query = query;
        ui.print_query = print_query;
        ui.expect = expect;
        ui.keymap = keymap;
        memset(&keymap, 0, sizeof(keymap));
        ui.events = events;
        memset(&events, 0, sizeof(events));
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
        ui.border = border_shape < 0 ? MM_BORDER_NONE : border_shape;
        ui.proxy_script = proxy_script;
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
        ui.footer_label = footer_label;
        ui.footer_label_pos = footer_label_pos;
        ui.footer_label_bottom = footer_label_bottom;
        ui.footer_border = footer_border;
        ui.footer_border_set = footer_border_set;
        ui.header_lines_border = header_lines_border;
        ui.header_lines_border_set = header_lines_border_set;
        ui.gutter = gutter_spec;
        ui.gutter_raw = gutter_raw_spec;
        ui.no_unicode = no_unicode;
        ui.no_hscroll = no_hscroll;
        /* --read0 items keep their newlines. --no-multi-line shows those
           breaks as a single row; otherwise each line is its own row. */
        ui.no_multi_line = no_multi_line;
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
        ui.freeze_left = freeze_left;
        ui.freeze_right = freeze_right;
        ui.separator = separator_spec ? separator_spec : "";
        ui.separator_default = separator_spec == NULL;
        ui.no_scrollbar = no_scrollbar;
        ui.no_clear = no_clear;
        ui.tabstop = tabstop;
        ui.no_input = no_input;
        ui.no_color = no_color;
        ui.plain_theme = color_base_nocolor && color_spec != NULL && !no_color;
        /* --keep-right and --sync match the behaviour this implementation
           already has: the preview is on the right and the initial search
           is always synchronous. */
        ui.mouse = !no_mouse;
        (void)keep_right;
        (void)sync;
        /* Unset --min-height is fzf's "10+": ten rows, plus the chrome. */
        if (!min_height_set) min_height = -10;
        ui.min_height = min_height;
        if (no_color) theme_set_nocolor(&ui.theme);
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
            if (style_full) ui.preview_win.border_shape = -1;
            ui.preview_win_initial = ui.preview_win;
            if (ui.preview_win.hidden) ui.preview_hidden = true;
        }
        if (nth_spec && !mm_parse_ranges(nth_spec, &ui.nth, &ui.nth_n)) {
            fprintf(stderr, "invalid format: %s\n", nth_spec);
            goto interactive_error;
        }
        if (with_nth_spec &&
            !mm_parse_ranges(with_nth_spec, &ui.with_nth, &ui.with_nth_n)) {
            fprintf(stderr, "invalid format: %s\n", with_nth_spec);
            goto interactive_error;
        }
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
        mm_keymap_free(&keymap); mm_event_map_free(&events);
        for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
        free(expect_keys);
        for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
        free(skip_names);
        return rc;
    interactive_error:
        mm_ui_free(&ui);
        mm_keymap_free(&keymap); mm_event_map_free(&events);
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
        fprintf(stderr, "invalid format: %s\n", nth_spec);
        goto fail;
    }
    mm_range_t *wnth = NULL; int wnth_n = 0;
    if (with_nth_spec && !mm_parse_ranges(with_nth_spec, &wnth, &wnth_n)) {
        fprintf(stderr, "invalid format: %s\n", with_nth_spec);
        goto fail;
    }

    char **ansi_lines = NULL;
    char **search_lines = lines;
    if (ansi && count > 0) {
        if ((size_t)count > SIZE_MAX / sizeof(*ansi_lines)) {
            fprintf(stderr, "fzf: too many input records\n");
            mm_pattern_free(pat);
            mm_delimiter_free(&delim);
            if (wnth != nth) free(wnth);
            free(nth);
            release_lines(lines, count, lines_owned);
            free(line_lens);
            free(data);
            mm_keymap_free(&keymap); mm_event_map_free(&events);
            for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
            free(expect_keys);
            for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
            free(skip_names);
            return 2;
        }
        ansi_lines = calloc((size_t)count, sizeof(*ansi_lines));
        if (!ansi_lines) {
            fprintf(stderr, "fzf: out of memory\n");
            mm_pattern_free(pat);
            mm_delimiter_free(&delim);
            if (wnth != nth) free(wnth);
            free(nth);
            release_lines(lines, count, lines_owned);
            free(line_lens);
            free(data);
            mm_keymap_free(&keymap); mm_event_map_free(&events);
            for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
            free(expect_keys);
            for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
            free(skip_names);
            return 2;
        }
        for (int i = 0; i < count; i++) {
            ansi_lines[i] = strip_ansi_record(lines[i]);
            if (!ansi_lines[i]) {
                fprintf(stderr, "fzf: out of memory\n");
                for (int j = 0; j < i; j++) free(ansi_lines[j]);
                free(ansi_lines);
                mm_pattern_free(pat);
                mm_delimiter_free(&delim);
                if (wnth != nth) free(wnth);
                free(nth);
                release_lines(lines, count, lines_owned);
                free(data);
                mm_keymap_free(&keymap); mm_event_map_free(&events);
                for (int j = 0; j < expect_n; j++) free(expect_keys[j]);
                free(expect_keys);
                for (int j = 0; j < nskip; j++) free((char *)skip_names[j]);
                free(skip_names);
                return 2;
            }
        }
        search_lines = ansi_lines;
        for (int i = 0; i < count; i++) line_lens[i] = strlen(ansi_lines[i]);
    }
    mm_input_t input = {
        .lines = search_lines, .count = count, .items = NULL, .base = 0,
        .delim = delim, .nth_ranges = nth, .nth_n = nth_n,
        .with_nth_ranges = wnth, .with_nth_n = wnth_n,
        .line_lens = line_lens
    };
    int n = 0;
    int *out = NULL;
    if (bench_ns > 0) {
        struct timespec now, deadline, t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &now);
        deadline.tv_sec = now.tv_sec + (time_t)(bench_ns / 1000000000LL);
        deadline.tv_nsec = now.tv_nsec + (long)(bench_ns % 1000000000LL);
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec++;
            deadline.tv_nsec -= 1000000000L;
        }
        int iters = 0;
        long long min_ns = LLONG_MAX, max_ns = 0, total_ns = 0;
        int match_count = 0;
        do {
            clock_gettime(CLOCK_MONOTONIC, &t0);
            free(out);
            out = mm_batch_search(&input, pat, &crit, tac, &n, NULL);
            clock_gettime(CLOCK_MONOTONIC, &t1);
            if (!out) {
                fprintf(stderr, "fzf: failed to filter input (out of memory)\n");
                mm_pattern_free(pat);
                mm_delimiter_free(&delim);
                if (wnth != nth) free(wnth);
                free(nth);
                if (ansi_lines) {
                    for (int i = 0; i < count; i++) free(ansi_lines[i]);
                    free(ansi_lines);
                }
                release_lines(lines, count, lines_owned);
                free(line_lens);
                free(data);
                mm_keymap_free(&keymap); mm_event_map_free(&events);
                for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
                free(expect_keys);
                for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
                free(skip_names);
                return 2;
            }
            long long dt = (t1.tv_sec - t0.tv_sec) * 1000000000LL +
                           (t1.tv_nsec - t0.tv_nsec);
            if (dt < 0) dt = 0;
            if (dt < min_ns) min_ns = dt;
            if (dt > max_ns) max_ns = dt;
            total_ns += dt;
            iters++;
            match_count = n;
            clock_gettime(CLOCK_MONOTONIC, &now);
        } while (now.tv_sec < deadline.tv_sec ||
                 (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
        long long ingest_ns =
            (ingest_end.tv_sec - ingest_start.tv_sec) * 1000000000LL +
            (ingest_end.tv_nsec - ingest_start.tv_nsec);
        if (ingest_ns < 0) ingest_ns = 0;
        double avg_ms = iters ? (double)(total_ns / iters) / 1e6 : 0;
        double min_ms = min_ns == LLONG_MAX ? 0 : (double)min_ns / 1e6;
        double max_ms = (double)max_ns / 1e6;
        double total_s = (double)total_ns / 1e9;
        double ingest_ms = (double)ingest_ns / 1e6;
        double selectivity = count > 0 ? (double)match_count * 100.0 / (double)count : 0;
        if (print_query) {
            fputs(filter, stdout);
            fputc(print0 ? '\0' : '\n', stdout);
        }
        printf("  %d iterations  avg: %.2fms  min: %.2fms  max: %.2fms  total: %.2fs  items: %d  matches: %d (%.2f%%)  ingestion: %.2fms\n",
               iters, avg_ms, min_ms, max_ms, total_s, count, match_count,
               selectivity, ingest_ms);
        free(out);
        mm_pattern_free(pat);
        mm_delimiter_free(&delim);
        if (wnth != nth) free(wnth);
        free(nth);
        if (ansi_lines) {
            for (int i = 0; i < count; i++) free(ansi_lines[i]);
            free(ansi_lines);
        }
        release_lines(lines, count, lines_owned);
        free(line_lens);
        free(data);
        mm_keymap_free(&keymap); mm_event_map_free(&events);
        for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
        free(expect_keys);
        for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
        free(skip_names);
        return 0;
    }
    out = mm_batch_search(&input, pat, &crit, tac, &n, NULL);
    (void)no_sort; /* handled by pat->sortable */
    if (!out) {
        fprintf(stderr, "fzf: failed to filter input (out of memory)\n");
        mm_pattern_free(pat);
        mm_delimiter_free(&delim);
        if (wnth != nth) free(wnth);
        free(nth);
        if (ansi_lines) {
            for (int i = 0; i < count; i++) free(ansi_lines[i]);
            free(ansi_lines);
        }
        release_lines(lines, count, lines_owned);
        free(line_lens);
        free(data);
        mm_keymap_free(&keymap); mm_event_map_free(&events);
        for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
        free(expect_keys);
        for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
        free(skip_names);
        return 2;
    }

    char osep = print0 ? '\0' : '\n';
    if (print_query) { fputs(filter, stdout); fputc(osep, stdout); }
    for (int i = 0; i < n; i++) {
        if (header_lines > 0 && (int)out[i] < header_lines) continue;
        const char *line = search_lines[out[i]];
        if (accept_nth_spec) {
            char *accepted = mm_accept_nth_expand(accept_nth_spec, line, delim,
                                                out[i]);
            if (!accepted) {
                fprintf(stderr, "fzf: failed to format accepted item (out of memory)\n");
                free(out);
                mm_pattern_free(pat);
                mm_delimiter_free(&delim);
                if (wnth != nth) free(wnth);
                free(nth);
                if (ansi_lines) {
                    for (int j = 0; j < count; j++) free(ansi_lines[j]);
                    free(ansi_lines);
                }
                release_lines(lines, count, lines_owned);
                free(data);
                mm_keymap_free(&keymap); mm_event_map_free(&events);
                for (int j = 0; j < expect_n; j++) free(expect_keys[j]);
                free(expect_keys);
                for (int j = 0; j < nskip; j++) free((char *)skip_names[j]);
                free(skip_names);
                return 2;
            }
            fputs(accepted, stdout);
            free(accepted);
        } else {
            size_t linelen = 0;
            int32_t li = out[i];
            if (line_lens && li >= 0 && (size_t)li < (size_t)count) linelen = line_lens[li];
            fwrite(line, 1, linelen, stdout);
        }
        fputc(osep, stdout);
    }

    mm_pattern_free(pat);
    mm_delimiter_free(&delim);
    free(out);
    if (ansi_lines) {
        for (int i = 0; i < count; i++) free(ansi_lines[i]);
        free(ansi_lines);
    }
    if (wnth != nth) free(wnth);
    free(nth);
    release_lines(lines, count, lines_owned);
    free(line_lens);
    free(data);
    mm_keymap_free(&keymap); mm_event_map_free(&events);
    for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
    free(expect_keys);
    for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
    free(skip_names);
    return n == 0 ? 1 : 0;

fail:
    free(bind_spec);
    mm_keymap_free(&keymap); mm_event_map_free(&events);
    for (int i = 0; i < expect_n; i++) free(expect_keys[i]);
    free(expect_keys);
    for (int i = 0; i < nskip; i++) free((char *)skip_names[i]);
    free(skip_names);
    return 2;
}
