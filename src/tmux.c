/* tmux.c - --tmux / --popup, matching fzf's parseTmuxOptions, popupArgStr,
   runTmux, and runZellij. The child is this binary plus the original
   arguments, with --no-popup --no-height appended so it does not recurse. */
#define _POSIX_C_SOURCE 200809L
#include "tmux.h"
#include "terminal.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

static volatile sig_atomic_t popup_child_pid;

static void popup_on_int(int sig) {
    pid_t pid = (pid_t)popup_child_pid;
    if (pid > 0) kill(pid, sig);
}

static void popup_error(const char *arg) {
    fprintf(stderr,
            "invalid popup option: %s (expected: "
            "[center|top|bottom|left|right][,SIZE[%%]][,SIZE[%%]][,border-native])\n",
            arg ? arg : "");
}

/* strconv.ParseFloat / Atoi, without leading spaces. Percent values use
   ParseFloat; bare numbers must be whole integers. */
static bool parse_size(const char *str, double max_percent,
                       double *size, bool *percent) {
    if (!str || !*str || isspace((unsigned char)str[0])) return false;
    size_t n = strlen(str);
    if (str[n - 1] == '%') {
        char *tmp = strndup(str, n - 1);
        if (!tmp) return false;
        if (!*tmp || isspace((unsigned char)tmp[0])) { free(tmp); return false; }
        char *end = NULL;
        errno = 0;
        double v = strtod(tmp, &end);
        bool ok = end != tmp && end && *end == '\0' && errno == 0;
        free(tmp);
        if (!ok || v < 0 || v > max_percent) return false;
        *size = v;
        *percent = true;
        return true;
    }
    if (strchr(str, '.')) return false;
    char *end = NULL;
    errno = 0;
    long long v = strtoll(str, &end, 10);
    if (end == str || !end || *end || errno == ERANGE || v < 0) return false;
    *size = (double)v;
    *percent = false;
    return true;
}

static void popup_defaults(mm_popup_opts_t *out, int index) {
    memset(out, 0, sizeof(*out));
    out->set = true;
    out->index = index;
    out->position = MM_POPUP_CENTER;
    out->width = 50;
    out->width_percent = true;
    out->height = 50;
    out->height_percent = true;
}

/* fzf splits on one or more commas or colons and keeps empty edge tokens. */
static int split_popup(char *s, char **toks, int max) {
    int n = 0;
    char *p = s;
    if (n < max) toks[n++] = p;
    while (*p && n < max) {
        if (*p == ',' || *p == ':') {
            *p++ = '\0';
            while (*p == ',' || *p == ':') p++;
            toks[n++] = p;
            if (!*p) break;
        } else {
            p++;
        }
    }
    return n;
}

bool mm_popup_parse(const char *arg, int index, mm_popup_opts_t *out) {
    popup_defaults(out, index);
    if (!arg) return true;

    char *copy = strdup(arg);
    if (!copy) {
        fprintf(stderr, "fzf: out of memory\n");
        return false;
    }
    char *toks[6];
    int n = split_popup(copy, toks, 6);
    if (n == 0 || n > 4) {
        popup_error(arg);
        free(copy);
        return false;
    }
    for (int i = 0; i < n; i++) {
        if (strcmp(toks[i], "border-native") == 0) {
            for (int j = i; j < n - 1; j++) toks[j] = toks[j + 1];
            n--;
            out->border = true;
            break;
        }
    }

    const char *first = "center";
    if (n > 0) first = toks[0];
    if (strcmp(first, "top") == 0 || strcmp(first, "up") == 0) {
        out->position = MM_POPUP_UP;
        out->width = 100;
        out->width_percent = true;
    } else if (strcmp(first, "bottom") == 0 || strcmp(first, "down") == 0) {
        out->position = MM_POPUP_DOWN;
        out->width = 100;
        out->width_percent = true;
    } else if (strcmp(first, "left") == 0) {
        out->position = MM_POPUP_LEFT;
        out->height = 100;
        out->height_percent = true;
    } else if (strcmp(first, "right") == 0) {
        out->position = MM_POPUP_RIGHT;
        out->height = 100;
        out->height_percent = true;
    } else if (strcmp(first, "center") != 0) {
        if (n >= 6) {
            popup_error(arg);
            free(copy);
            return false;
        }
        for (int j = n; j > 0; j--) toks[j] = toks[j - 1];
        toks[0] = (char *)"center";
        n++;
        first = "center";
    }

    double size1 = 0, size2 = 0;
    bool pct1 = false, pct2 = false;
    if (n > 1) {
        if (!parse_size(toks[1], 100, &size1, &pct1)) {
            popup_error(arg);
            free(copy);
            return false;
        }
    }
    if (n == 3) {
        if (!parse_size(toks[2], 100, &size2, &pct2)) {
            popup_error(arg);
            free(copy);
            return false;
        }
        out->width = size1;
        out->width_percent = pct1;
        out->height = size2;
        out->height_percent = pct2;
    } else if (n == 2) {
        if (strcmp(first, "top") == 0 || strcmp(first, "up") == 0 ||
            strcmp(first, "bottom") == 0 || strcmp(first, "down") == 0) {
            out->height = size1;
            out->height_percent = pct1;
        } else if (strcmp(first, "left") == 0 || strcmp(first, "right") == 0) {
            out->width = size1;
            out->width_percent = pct1;
        } else {
            out->width = size1;
            out->width_percent = pct1;
            out->height = size1;
            out->height_percent = pct1;
        }
    }
    free(copy);
    return true;
}

bool mm_margin_is_default(const char *spec) {
    if (!spec) return true;
    int parts = 1;
    for (const char *p = spec; *p; p++) if (*p == ',') parts++;
    if (parts < 1 || parts > 4) return false;
    const char *p = spec;
    for (int i = 0; i < parts; i++) {
        const char *comma = strchr(p, ',');
        size_t n = comma ? (size_t)(comma - p) : strlen(p);
        char *tok = strndup(p, n);
        if (!tok) return false;
        double size = 0;
        bool percent = false;
        bool ok = parse_size(tok, 49, &size, &percent) && size == 0;
        free(tok);
        if (!ok) return false;
        if (!comma) break;
        p = comma + 1;
    }
    return true;
}

static void format_size(char *dst, size_t n, double size, bool percent) {
    if (percent) snprintf(dst, n, "%d%%", (int)size);
    else snprintf(dst, n, "%d", (int)size);
}

static char *lookup_bin(const char *file) {
    if (!file || !*file) return NULL;
    if (strchr(file, '/')) {
        if (access(file, X_OK) == 0) return strdup(file);
        return NULL;
    }
    const char *path = getenv("PATH");
    if (!path || !*path) return NULL;
    char *copy = strdup(path);
    if (!copy) return NULL;
    char *save = NULL;
    for (char *dir = strtok_r(copy, ":", &save); dir; dir = strtok_r(NULL, ":", &save)) {
        if (!*dir) continue;
        char candidate[PATH_MAX];
        int wrote = snprintf(candidate, sizeof(candidate), "%s/%s", dir, file);
        if (wrote < 0 || (size_t)wrote >= sizeof(candidate)) continue;
        if (access(candidate, X_OK) == 0) {
            free(copy);
            return strdup(candidate);
        }
    }
    free(copy);
    return NULL;
}

static char *shell_quote(const char *s) {
    size_t extra = 0;
    for (const char *p = s ? s : ""; *p; p++) if (*p == '\'') extra += 3;
    size_t n = strlen(s ? s : "");
    char *out = malloc(n + extra + 3);
    if (!out) return NULL;
    char *w = out;
    *w++ = '\'';
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\'') {
            memcpy(w, "'\\''", 4);
            w += 4;
        } else {
            *w++ = *p;
        }
    }
    *w++ = '\'';
    *w = '\0';
    return out;
}

static bool write_shell_quote(FILE *f, const char *s) {
    if (fputc('\'', f) == EOF) return false;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\'') {
            if (fputs("'\\''", f) == EOF) return false;
        } else if (fputc(*p, f) == EOF) {
            return false;
        }
    }
    return fputc('\'', f) != EOF;
}

/* Go's %q for the fifo paths. The paths we generate are plain. */
static bool write_go_quote(FILE *f, const char *s) {
    if (fputc('"', f) == EOF) return false;
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\\' || *p == '"') {
            if (fputc('\\', f) == EOF || fputc(*p, f) == EOF) return false;
        } else if (*p == '\n') {
            if (fputs("\\n", f) == EOF) return false;
        } else if (fputc(*p, f) == EOF) {
            return false;
        }
    }
    return fputc('"', f) != EOF;
}

static bool ident_ok(const char *s, size_t n) {
    if (n == 0) return false;
    unsigned char c = (unsigned char)s[0];
    if (!(isalpha(c) || c == '_')) return false;
    for (size_t i = 1; i < n; i++) {
        c = (unsigned char)s[i];
        if (!(isalnum(c) || c == '_')) return false;
    }
    return true;
}

static bool write_exports(FILE *f, bool *need_bash) {
    if (fputs("FZF_DEFAULT_COMMAND=\nFZF_DEFAULT_OPTS=\nFZF_DEFAULT_OPTS_FILE=\n",
              f) == EOF)
        return false;
    if (!environ) return true;
    for (char **e = environ; *e; e++) {
        char *eq = strchr(*e, '=');
        if (!eq || eq == *e) continue;
        size_t nlen = (size_t)(eq - *e);
        if (nlen >= 16 && strncmp(*e, "__FZF_INTERNAL_", 16) == 0) continue;
        const char *val = eq + 1;
        if (ident_ok(*e, nlen)) {
            if (fputs("export ", f) == EOF) return false;
            if (fwrite(*e, 1, nlen, f) != nlen) return false;
            if (fputc('=', f) == EOF) return false;
            if (!write_shell_quote(f, val)) return false;
            if (fputc('\n', f) == EOF) return false;
        } else if (nlen > 12 && strncmp(*e, "BASH_FUNC_", 10) == 0 &&
                   nlen >= 2 && (*e)[nlen - 2] == '%' && (*e)[nlen - 1] == '%') {
            /* name is the slice between the prefix and the %% suffix. */
            if (nlen < 12) continue;
            size_t name_n = nlen - 12;
            if (fwrite(*e + 10, 1, name_n, f) != name_n) return false;
            if (fputs(val, f) == EOF) return false;
            if (fputc('\n', f) == EOF) return false;
            if (fputs("export -f ", f) == EOF) return false;
            if (fwrite(*e + 10, 1, name_n, f) != name_n) return false;
            if (fputc('\n', f) == EOF) return false;
            *need_bash = true;
        }
    }
    return true;
}

static bool write_command(FILE *f, int argc, char **argv, bool inject_margin,
                          bool inject_border, bool have_input,
                          const char *in_path, const char *out_path) {
    if (argc < 1 || !argv || !argv[0]) return false;
    if (!write_shell_quote(f, argv[0])) return false;
    if (fputs(" '--bind=ctrl-z:ignore'", f) == EOF) return false;
    if (inject_margin && fputs(" '--margin=0,1'", f) == EOF) return false;
    for (int i = 1; i < argc; i++) {
        if (fputc(' ', f) == EOF) return false;
        if (!write_shell_quote(f, argv[i] ? argv[i] : "")) return false;
    }
    if (inject_border && fputs(" '--border=rounded'", f) == EOF) return false;
    if (fputs(" --no-popup --no-height --no-force-tty-in --proxy-script \"$0\"",
              f) == EOF)
        return false;
    if (have_input) {
        if (fputs(" < ", f) == EOF || !write_go_quote(f, in_path)) return false;
    }
    if (fputs(" > ", f) == EOF || !write_go_quote(f, out_path)) return false;
    if (fputc('\n', f) == EOF) return false;
    return true;
}

typedef struct {
    char *path;
    int peer;          /* stdin for the writer; unused for the reader */
    bool to_stdout;
} pipe_task_t;

static void *pipe_task(void *arg) {
    pipe_task_t *task = arg;
    int fifo = open(task->path, task->to_stdout ? O_RDONLY : O_WRONLY);
    if (fifo < 0) {
        free(task->path);
        free(task);
        return NULL;
    }
    char buf[8192];
    if (task->to_stdout) {
        for (;;) {
            ssize_t n = read(fifo, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(STDOUT_FILENO, buf + off, (size_t)(n - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    n = off;
                    break;
                }
                off += w;
            }
        }
    } else {
        for (;;) {
            ssize_t n = read(task->peer, buf, sizeof(buf));
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) break;
            ssize_t off = 0;
            while (off < n) {
                ssize_t w = write(fifo, buf + off, (size_t)(n - off));
                if (w < 0) {
                    if (errno == EINTR) continue;
                    n = off;
                    break;
                }
                off += w;
            }
        }
    }
    close(fifo);
    free(task->path);
    free(task);
    return NULL;
}

static char *make_fifo(const char *name) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    for (int attempt = 0; attempt < 8; attempt++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        unsigned long long ns = (unsigned long long)ts.tv_sec * 1000000000ull +
                                (unsigned long long)ts.tv_nsec + (unsigned)attempt;
        char *path = malloc(PATH_MAX);
        if (!path) return NULL;
        int wrote = snprintf(path, PATH_MAX, "%s/fzf-%s-%llu", dir, name, ns);
        if (wrote < 0 || wrote >= PATH_MAX) {
            free(path);
            return NULL;
        }
        if (mkfifo(path, 0600) == 0) return path;
        free(path);
        if (errno != EEXIST) return NULL;
    }
    return NULL;
}

static int open_keepalive(const char *path) {
    int fd = open(path, O_RDWR);
    if (fd >= 0) fcntl(fd, F_SETFD, FD_CLOEXEC);
    return fd;
}

static bool start_pipe(pthread_t *thread, bool *started, const char *path,
                       bool to_stdout, int peer) {
    pipe_task_t *task = calloc(1, sizeof(*task));
    if (!task) return false;
    task->path = strdup(path);
    task->to_stdout = to_stdout;
    task->peer = peer;
    if (!task->path || pthread_create(thread, NULL, pipe_task, task) != 0) {
        free(task->path);
        free(task);
        return false;
    }
    *started = true;
    return true;
}

static char *make_temp(void) {
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    char *path = malloc(PATH_MAX);
    if (!path) return NULL;
    int wrote = snprintf(path, PATH_MAX, "%s/fzf-temp-XXXXXX", dir);
    if (wrote < 0 || wrote >= PATH_MAX) {
        free(path);
        return NULL;
    }
    int fd = mkstemp(path);
    if (fd < 0) {
        free(path);
        return NULL;
    }
    close(fd);
    return path;
}

static int tmux_dim(double size, bool percent, int window) {
    int dim = (int)size;
    if (percent) dim = window * dim / 100;
    if (dim < 3) dim = 3;
    if (dim > window) dim = window;
    return dim;
}

static bool tmux_floating_info(int *width, int *height) {
    const char *target = getenv("TMUX_PANE");
    if (!target || !*target) return false;
    int fds[2];
    if (pipe(fds) != 0) return false;
    pid_t pid = fork();
    if (pid < 0) {
        close(fds[0]);
        close(fds[1]);
        return false;
    }
    if (pid == 0) {
        dup2(fds[1], STDOUT_FILENO);
        close(fds[0]);
        close(fds[1]);
        execlp("tmux", "tmux", "display-message", "-p", "-t", target,
               "#{window_width} #{window_height}", ";", "list-commands",
               "new-pane", (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    char *buf = NULL;
    size_t len = 0, cap = 0;
    char tmp[1024];
    for (;;) {
        ssize_t n = read(fds[0], tmp, sizeof(tmp));
        if (n < 0) {
            if (errno == EINTR) continue;
            break;
        }
        if (n == 0) break;
        if (len + (size_t)n + 1 > cap) {
            size_t next = cap ? cap * 2 : 2048;
            while (next < len + (size_t)n + 1) next *= 2;
            char *grown = realloc(buf, next);
            if (!grown) break;
            buf = grown;
            cap = next;
        }
        memcpy(buf + len, tmp, (size_t)n);
        len += (size_t)n;
    }
    close(fds[0]);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
    if (buf) buf[len] = '\0';
    bool ok = false;
    if (buf && strstr(buf, "new-pane")) {
        int w = 0, h = 0;
        if (sscanf(buf, "%d %d", &w, &h) == 2 && w >= 3 && h >= 3) {
            *width = w;
            *height = h;
            ok = true;
        }
    }
    free(buf);
    return ok;
}

static char *strip_label(const char *label) {
    if (!label) label = "";
    char *out = malloc(strlen(label) + 1);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; label[i];) {
        if (label[i] == '\033' && label[i + 1] == '[') {
            i += 2;
            while (label[i] && !(label[i] >= '@' && label[i] <= '~')) i++;
            if (label[i]) i++;
            continue;
        }
        out[j++] = label[i++];
    }
    out[j] = '\0';
    return out;
}

static char *escape_tmux_format(const char *s) {
    size_t n = strlen(s ? s : "");
    char *out = malloc(n * 2 + 3);
    if (!out) return NULL;
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '#') out[j++] = '#';
        out[j++] = s[i];
    }
    out[j] = '\0';
    if (j > 0 && out[j - 1] == ';') {
        out[j - 1] = '\\';
        out[j++] = ';';
        out[j] = '\0';
    }
    return out;
}

static const char *base_name(const char *path) {
    const char *slash = strrchr(path, '/');
    return slash ? slash + 1 : path;
}

/* Unset, "line", and "none" all count as no box. fzf draws no frame
   for any of them, so the multiplexer border is the native one. */
static bool no_border_specified(int border_shape) {
    return border_shape < 0 || border_shape == MM_BORDER_LINE ||
           border_shape == MM_BORDER_NONE;
}

static char *build_float_script(const char *sh, const char *temp, const char *dir,
                                const char *label, bool owns_label,
                                int width, int height, int x, int y) {
    char *signal_raw = NULL;
    char *signal = NULL;
    char *code = NULL;
    char *code_path = NULL;
    char *plain = NULL;
    char *formatted = NULL;
    char *label_q = NULL;
    char *format_q = NULL;
    char *sh_q = NULL;
    char *temp_q = NULL;
    char *dir_q = NULL;
    char *target_q = NULL;
    char *resize_q = NULL;
    char *pane = NULL;
    char *pane_q = NULL;
    char *script = NULL;
    const char *target = getenv("TMUX_PANE");
    if (!target) target = "";

    size_t temp_n = strlen(temp);
    code_path = malloc(temp_n + 6);
    signal_raw = malloc(temp_n + 8);
    if (!code_path || !signal_raw) goto done;
    snprintf(code_path, temp_n + 6, "%s.code", temp);
    snprintf(signal_raw, temp_n + 8, "fzf-%s", base_name(temp));
    signal = shell_quote(signal_raw);
    code = shell_quote(code_path);
    plain = strip_label(owns_label ? label : "");
    formatted = plain ? escape_tmux_format(plain) : NULL;
    label_q = formatted ? shell_quote(formatted) : NULL;
    format_q = shell_quote("#{@fzf-border-label}");
    sh_q = shell_quote(sh);
    temp_q = shell_quote(temp);
    dir_q = shell_quote(dir);
    target_q = shell_quote(target);
    if (!signal || !code || !label_q || !format_q || !sh_q || !temp_q ||
        !dir_q || !target_q)
        goto done;

    char resize_raw[PATH_MAX + 32];
    snprintf(resize_raw, sizeof(resize_raw), "resize-pane -Z -t %s", target);
    resize_q = shell_quote(resize_raw);
    if (!resize_q) goto done;

    /* The pane command is one single-quoted argument of sh -c. */
    size_t pane_cap = strlen(label_q) + strlen(format_q) + strlen(sh_q) +
                      strlen(temp_q) + strlen(code) + strlen(signal) + 1024;
    pane = malloc(pane_cap);
    if (!pane) goto done;
    snprintf(pane, pane_cap,
             "tmux set-option -p -t \"$TMUX_PANE\" remain-on-exit off 2> /dev/null; "
             "tmux set-option -p -t \"$TMUX_PANE\" @fzf-border-label %s 2> /dev/null; "
             "tmux set-option -p -t \"$TMUX_PANE\" pane-border-format %s 2> /dev/null; "
             "%s%s %s; echo $? > %s; tmux wait-for -S %s",
             label_q, format_q,
             owns_label ? "export __FZF_INTERNAL_TMUX_LABEL_PANE=\"$TMUX_PANE\"; " : "",
             sh_q, temp_q, code, signal);
    pane_q = shell_quote(pane);
    if (!pane_q) goto done;

    size_t script_cap = strlen(pane_q) + strlen(target_q) * 2 + strlen(resize_q) +
                        strlen(dir_q) + strlen(sh_q) + strlen(code) * 4 +
                        strlen(signal) * 2 + 2048;
    script = malloc(script_cap);
    if (!script) goto done;
    snprintf(script, script_cap,
             "trap '[ -n \"$id\" ] && tmux kill-pane -t \"$id\" 2> /dev/null' INT TERM HUP\n"
             "id=$(tmux if -F -t %s '#{window_zoomed_flag}' %s ';' "
             "new-pane -P -F '#{pane_id}' -t %s -c %s -x %d -y %d -X %d -Y %d %s -c %s) "
             "|| { status=$?; rm -f %s; exit \"$status\"; }\n"
             "{ while tmux has-session -t \"$id\" 2> /dev/null; do sleep 1; done; "
             "tmux wait-for -S %s; } &\n"
             "watchdog=$!\n"
             "tmux wait-for %s &\n"
             "waiter=$!\n"
             "wait \"$waiter\"\n"
             "kill \"$watchdog\" \"$waiter\" 2> /dev/null\n"
             "wait 2> /dev/null\n"
             "if [ -s %s ]; then code=$(cat %s); else code=130; fi\n"
             "rm -f %s\n"
             "exit \"$code\"",
             target_q, resize_q, target_q, dir_q, width - 2, height - 2, x, y,
             sh_q, pane_q, code, signal, signal, code, code, code);

done:
    free(signal_raw);
    free(signal);
    free(code);
    free(code_path);
    free(plain);
    free(formatted);
    free(label_q);
    free(format_q);
    free(sh_q);
    free(temp_q);
    free(dir_q);
    free(target_q);
    free(resize_q);
    free(pane);
    free(pane_q);
    return script;
}

static int spawn_and_wait(const char *file, char *const args[]) {
    char *path = lookup_bin(file);
    if (!path) return 0;
    pid_t pid = fork();
    if (pid < 0) {
        free(path);
        fprintf(stderr, "fzf: %s\n", strerror(errno));
        return 2;
    }
    if (pid == 0) {
        execv(path, args);
        _exit(127);
    }
    free(path);
    struct sigaction sa, old;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = popup_on_int;
    sigemptyset(&sa.sa_mask);
    popup_child_pid = (sig_atomic_t)pid;
    sigaction(SIGINT, &sa, &old);
    int status = 0;
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) break;
    }
    sigaction(SIGINT, &old, NULL);
    popup_child_pid = 0;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return 2;
}

static int become_exec(const char *become_path, const char *with_shell) {
    FILE *f = fopen(become_path, "rb");
    if (!f) {
        fprintf(stderr, "open %s: %s\n", become_path, strerror(errno));
        return 2;
    }
    char *data = NULL;
    size_t len = 0, cap = 0;
    char tmp[4096];
    for (;;) {
        size_t n = fread(tmp, 1, sizeof(tmp), f);
        if (n) {
            if (len + n + 1 > cap) {
                size_t next = cap ? cap * 2 : 4096;
                while (next < len + n + 1) next *= 2;
                char *grown = realloc(data, next);
                if (!grown) {
                    free(data);
                    fclose(f);
                    fprintf(stderr, "fzf: out of memory\n");
                    return 2;
                }
                data = grown;
                cap = next;
            }
            memcpy(data + len, tmp, n);
            len += n;
        }
        if (n < sizeof(tmp)) break;
    }
    fclose(f);
    if (!data || len == 0) {
        free(data);
        fprintf(stderr, "invalid become command\n");
        return 2;
    }
    data[len] = '\0';
    /* Count NUL-separated fields. The bytes already contain interior NULs. */
    size_t fields = 1;
    for (size_t i = 0; i < len; i++) if (data[i] == '\0') fields++;
    /* A trailing NUL from our terminator is extra. Drop it. */
    if (len > 0 && data[len - 1] == '\0') fields--;
    char **elems = calloc(fields + 1, sizeof(char *));
    if (!elems) {
        free(data);
        return 2;
    }
    size_t fi = 0;
    elems[fi++] = data;
    for (size_t i = 0; i + 1 < len && fi < fields; i++) {
        if (data[i] == '\0') elems[fi++] = data + i + 1;
    }
    const char *command = elems[0];
    const char *shell = getenv("SHELL");
    if (!shell || !*shell) shell = "sh";
    char *shell_dup = NULL;
    char *extra_arg = NULL;
    bool use_c = true;
    if (with_shell && *with_shell) {
        /* First word is the shell. A following word replaces the default -c. */
        shell_dup = strdup(with_shell);
        if (!shell_dup) {
            free(elems);
            free(data);
            return 2;
        }
        char *sp = strchr(shell_dup, ' ');
        if (sp) {
            *sp = '\0';
            extra_arg = sp + 1;
            while (*extra_arg == ' ') extra_arg++;
            if (!*extra_arg) extra_arg = NULL;
        }
        shell = shell_dup;
        use_c = false;
    }
    char *shell_path = lookup_bin(shell);
    if (!shell_path) {
        fprintf(stderr, "fzf (become): exec: \"%s\": executable file not found in $PATH\n",
                shell);
        free(shell_dup);
        free(elems);
        free(data);
        return 127;
    }
    size_t env_n = fi > 0 ? fi - 1 : 0;
    char **envp = calloc(env_n + 1, sizeof(char *));
    char **argv = calloc(6, sizeof(char *));
    if (!envp || !argv) {
        free(envp);
        free(argv);
        free(shell_path);
        free(shell_dup);
        free(elems);
        free(data);
        return 2;
    }
    for (size_t i = 1; i < fi; i++) envp[i - 1] = elems[i];
    int a = 0;
    argv[a++] = shell_path;
    if (extra_arg) argv[a++] = extra_arg;
    else if (use_c) argv[a++] = (char *)"-c";
    argv[a++] = (char *)command;
    argv[a] = NULL;
    execve(shell_path, argv, envp);
    fprintf(stderr, "fzf (become): %s\n", strerror(errno));
    return 127;
}

bool mm_popup_launch(const mm_popup_opts_t *opt, int height_index,
                     bool margin_default, int border_shape,
                     const char *border_label, const char *with_shell,
                     bool force_tty_in, int argc, char **argv, int *code) {
    if (!opt || !opt->set || !code) return false;
    const char *tmux_env = getenv("TMUX");
    const char *zellij_env = getenv("ZELLIJ");
    bool tmux_on = tmux_env && *tmux_env && opt->index >= height_index;
    bool zellij_on = !tmux_on && zellij_env && *zellij_env &&
                     opt->index >= height_index;
    if (!tmux_on && !zellij_on) return false;

    mm_popup_opts_t o = *opt;
    bool native = o.border || no_border_specified(border_shape);
    int win_w = 0, win_h = 0;
    bool floating = false;
    if (tmux_on && native && tmux_floating_info(&win_w, &win_h)) {
        o.border = true;
        floating = true;
    }
    if (zellij_on && native) o.border = true;

    bool inject_border = !o.border &&
        (border_shape < 0 || border_shape == MM_BORDER_LINE);
    bool inject_margin = o.border && margin_default;
    bool tty_in = force_tty_in || isatty(STDIN_FILENO);

    char *out_fifo = NULL;
    char *in_fifo = NULL;
    int out_keep = -1, in_keep = -1;
    pthread_t out_th, in_th;
    bool out_started = false, in_started = false;
    char *temp = NULL;
    char *become = NULL;
    char dir[PATH_MAX];
    int status = 0;

    out_fifo = make_fifo("proxy-output");
    if (!out_fifo) {
        fprintf(stderr, "%s\n", strerror(errno));
        *code = 2;
        return true;
    }
    out_keep = open_keepalive(out_fifo);
    if (out_keep < 0) {
        fprintf(stderr, "%s\n", strerror(errno));
        status = 2;
        goto cleanup;
    }
    if (!start_pipe(&out_th, &out_started, out_fifo, true, -1)) {
        fprintf(stderr, "fzf: out of memory\n");
        status = 2;
        goto cleanup;
    }
    if (!tty_in) {
        in_fifo = make_fifo("proxy-input");
        if (!in_fifo) {
            fprintf(stderr, "%s\n", strerror(errno));
            status = 2;
            goto cleanup;
        }
        in_keep = open_keepalive(in_fifo);
        if (in_keep < 0) {
            fprintf(stderr, "%s\n", strerror(errno));
            status = 2;
            goto cleanup;
        }
        if (!start_pipe(&in_th, &in_started, in_fifo, false, STDIN_FILENO)) {
            fprintf(stderr, "fzf: out of memory\n");
            status = 2;
            goto cleanup;
        }
    }

    temp = make_temp();
    if (!temp) {
        fprintf(stderr, "%s\n", strerror(errno));
        status = 2;
        goto cleanup;
    }
    FILE *tf = fopen(temp, "w");
    if (!tf) {
        fprintf(stderr, "%s\n", strerror(errno));
        status = 2;
        goto cleanup;
    }
    bool need_bash = false;
    bool wrote = write_exports(tf, &need_bash) &&
                 write_command(tf, argc, argv, inject_margin, inject_border,
                               in_fifo != NULL, in_fifo, out_fifo);
    if (fclose(tf) != 0) wrote = false;
    if (!wrote) {
        fprintf(stderr, "fzf: failed to write temporary file\n");
        status = 2;
        goto cleanup;
    }

    size_t become_n = strlen(temp) + 8;
    become = malloc(become_n);
    if (!become) {
        status = 2;
        goto cleanup;
    }
    snprintf(become, become_n, "%s.become", temp);
    int bfd = open(become, O_CREAT | O_EXCL | O_WRONLY, 0600);
    if (bfd < 0) {
        fprintf(stderr, "%s\n", strerror(errno));
        status = 2;
        goto cleanup;
    }
    close(bfd);

    if (!getcwd(dir, sizeof(dir))) snprintf(dir, sizeof(dir), ".");
    const char *sh = need_bash ? "bash" : "sh";

    if (zellij_on) {
        char width[64], height[64];
        format_size(width, sizeof(width), o.width, o.width_percent);
        format_size(height, sizeof(height), o.height, o.height_percent);
        char name[PATH_MAX];
        char *plain = NULL;
        bool owns = o.border && no_border_specified(border_shape);
        if (owns) plain = strip_label(border_label);
        const char *zargs[20];
        int n = 0;
        zargs[n++] = "zellij";
        zargs[n++] = "run";
        zargs[n++] = "--floating";
        zargs[n++] = "--close-on-exit";
        zargs[n++] = "--block-until-exit";
        zargs[n++] = "--cwd";
        zargs[n++] = dir;
        if (!o.border) {
            zargs[n++] = "--borderless";
            zargs[n++] = "true";
        } else {
            snprintf(name, sizeof(name), "--name=%s", plain ? plain : "");
            zargs[n++] = name;
        }
        if (o.position == MM_POPUP_UP) {
            zargs[n++] = "-y";
            zargs[n++] = "0";
        } else if (o.position == MM_POPUP_DOWN) {
            zargs[n++] = "-y";
            zargs[n++] = "9999";
        } else if (o.position == MM_POPUP_LEFT) {
            zargs[n++] = "-x";
            zargs[n++] = "0";
        } else if (o.position == MM_POPUP_RIGHT) {
            zargs[n++] = "-x";
            zargs[n++] = "9999";
        }
        zargs[n++] = "--width";
        zargs[n++] = width;
        zargs[n++] = "--height";
        zargs[n++] = height;
        zargs[n++] = "--";
        char *sh_q = shell_quote(sh);
        char *temp_q = shell_quote(temp);
        char cmd[PATH_MAX * 2];
        if (owns && sh_q && temp_q) {
            snprintf(cmd, sizeof(cmd),
                     "export __FZF_INTERNAL_ZELLIJ_LABEL_PANE=\"$ZELLIJ_PANE_ID\"; exec %s %s",
                     sh_q, temp_q);
            zargs[n++] = sh;
            zargs[n++] = "-c";
            zargs[n++] = cmd;
        } else {
            zargs[n++] = sh;
            zargs[n++] = temp;
        }
        zargs[n] = NULL;
        status = spawn_and_wait("zellij", (char *const *)zargs);
        free(sh_q);
        free(temp_q);
        free(plain);
    } else if (floating) {
        int width = tmux_dim(o.width, o.width_percent, win_w);
        int height = tmux_dim(o.height, o.height_percent, win_h);
        int x = (win_w - width) / 2 + 1;
        int y = (win_h - height) / 2 + 1;
        if (o.position == MM_POPUP_UP) y = 1;
        else if (o.position == MM_POPUP_DOWN) y = win_h - height + 1;
        else if (o.position == MM_POPUP_LEFT) x = 1;
        else if (o.position == MM_POPUP_RIGHT) x = win_w - width + 1;
        /* Pre-create the exit-status file before the pane can run. */
        char code_path[PATH_MAX];
        snprintf(code_path, sizeof(code_path), "%s.code", temp);
        int cfd = open(code_path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        if (cfd < 0) {
            fprintf(stderr, "%s\n", strerror(errno));
            status = 2;
            goto cleanup;
        }
        close(cfd);
        char *script = build_float_script(sh, temp, dir, border_label,
                                          no_border_specified(border_shape),
                                          width, height, x, y);
        if (!script) {
            unlink(code_path);
            fprintf(stderr, "fzf: out of memory\n");
            status = 2;
            goto cleanup;
        }
        char *sargs[] = { (char *)sh, (char *)"-c", script, NULL };
        status = spawn_and_wait(sh, sargs);
        free(script);
        unlink(code_path);
    } else {
        char width[64], height[64];
        format_size(width, sizeof(width), o.width, o.width_percent);
        format_size(height, sizeof(height), o.height, o.height_percent);
        char warg[80], harg[80];
        snprintf(warg, sizeof(warg), "-w%s", width);
        snprintf(harg, sizeof(harg), "-h%s", height);
        const char *targs[16];
        int n = 0;
        targs[n++] = "tmux";
        targs[n++] = "display-popup";
        targs[n++] = "-E";
        targs[n++] = "-d";
        targs[n++] = dir;
        if (!o.border) targs[n++] = "-B";
        if (o.position == MM_POPUP_UP) {
            targs[n++] = "-xC";
            targs[n++] = "-y0";
        } else if (o.position == MM_POPUP_DOWN) {
            targs[n++] = "-xC";
            targs[n++] = "-y9999";
        } else if (o.position == MM_POPUP_LEFT) {
            targs[n++] = "-x0";
            targs[n++] = "-yC";
        } else if (o.position == MM_POPUP_RIGHT) {
            targs[n++] = "-xR";
            targs[n++] = "-yC";
        } else {
            targs[n++] = "-xC";
            targs[n++] = "-yC";
        }
        targs[n++] = warg;
        targs[n++] = harg;
        targs[n++] = sh;
        targs[n++] = temp;
        targs[n] = NULL;
        status = spawn_and_wait("tmux", (char *const *)targs);
    }

    if (status == 126 && become) {
        int bcode = become_exec(become, with_shell);
        status = bcode;
    }

cleanup:
    if (out_keep >= 0) close(out_keep);
    if (in_keep >= 0) close(in_keep);
    if (out_started) pthread_join(out_th, NULL);
    if (in_started) pthread_join(in_th, NULL);
    if (out_fifo) {
        unlink(out_fifo);
        free(out_fifo);
    }
    if (in_fifo) {
        unlink(in_fifo);
        free(in_fifo);
    }
    if (become) {
        unlink(become);
        free(become);
    }
    if (temp) {
        unlink(temp);
        free(temp);
    }
    *code = status;
    return true;
}
