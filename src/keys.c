/* keys.c - terminal input decoding.
   Port of the escape-sequence handling in fzf src/tui/light.go
   (GetChar / escSequence / mouseSequence). */
#define _POSIX_C_SOURCE 200809L
#include "event.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <poll.h>
#include <errno.h>
#include <time.h>

#define MAX_INPUT_BUFFER (1024 * 1024)
#define ESC_POLL_INTERVAL 5   /* ms */
#define ESC_DELAY_DEFAULT 100 /* ms */

typedef enum {
    MM_GET_CHAR_SUCCESS,
    MM_GET_CHAR_ERROR,
    MM_GET_CHAR_CANCELLED,
} mm_getchar_result_t;

static bool input_push(mm_keybuf_t *in, const char *data, size_t n) {
    if (in->len + n + 1 > in->cap) {
        size_t nc = in->cap ? in->cap : 256;
        while (in->len + n + 1 > nc) nc *= 2;
        char *nb = realloc(in->buf, nc);
        if (!nb) return false;
        in->buf = nb;
        in->cap = nc;
    }
    memcpy(in->buf + in->len, data, n);
    in->len += n;
    in->buf[in->len] = 0;
    return true;
}

static void input_consume(mm_keybuf_t *in, size_t n) {
    if (n >= in->len) { in->len = 0; if (in->buf) in->buf[0] = 0; return; }
    memmove(in->buf, in->buf + n, in->len - n);
    in->len -= n;
    in->buf[in->len] = 0;
}

/* Read available bytes from fd into the input buffer. Returns false on a
   hard read error. */
static bool read_available(int fd, mm_keybuf_t *in, int timeout_ms) {
    struct pollfd pfd = { fd, POLLIN, 0 };
    int rc = poll(&pfd, 1, timeout_ms);
    if (rc <= 0) return true;  /* timeout: not an error */
    char tmp[4096];
    ssize_t n = read(fd, tmp, sizeof(tmp));
    if (n < 0) {
        if (errno == EINTR || errno == EAGAIN) return true;
        return false;
    }
    if (n > 0 && !input_push(in, tmp, (size_t)n)) return false;
    return true;
}

static mm_getchar_result_t get_bytes_internal(int fd, mm_keybuf_t *in,
                                               bool cancellable, int esc_delay) {
    int retries = 0;
    /* Wait for the first byte. */
    for (;;) {
        struct pollfd pfd = { fd, POLLIN, 0 };
        /* The timeout is short, so that the items of a live stream are
           picked up (and the preview refreshed) while waiting for a key. */
        int rc = poll(&pfd, 1, cancellable ? 1 : 5);
        if (rc < 0) {
            if (errno == EINTR) continue;
            return MM_GET_CHAR_ERROR;
        }
        if (rc == 0) {
            if (cancellable) return MM_GET_CHAR_CANCELLED;
            continue;
        }
        break;
    }
    char c;
    ssize_t n = read(fd, &c, 1);
    if (n < 0) return (errno == EINTR) ? MM_GET_CHAR_CANCELLED : MM_GET_CHAR_ERROR;
    if (n == 0) return MM_GET_CHAR_ERROR;
    if (!input_push(in, &c, 1)) return MM_GET_CHAR_ERROR;
    if (c == 0x1b) retries = esc_delay / ESC_POLL_INTERVAL;

    char pc = c;
    for (;;) {
        size_t before = in->len;
        if (!read_available(fd, in, ESC_POLL_INTERVAL)) return MM_GET_CHAR_ERROR;
        if (in->len == before) {
            /* No new byte arrived within the poll interval. */
            if (retries > 0) {
                retries--;
                struct timespec ts = { ESC_POLL_INTERVAL / 1000,
                                       (ESC_POLL_INTERVAL % 1000) * 1000000L };
                nanosleep(&ts, NULL);
                continue;
            }
            break;
        }
        char cur = in->buf[in->len - 1];
        if (cur == 0x1b && pc != cur) retries = esc_delay / ESC_POLL_INTERVAL;
        else retries = 0;
        pc = cur;
        if (in->len > MAX_INPUT_BUFFER) return MM_GET_CHAR_ERROR;
    }
    return MM_GET_CHAR_SUCCESS;
}

static bool is_offset_report(const char *s, size_t len) {
    /* "\x1b[<n>;<n>R" */
    if (len < 4 || s[0] != 0x1b || s[1] != '[') return false;
    size_t i = 2;
    size_t digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (!digits || i >= len || s[i] != ';') return false;
    i++;
    digits = 0;
    while (i < len && s[i] >= '0' && s[i] <= '9') { i++; digits++; }
    if (!digits || i >= len || s[i] != 'R') return false;
    return i + 1 == len;
}

static bool is_paste_mode_report(const char *s, size_t len) {
    /* "\x1b[?2004;<n>$y" */
    if (len < 10 || s[0] != 0x1b || s[1] != '[') return false;
    if (strncmp(s + 2, "?2004;", 6) != 0) return false;
    size_t i = 8;
    if (i >= len || s[i] < '0' || s[i] > '4') return false;
    i++;
    if (i + 1 >= len || s[i] != '$' || s[i + 1] != 'y') return false;
    return i + 2 == len;
}

/* Decode an escape sequence starting at in->buf[0]. Advances *sz. */
static mm_event_type_t esc_sequence(mm_keybuf_t *in, size_t *sz) {
    const char *buf = in->buf;
    size_t len = in->len;

    if (len < 2) { *sz = 1; return MM_EV_ESC; }
    if (is_offset_report(buf, len) || is_paste_mode_report(buf, len)) {
        *sz = len;
        return MM_EV_INVALID;
    }
    *sz = 2;
    if (buf[1] == 8) return MM_EV_CTRL_ALT_BACKSPACE;
    if (buf[1] >= 1 && buf[1] <= 'z' - 'a' + 1)
        return (mm_event_type_t)(MM_EV_CTRL_A + (buf[1] - 1));

    bool alt = false;
    const char *b = buf;
    size_t l = len;
    if (l > 2 && b[1] == 0x1b) { b++; l--; alt = true; }

    switch (b[1]) {
    case 0x1b: *sz = 2; return MM_EV_ESC;
    case 127:  *sz = 2; return MM_EV_ALT_BACKSPACE;
    case '[':
    case 'O': {
        if (l < 3) { *sz = 2; return MM_EV_INVALID; }
        /* A modified key: "ESC [ 1 ; <mod> <letter>" where the modifier 3
           stands for Alt. */
        if (b[2] >= '0' && b[2] <= '9' && l > 3 && b[3] == ';') {
            size_t k = 4;   /* skip the first parameter and the ';' */
            int mod = 0;
            while (k < l && b[k] >= '0' && b[k] <= '9') {
                mod = mod * 10 + (b[k] - '0');
                k++;
            }
            if (k < l && mod == 3 && b[k] >= 'A' && b[k] <= 'D') {
                *sz = k + 1;
                switch (b[k]) {
                case 'A': return MM_EV_ALT_UP;
                case 'B': return MM_EV_ALT_DOWN;
                case 'C': return MM_EV_ALT_RIGHT;
                case 'D': return MM_EV_ALT_LEFT;
                default: break;
                }
            }
        }
        *sz = 3;
        switch (b[2]) {
        case 'D': return alt ? MM_EV_ALT_LEFT : MM_EV_LEFT;
        case 'C': return alt ? MM_EV_ALT_RIGHT : MM_EV_RIGHT;
        case 'B': return alt ? MM_EV_ALT_DOWN : MM_EV_DOWN;
        case 'A': return alt ? MM_EV_ALT_UP : MM_EV_UP;
        case 'Z': return MM_EV_SHIFT_TAB;
        case 'H': return MM_EV_HOME;
        case 'F': return MM_EV_END;
        case 'P': return MM_EV_F1;
        case 'Q': return MM_EV_F2;
        case 'R': return MM_EV_F3;
        case 'S': return MM_EV_F4;
        case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': {
            if (l < 4) { *sz = 3; return MM_EV_INVALID; }
            *sz = 4;
            switch (b[2]) {
            case '1': return MM_EV_HOME;
            case '2': return MM_EV_INSERT;
            case '3': return MM_EV_DELETE;
            case '4': return MM_EV_END;
            case '5': return MM_EV_PAGE_UP;
            case '6': return MM_EV_PAGE_DOWN;
            case '7': return MM_EV_HOME;
            case '8': return MM_EV_END;
            default: *sz = 4; return MM_EV_INVALID;
            }
        }
        default:
            *sz = 3;
            return MM_EV_INVALID;
        }
    }
    default:
        *sz = 2;
        return MM_EV_INVALID;
    }
}

/* Read the next event from fd. Returns false on error. */
bool mm_read_event(int fd, mm_keybuf_t *in, mm_event_t *ev, bool cancellable) {
    const char *esc = getenv("ESCDELAY");
    int esc_delay = esc ? atoi(esc) : ESC_DELAY_DEFAULT;
    if (esc_delay < 0) esc_delay = 0;

    if (in->len == 0) {
        mm_getchar_result_t res = get_bytes_internal(fd, in, cancellable, esc_delay);
        if (res == MM_GET_CHAR_CANCELLED) { ev->type = MM_EV_INVALID; return true; }
        if (res != MM_GET_CHAR_SUCCESS) { ev->type = MM_EV_FATAL; return false; }
    }
    if (in->len == 0) { ev->type = MM_EV_FATAL; return false; }

    size_t sz = 1;
    unsigned char c0 = (unsigned char)in->buf[0];
    mm_event_type_t type;
    wchar_t rune = 0;

    switch (c0) {
    case 3:  type = MM_EV_CTRL_C; break;
    case 7:  type = MM_EV_CTRL_G; break;
    case 17: type = MM_EV_CTRL_Q; break;
    case 127: type = MM_EV_BACKSPACE; break;
    case 8:  type = MM_EV_CTRL_BACKSPACE; break;
    case 0:  type = MM_EV_CTRL_SPACE; break;
    case 28: type = MM_EV_CTRL_BACKSLASH; break;
    case 29: type = MM_EV_CTRL_RIGHT_BRACKET; break;
    case 30: type = MM_EV_CTRL_CARET; break;
    case 31: type = MM_EV_CTRL_SLASH; break;
    case 0x1b:
        type = esc_sequence(in, &sz);
        break;
    default:
        if (c0 <= 26) {   /* CTRL-A ~ CTRL-Z */
            type = (mm_event_type_t)c0;
            break;
        }
        {
            wchar_t wc;
            size_t used = mm_utf8_decode(in->buf, in->len, &wc);
            if (used == 0) used = 1;
            if (wc == (wchar_t)0xFFFD && used == 1 && (unsigned char)in->buf[0] >= 0x80) {
                type = MM_EV_ESC;
                sz = 1;
                break;
            }
            type = MM_EV_RUNE;
            rune = wc;
            sz = used;
        }
        break;
    }

    input_consume(in, sz);
    ev->type = type;
    ev->rune = rune;
    ev->x = 0;
    ev->y = 0;
    return true;
}

void mm_keybuf_init(mm_keybuf_t *in) { memset(in, 0, sizeof(*in)); }
void mm_keybuf_free(mm_keybuf_t *in) { free(in->buf); in->buf = NULL; in->len = in->cap = 0; }
