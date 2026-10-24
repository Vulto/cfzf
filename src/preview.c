/* preview.c - run the preview command and collect its output. */
#define _POSIX_C_SOURCE 200809L
#include "preview.h"

#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdio.h>

struct mm_preview_t {
    char *command;
    char *shell;
    char *selected;   /* the selected items, space separated ({+}) */
    char *tmpfile;    /* a file holding the selected items ({f}) */
    pid_t pid;
    int   fd;          /* read end of the pipe, -1 when idle */
    char *buf;
    size_t len, cap;
    bool  running;
};

bool mm_preview_window_parse(const char *spec, mm_preview_win_t *out) {
    /* Defaults: right, 50%, with a border. */
    out->pos = MM_PREVIEW_RIGHT;
    out->size = 50;
    out->border = true;
    out->border_left_only = false;
    out->info = true;
    out->wrap = false;
    out->cycle = false;

    if (!spec || !*spec) return true;
    const char *p = spec;
    while (*p) {
        /* fzf uses comma-separated options (right,58%,border-left,wrap).
           Accept the earlier colon-separated form as well. */
        const char *sep = strpbrk(p, ",:");
        size_t len = sep ? (size_t)(sep - p) : strlen(p);
        char tok[64];
        if (len >= sizeof(tok)) return false;
        memcpy(tok, p, len);
        tok[len] = 0;
        if (len > 0) {
            if (!strcmp(tok, "right")) out->pos = MM_PREVIEW_RIGHT;
            else if (!strcmp(tok, "left")) out->pos = MM_PREVIEW_LEFT;
            else if (!strcmp(tok, "top") || !strcmp(tok, "up")) out->pos = MM_PREVIEW_TOP;
            else if (!strcmp(tok, "bottom") || !strcmp(tok, "down")) out->pos = MM_PREVIEW_BOTTOM;
            else if (!strcmp(tok, "hidden")) { /* the preview is hidden */ }
            else if (!strcmp(tok, "border")) {
                out->border = true;
                out->border_left_only = false;
            } else if (!strcmp(tok, "border-left")) {
                out->border = true;
                out->border_left_only = true;
            } else if (!strcmp(tok, "noborder")) {
                out->border = false;
                out->border_left_only = false;
            } else if (!strcmp(tok, "info")) out->info = true;
            else if (!strcmp(tok, "noinfo")) out->info = false;
            else if (!strcmp(tok, "wrap")) out->wrap = true;
            else if (!strcmp(tok, "nowrap")) out->wrap = false;
            else if (!strcmp(tok, "wrap-word")) out->wrap = true;
            else if (!strcmp(tok, "cycle")) out->cycle = true;
            else if (!strcmp(tok, "nocycle")) out->cycle = false;
            else if (!strcmp(tok, "nohidden") || !strcmp(tok, "default")) {
                /* Explicit defaults; no additional state is needed here. */
            } else if (tok[0] >= '0' && tok[0] <= '9') {
                /* A size: "58%" or a number of lines. */
                char *end = NULL;
                long v = strtol(tok, &end, 10);
                if (end == tok || v <= 0 || v > 100 ||
                    (*end != '\0' && (strcmp(end, "%") != 0))) return false;
                out->size = (int)v;
            } else {
                return false;
            }
        }
        if (!sep) break;
        p = sep + 1;
    }
    return true;
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
    if (p->tmpfile) { unlink(p->tmpfile); free(p->tmpfile); }
    free(p->buf);
    free(p);
}

/* Expand the fzf placeholders in the command template. */
/* Write the selected items to a temporary file and return its path. */
static char *preview_tmpfile(mm_preview_t *p, const char *selected) {
    if (p->tmpfile) return p->tmpfile;
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    size_t n = strlen(dir) + 32;
    char *path = malloc(n);
    if (!path) return NULL;
    snprintf(path, n, "%s/fzf-preview-XXXXXX", dir);
    int fd = mkstemp(path);
    if (fd < 0) { free(path); return NULL; }
    if (selected && *selected) {
        size_t len = strlen(selected);
        if (write(fd, selected, len) < 0) { /* best effort */ }
    }
    close(fd);
    p->tmpfile = path;
    return path;
}

static char *preview_expand(mm_preview_t *p, const char *cmd, const char *item,
                            const char *query, int index) {
    size_t cap = strlen(cmd) * 2 + 64;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; cmd[i]; i++) {
        if (cmd[i] == '{' && cmd[i + 1] == '}') {
            const char *rep = item ? item : "";
            size_t rl = strlen(rep);
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, rep, rl);
            o += rl;
            i++;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'q' && cmd[i + 2] == '}') {
            size_t rl = query ? strlen(query) : 0;
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, query ? query : "", rl);
            o += rl;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == '+' && cmd[i + 2] == '}') {
            const char *rep = p->selected ? p->selected : "";
            size_t rl = strlen(rep);
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, rep, rl);
            o += rl;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'f' && cmd[i + 2] == '}') {
            const char *rep = preview_tmpfile(p, p->selected);
            size_t rl = rep ? strlen(rep) : 0;
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            if (rep) memcpy(out + o, rep, rl);
            o += rl;
            i += 2;
            continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'r' && cmd[i + 2] == '}') {
            /* The 1-based row of the current selection. */
            char num[32];
            int nl = snprintf(num, sizeof(num), "%d", index + 1);
            const char *rep = num;
            size_t rl = (size_t)nl;
            while (o + rl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, rep, rl); o += rl; i += 2; continue;
        }
        if (cmd[i] == '{' && cmd[i + 1] == 'n' && cmd[i + 2] == '}') {
            char num[32];
            int nl = snprintf(num, sizeof(num), "%d", index);
            while (o + (size_t)nl + 2 > cap) { cap *= 2; out = realloc(out, cap); }
            memcpy(out + o, num, (size_t)nl);
            o += (size_t)nl;
            i += 2;
            continue;
        }
        if (o + 2 > cap) { cap *= 2; out = realloc(out, cap); }
        out[o++] = cmd[i];
    }
    out[o] = 0;
    return out;
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

const char *mm_preview_output(mm_preview_t *p) {
    if (!p || !p->buf) return "";
    return p->buf;
}

void mm_preview_set_selected(mm_preview_t *p, const char *selected) {
    if (!p) return;
    free(p->selected);
    p->selected = selected ? strdup(selected) : NULL;
}

void mm_preview_set_command(mm_preview_t *p, const char *command) {
    if (!p) return;
    free(p->command);
    p->command = command ? strdup(command) : NULL;
}
