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

/* ESC followed by a normal key is complete. CSI and SS3 keep their
   parameters in 0x20-0x3f and end on a final byte in 0x40-0x7e, except
   '$', which is an intermediate in the paste-mode report. A finished
   sequence must not sit through another escape poll: alt-j is ESC then
   'j', and that extra poll is a full ESC_POLL_INTERVAL on every such key. */
static bool sequence_complete(const mm_keybuf_t *in) {
    if (!in->buf || in->len == 0) return false;
    unsigned char c = (unsigned char)in->buf[0];
    if (c != 0x1b) return false;
    if (in->len < 2) return false;
    unsigned char n = (unsigned char)in->buf[1];
    if (n != '[' && n != 'O' && n != 0x1b) return true;
    /* ESC [ and ESC O are the start of a longer sequence. */
    if (in->len < 3) return false;
    unsigned char last = (unsigned char)in->buf[in->len - 1];
    if (last == '$' || last == '<' || last == ';') return false;
    if (last >= '0' && last <= '9') return false;
    return last >= 0x40 && last <= 0x7e;
}

/* A finished key does not need the escape poll. ESC and a partial UTF-8
   sequence still wait, because the rest of the sequence may be on its way. */
static bool input_ready(const mm_keybuf_t *in) {
    if (!in->buf || in->len == 0) return false;
    unsigned char c = (unsigned char)in->buf[0];
    if (c == 0x1b) return false;
    if (c < 0x80) return true;
    int need = 1;
    if ((c & 0xE0) == 0xC0) need = 2;
    else if ((c & 0xF0) == 0xE0) need = 3;
    else if ((c & 0xF8) == 0xF0) need = 4;
    else return true;
    return (int)in->len >= need;
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
    /* Plain characters and finished UTF-8 are complete. Waiting here cost
       a poll interval on every keystroke before the search could start. */
    if (retries == 0 && input_ready(in)) {
        if (!read_available(fd, in, 0)) return MM_GET_CHAR_ERROR;
        return MM_GET_CHAR_SUCCESS;
    }
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
        if (sequence_complete(in)) {
            /* Slurp a key that is already queued, then stop. Polling again
               would wait ESC_POLL_INTERVAL for a byte that is not coming. */
            if (!read_available(fd, in, 0)) return MM_GET_CHAR_ERROR;
            return MM_GET_CHAR_SUCCESS;
        }
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

/* SGR mouse: ESC [ < btn ; x ; y M/m. Coordinates in the sequence are
   1-based. Returns MM_EV_MOUSE and fills ev, or MM_EV_INVALID. */
static mm_event_type_t mouse_sequence(const char *buf, const char *b, size_t l,
                                      size_t *sz, mm_event_t *ev) {
    if (l < 4) { *sz = l > 0 ? l : 1; return MM_EV_INVALID; }
    const char *s = b + 3;
    size_t avail = l - 3;
    size_t end = 0;
    while (end < avail && s[end] != 'M' && s[end] != 'm') end++;
    if (end >= avail || end == 0) { *sz = 3; return MM_EV_INVALID; }
    int vals[3] = {0, 0, 0};
    int vi = 0;
    int cur = 0;
    bool any = false;
    for (size_t i = 0; i < end; i++) {
        char c = s[i];
        if (c == ';') {
            if (!any || vi >= 2) { *sz = 3; return MM_EV_INVALID; }
            vals[vi++] = cur;
            cur = 0;
            any = false;
        } else if (c >= '0' && c <= '9') {
            if (cur > (1000000 - (c - '0')) / 10) { *sz = 3; return MM_EV_INVALID; }
            cur = cur * 10 + (c - '0');
            any = true;
        } else {
            *sz = 3;
            return MM_EV_INVALID;
        }
    }
    if (!any || vi != 2) { *sz = 3; return MM_EV_INVALID; }
    vals[2] = cur;
    *sz = (size_t)(b - buf) + 3 + end + 1;
    int btn = vals[0];
    ev->x = vals[1] - 1;
    ev->y = vals[2] - 1;
    ev->mouse_down = s[end] == 'M';
    ev->mouse_scroll = 0;
    if (btn >= 64) {
        int wheel = (btn - 64) & 3;
        ev->mouse_scroll = wheel == 0 ? 1 : wheel == 1 ? -1 : 0;
        ev->mouse_down = false;
    }
    return MM_EV_MOUSE;
}

static bool byte_in(unsigned char c, const char *set) {
    return strchr(set, (char)c) != NULL;
}

/* Modifier bits on a CSI arrow, in the same order fzf checks them. */
static mm_event_type_t nav_modified(bool ctrl, bool alt, bool shift, unsigned char ch) {
    switch (ch) {
    case 'A':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_UP;
        if (ctrl && alt) return MM_EV_CTRL_ALT_UP;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_UP;
        if (alt && shift) return MM_EV_ALT_SHIFT_UP;
        if (ctrl) return MM_EV_CTRL_UP;
        if (alt) return MM_EV_ALT_UP;
        if (shift) return MM_EV_SHIFT_UP;
        return MM_EV_UP;
    case 'B':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_DOWN;
        if (ctrl && alt) return MM_EV_CTRL_ALT_DOWN;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_DOWN;
        if (alt && shift) return MM_EV_ALT_SHIFT_DOWN;
        if (ctrl) return MM_EV_CTRL_DOWN;
        if (alt) return MM_EV_ALT_DOWN;
        if (shift) return MM_EV_SHIFT_DOWN;
        return MM_EV_DOWN;
    case 'C':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_RIGHT;
        if (ctrl && alt) return MM_EV_CTRL_ALT_RIGHT;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_RIGHT;
        if (alt && shift) return MM_EV_ALT_SHIFT_RIGHT;
        if (ctrl) return MM_EV_CTRL_RIGHT;
        if (shift) return MM_EV_SHIFT_RIGHT;
        if (alt) return MM_EV_ALT_RIGHT;
        return MM_EV_RIGHT;
    case 'D':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_LEFT;
        if (ctrl && alt) return MM_EV_CTRL_ALT_LEFT;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_LEFT;
        if (alt && shift) return MM_EV_ALT_SHIFT_LEFT;
        if (ctrl) return MM_EV_CTRL_LEFT;
        if (alt) return MM_EV_ALT_LEFT;
        if (shift) return MM_EV_SHIFT_LEFT;
        return MM_EV_LEFT;
    case 'H':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_HOME;
        if (ctrl && alt) return MM_EV_CTRL_ALT_HOME;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_HOME;
        if (alt && shift) return MM_EV_ALT_SHIFT_HOME;
        if (ctrl) return MM_EV_CTRL_HOME;
        if (alt) return MM_EV_ALT_HOME;
        if (shift) return MM_EV_SHIFT_HOME;
        return MM_EV_HOME;
    case 'F':
        if (ctrl && alt && shift) return MM_EV_CTRL_ALT_SHIFT_END;
        if (ctrl && alt) return MM_EV_CTRL_ALT_END;
        if (ctrl && shift) return MM_EV_CTRL_SHIFT_END;
        if (alt && shift) return MM_EV_ALT_SHIFT_END;
        if (ctrl) return MM_EV_CTRL_END;
        if (alt) return MM_EV_ALT_END;
        if (shift) return MM_EV_SHIFT_END;
        return MM_EV_END;
    default:
        return MM_EV_INVALID;
    }
}

/* "ESC [ 1 ; <mod> <key>". Kitty and xterm send plain arrows as
   "ESC [ 1 ; 1 A", and a short read of that sequence leaves "1A" to be
   typed into the query. The whole sequence is one navigation key. */
static mm_event_type_t csi_modified_nav(const char *b, size_t l, size_t *sz) {
    if (l < 6) {
        *sz = l;
        return MM_EV_INVALID;
    }
    *sz = 6;
    unsigned char m = (unsigned char)b[4];
    if (m < '1' || m > '9') return MM_EV_INVALID;
    bool ctrl = byte_in(m, "5678");
    bool alt = byte_in(m, "3478");
    bool shift = byte_in(m, "2468");
    unsigned char ch = (unsigned char)b[5];
    if (m == '9') {
        ctrl = false;
        alt = true;
        shift = false;
    } else if (m == '1' && byte_in(ch, "0123456")) {
        /* Modifier 10 through 16. The key byte follows the second digit. */
        if (l < 7) {
            *sz = l;
            return MM_EV_INVALID;
        }
        ctrl = byte_in(ch, "3456");
        alt = true;
        shift = byte_in(ch, "0246");
        *sz = 7;
        ch = (unsigned char)b[6];
    }
    return nav_modified(ctrl, alt, shift, ch);
}

/* Decode an escape sequence starting at in->buf[0]. Advances *sz. */
static mm_event_type_t esc_sequence(mm_keybuf_t *in, size_t *sz, mm_event_t *ev) {
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
        *sz = 3;
        switch (b[2]) {
        case 'D': return alt ? MM_EV_ALT_LEFT : MM_EV_LEFT;
        case 'C': return alt ? MM_EV_ALT_RIGHT : MM_EV_RIGHT;
        case 'B': return alt ? MM_EV_ALT_DOWN : MM_EV_DOWN;
        case 'A': return alt ? MM_EV_ALT_UP : MM_EV_UP;
        case 'Z': return MM_EV_SHIFT_TAB;
        case '<': return mouse_sequence(buf, b, l, sz, ev);
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
            case '1':
                if (b[3] == '~') return MM_EV_HOME;
                if (b[3] == '1' || b[3] == '2' || b[3] == '3' || b[3] == '4' ||
                    b[3] == '5' || b[3] == '7' || b[3] == '8' || b[3] == '9') {
                    /* ESC [ 11 ~ through ESC [ 19 ~ are F1 through F8. */
                    if (l >= 5 && b[4] == '~') {
                        *sz = 5;
                        switch (b[3]) {
                        case '1': return MM_EV_F1;
                        case '2': return MM_EV_F2;
                        case '3': return MM_EV_F3;
                        case '4': return MM_EV_F4;
                        case '5': return MM_EV_F5;
                        case '7': return MM_EV_F6;
                        case '8': return MM_EV_F7;
                        case '9': return MM_EV_F8;
                        default: break;
                        }
                    }
                    return MM_EV_INVALID;
                }
                if (b[3] == ';') return csi_modified_nav(b, l, sz);
                return MM_EV_INVALID;
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
        /* ESC followed by a character is Alt-that character (alt-j, alt-k). */
        if ((unsigned char)b[1] >= 32) {
            *sz = 2;
            ev->rune = (unsigned char)b[1];
            return MM_EV_ALT;
        }
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
    memset(ev, 0, sizeof(*ev));

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
        type = esc_sequence(in, &sz, ev);
        if (type == MM_EV_ALT) rune = ev->rune;
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
    if (type != MM_EV_MOUSE) {
        ev->x = 0;
        ev->y = 0;
        ev->mouse_down = false;
        ev->mouse_scroll = 0;
    }
    return true;
}

void mm_keybuf_init(mm_keybuf_t *in) { memset(in, 0, sizeof(*in)); }
void mm_keybuf_free(mm_keybuf_t *in) { free(in->buf); in->buf = NULL; in->len = in->cap = 0; }
