/* preview.c - run the preview command and collect its output. */
#define _POSIX_C_SOURCE 200809L
#include "preview.h"
#include "terminal.h"
#include "util.h"

#include <limits.h>

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <errno.h>

struct mm_preview_t {
    char *command;
    char *shell;
    char *selected;        /* raw newline-separated items for {f} */
    char *selected_shell;   /* shell-quoted arguments for {+} */
    char *tmpfile;          /* a file holding the selected items ({f}) */
    pid_t pid;
    int   fd;          /* read end of the pipe, -1 when idle */
    char *buf;
    size_t len, cap;
    bool  running;
};

void mm_preview_window_reset(mm_preview_win_t *out) {
    /* Defaults: right, 50%, the --preview-border shape, visible. */
    out->pos = MM_PREVIEW_RIGHT;
    out->size = 50;
    out->size_percent = true;
    out->border_shape = -1;
    out->hidden = false;
    out->info = true;
    out->wrap = false;
    out->wrap_word = false;
    out->cycle = false;
}

/* One comma-separated token. Returns false when fzf would reject it. */
static bool preview_apply_token(const char *tok, mm_preview_win_t *out) {
    if (!strcmp(tok, "right")) out->pos = MM_PREVIEW_RIGHT;
    else if (!strcmp(tok, "left")) out->pos = MM_PREVIEW_LEFT;
    else if (!strcmp(tok, "top") || !strcmp(tok, "up")) out->pos = MM_PREVIEW_TOP;
    else if (!strcmp(tok, "bottom") || !strcmp(tok, "down")) out->pos = MM_PREVIEW_BOTTOM;
    else if (!strcmp(tok, "hidden")) out->hidden = true;
    else if (!strcmp(tok, "nohidden")) out->hidden = false;
    else if (!strcmp(tok, "default")) mm_preview_window_reset(out);
    else if (!strcmp(tok, "border") || !strcmp(tok, "rounded") ||
             !strcmp(tok, "border-rounded"))
        out->border_shape = MM_BORDER_ROUNDED;
    else if (!strcmp(tok, "sharp") || !strcmp(tok, "border-sharp"))
        out->border_shape = MM_BORDER_SHARP;
    else if (!strcmp(tok, "border-bold")) out->border_shape = MM_BORDER_BOLD;
    else if (!strcmp(tok, "border-double")) out->border_shape = MM_BORDER_DOUBLE;
    else if (!strcmp(tok, "border-block")) out->border_shape = MM_BORDER_BLOCK;
    else if (!strcmp(tok, "border-thinblock")) out->border_shape = MM_BORDER_THINBLOCK;
    else if (!strcmp(tok, "border-dashed")) out->border_shape = MM_BORDER_DASHED;
    else if (!strcmp(tok, "border-horizontal")) out->border_shape = MM_BORDER_HORIZONTAL;
    else if (!strcmp(tok, "border-vertical")) out->border_shape = MM_BORDER_VERTICAL;
    else if (!strcmp(tok, "border-top") || !strcmp(tok, "border-up"))
        out->border_shape = MM_BORDER_TOP;
    else if (!strcmp(tok, "border-bottom") || !strcmp(tok, "border-down"))
        out->border_shape = MM_BORDER_BOTTOM;
    else if (!strcmp(tok, "border-left")) out->border_shape = MM_BORDER_LEFT;
    else if (!strcmp(tok, "border-right")) out->border_shape = MM_BORDER_RIGHT;
    else if (!strcmp(tok, "noborder") || !strcmp(tok, "border-none"))
        out->border_shape = MM_BORDER_NONE;
    else if (!strcmp(tok, "info")) out->info = true;
    else if (!strcmp(tok, "noinfo")) out->info = false;
    else if (!strcmp(tok, "wrap")) { out->wrap = true; out->wrap_word = false; }
    else if (!strcmp(tok, "nowrap")) { out->wrap = false; out->wrap_word = false; }
    else if (!strcmp(tok, "wrap-word")) { out->wrap = true; out->wrap_word = true; }
    else if (!strcmp(tok, "cycle")) out->cycle = true;
    else if (!strcmp(tok, "nocycle")) out->cycle = false;
    else if (tok[0] >= '0' && tok[0] <= '9') {
        /* "58%" is a percentage. A bare number is columns or lines. */
        char *end = NULL;
        long v = strtol(tok, &end, 10);
        if (end == tok || v < 0 || v > INT_MAX) return false;
        if (strcmp(end, "%") == 0) {
            if (v > 100) return false;
            out->size = (int)v;
            out->size_percent = true;
        } else if (*end == '\0') {
            out->size = (int)v;
            out->size_percent = false;
        } else {
            return false;
        }
    } else {
        return false;
    }
    return true;
}

bool mm_preview_window_apply(const char *spec, mm_preview_win_t *out) {
    if (!spec || !*spec) return true;
    const char *p = spec;
    while (*p) {
        /* fzf uses comma-separated options (right,58%,border-left,wrap).
           Accept the earlier colon-separated form as well. */
        const char *sep = strpbrk(p, ",:");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        char tok[64];
        if (len >= sizeof(tok)) return false;
        if (len > 0) {
            memcpy(tok, p, len);
            tok[len] = 0;
            if (!preview_apply_token(tok, out)) return false;
        }
        if (!sep) break;
        p = sep + 1;
    }
    return true;
}

bool mm_preview_window_parse(const char *spec, mm_preview_win_t *out) {
    mm_preview_window_reset(out);
    return mm_preview_window_apply(spec, out);
}

mm_preview_t *mm_preview_new(const char *command, const char *shell) {
    mm_preview_t *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->command = command ? strdup(command) : NULL;
    p->shell = shell ? strdup(shell) : NULL;
    p->pid = -1;
    p->fd = -1;
    return p;
}

void mm_preview_free(mm_preview_t *p) {
    if (!p) return;
    if (p->fd >= 0) close(p->fd);
    if (p->pid > 0) { kill(p->pid, SIGTERM); waitpid(p->pid, NULL, 0); }
    free(p->command);
    free(p->shell);
    free(p->selected);
    free(p->selected_shell);
    if (p->tmpfile) { unlink(p->tmpfile); free(p->tmpfile); }
    free(p->buf);
    free(p);
}

/* Append bytes to a command string while checking size arithmetic. */
static bool preview_append(char **out, size_t *length, size_t *capacity,
                           const char *text, size_t n) {
    if (n > SIZE_MAX - *length - 1) return false;
    size_t need = *length + n + 1;
    if (need > *capacity) {
        size_t cap = *capacity ? *capacity : 64;
        while (cap < need) {
            if (cap > SIZE_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        char *grown = realloc(*out, cap);
        if (!grown) return false;
        *out = grown;
        *capacity = cap;
    }
    if (n) memcpy(*out + *length, text, n);
    *length += n;
    (*out)[*length] = '\0';
    return true;
}

/* Write all selected records, handling short writes and interrupted calls. */
static bool preview_write_selected(int fd, const char *selected) {
    size_t len = selected ? strlen(selected) : 0;
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, selected + written, len - written);
        if (n > 0) {
            written += (size_t)n;
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
    return true;
}

/* Write selected records to a reusable file; refreshed selections must not
   leave the contents from the previous invocation. */
static char *preview_tmpfile(mm_preview_t *p, const char *selected) {
    if (p->tmpfile) {
        int fd = open(p->tmpfile, O_WRONLY | O_TRUNC);
        if (fd < 0) return NULL;
        bool ok = preview_write_selected(fd, selected);
        close(fd);
        return ok ? p->tmpfile : NULL;
    }
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t dir_len = strlen(dir);
    if (dir_len > SIZE_MAX - 32) return NULL;
    char *path = malloc(dir_len + 32);
    if (!path) return NULL;
    snprintf(path, dir_len + 32, "%s/fzf-preview-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    if (!preview_write_selected(fd, selected)) {
        close(fd);
        unlink(path);
        free(path);
        return NULL;
    }
    close(fd);
    p->tmpfile = path;
    return path;
}

/* Replace preview placeholders with shell-safe arguments. {r} is raw item
   text, while {n} is its zero-based input index. Backslash escapes a slot. */
static char *preview_expand(mm_preview_t *p, const char *cmd, const char *item,
                            const char *query, int index) {
    char *out = NULL;
    size_t length = 0, capacity = 0;
    const char *raw_item = item ? item : "";
    const char *raw_query = query ? query : "";

    for (size_t i = 0; cmd[i]; i++) {
        if (cmd[i] == '\\' && cmd[i + 1] == '{') {
            size_t literal_len = 0;
            if (cmd[i + 2] == '}') literal_len = 2;
            else if ((cmd[i + 2] == 'q' || cmd[i + 2] == '+' ||
                      cmd[i + 2] == 'f' || cmd[i + 2] == 'r' ||
                      cmd[i + 2] == 'n') && cmd[i + 3] == '}')
                literal_len = 3;
            if (literal_len) {
                if (!preview_append(&out, &length, &capacity,
                                    cmd + i + 1, literal_len))
                    goto fail;
                i += literal_len;
                continue;
            }
        }

        const char *value = NULL;
        char *owned = NULL;
        size_t slot_len = 0;
        if (cmd[i] == '{' && cmd[i + 1] == '}') {
            owned = mm_shell_quote(raw_item);
            slot_len = 2;
        } else if (cmd[i] == '{' && cmd[i + 1] == 'q' && cmd[i + 2] == '}') {
            owned = mm_shell_quote(raw_query);
            slot_len = 3;
        } else if (cmd[i] == '{' && cmd[i + 1] == '+' && cmd[i + 2] == '}') {
            if (p->selected_shell) value = p->selected_shell;
            else {
                owned = mm_shell_quote(raw_item);
                value = owned;
            }
            slot_len = 3;
        } else if (cmd[i] == '{' && cmd[i + 1] == 'f' && cmd[i + 2] == '}') {
            const char *path = preview_tmpfile(p, p->selected ? p->selected : "");
            owned = mm_shell_quote(path ? path : "");
            slot_len = 3;
        } else if (cmd[i] == '{' && cmd[i + 1] == 'r' && cmd[i + 2] == '}') {
            value = raw_item;
            slot_len = 3;
        } else if (cmd[i] == '{' && cmd[i + 1] == 'n' && cmd[i + 2] == '}') {
            char number[32];
            int n = snprintf(number, sizeof(number), "%d", index);
            if (n < 0 || (size_t)n >= sizeof(number) ||
                !preview_append(&out, &length, &capacity, number, (size_t)n))
                goto fail;
            i += 2;
            continue;
        } else {
            if (!preview_append(&out, &length, &capacity, cmd + i, 1))
                goto fail;
            continue;
        }

        if (owned) value = owned;
        if (!value) { free(owned); goto fail; }
        if (!preview_append(&out, &length, &capacity, value, strlen(value))) {
            free(owned);
            goto fail;
        }
        free(owned);
        i += slot_len - 1;
    }

    if (!out) return strdup("");
    return out;

fail:
    free(out);
    return NULL;
}

void mm_preview_start(mm_preview_t *p, const char *item, const char *query, int index) {
    if (!p || !p->command) return;
    /* Cancel a running command. */
    if (p->fd >= 0) { close(p->fd); p->fd = -1; }
    if (p->pid > 0) { kill(p->pid, SIGTERM); waitpid(p->pid, NULL, 0); p->pid = -1; }
    p->running = false;
    p->len = 0;
    if (p->buf) p->buf[0] = 0;

    char *cmd = preview_expand(p, p->command, item, query, index);
    if (!cmd) return;
    int fds[2];
    if (pipe(fds) != 0) { free(cmd); return; }
    pid_t pid = fork();
    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        if (p->shell && *p->shell)
            execl(p->shell, p->shell, "-c", cmd, (char *)NULL);
        else
            execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
        _exit(127);
    }
    close(fds[1]);
    free(cmd);
    p->pid = pid;
    p->fd = fds[0];
    p->running = true;
    int fl = fcntl(p->fd, F_GETFL, 0);
    fcntl(p->fd, F_SETFL, fl | O_NONBLOCK);
}

bool mm_preview_poll(mm_preview_t *p) {
    if (!p || p->fd < 0) return false;
    bool got = false;
    for (;;) {
        char tmp[4096];
        ssize_t n = read(p->fd, tmp, sizeof(tmp));
        if (n > 0) {
            if (p->len + (size_t)n + 1 > p->cap) {
                size_t nc = p->cap ? p->cap : 8192;
                while (p->len + (size_t)n + 1 > nc) nc *= 2;
                p->buf = realloc(p->buf, nc);
                p->cap = nc;
            }
            memcpy(p->buf + p->len, tmp, (size_t)n);
            p->len += (size_t)n;
            p->buf[p->len] = 0;
            got = true;
            continue;
        }
        if (n == 0) {
            close(p->fd);
            p->fd = -1;
            if (p->pid > 0) { waitpid(p->pid, NULL, 0); p->pid = -1; }
            p->running = false;
            got = true;
        }
        break;
    }
    return got;
}

bool mm_preview_drain(mm_preview_t *p, int timeout_ms) {
    if (!p || p->fd < 0) return false;
    struct pollfd pfd = { p->fd, POLLIN, 0 };
    int rc;
    do {
        rc = poll(&pfd, 1, timeout_ms);
    } while (rc < 0 && errno == EINTR);
    if (rc <= 0) return false;
    return mm_preview_poll(p);
}

const char *mm_preview_output(mm_preview_t *p) {
    if (!p || !p->buf) return "";
    return p->buf;
}

void mm_preview_set_selected(mm_preview_t *p, const char *selected,
                             const char *selected_shell) {
    if (!p) return;
    char *raw_copy = strdup(selected ? selected : "");
    char *shell_copy = strdup(selected_shell ? selected_shell : "");
    if (!raw_copy || !shell_copy) {
        free(raw_copy);
        free(shell_copy);
        return;
    }
    free(p->selected);
    free(p->selected_shell);
    p->selected = raw_copy;
    p->selected_shell = shell_copy;
}

void mm_preview_set_command(mm_preview_t *p, const char *command) {
    if (!p) return;
    free(p->command);
    p->command = command ? strdup(command) : NULL;
}
