/* ansi.c - ANSI escape-sequence parsing and color extraction.
   Faithful port of fzf src/ansi.go (v0.74.3). */
#include "ansi.h"
#include "util.h"

#include <stdlib.h>
#include <string.h>

#define RUNE_SELF 0x80

static bool is_numeric(uint8_t c) { return c >= '0' && c <= '9'; }

static int string_rune_count(const char *s, size_t len) {
    int n = 0;
    size_t i = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(s + i, len - i, &cp);
        if (used == 0) used = 1;
        i += used;
        n++;
    }
    return n;
}

/* Decode last rune in s[0..len); returns byte length of that rune. */
static int decode_last_rune(const char *s, size_t len) {
    /* Walk forward keeping the last rune's length. */
    int last = 1;
    size_t i = 0;
    while (i < len) {
        wchar_t cp;
        size_t used = mm_utf8_decode(s + i, len - i, &cp);
        if (used == 0) used = 1;
        last = (int)used;
        i += used;
    }
    return last;
}

static int decode_rune_len(const char *s, size_t len) {
    wchar_t cp;
    size_t used = mm_utf8_decode(s, len, &cp);
    return used ? (int)used : 1;
}

static bool is_ctrl_seq_start(uint8_t c) {
    return c == '\\' || c == '[' || c == '(' || c == ')';
}

/* matchControlSequence: s[0] is '\x1b'. Return bytes consumed or -1. */
static int match_control_sequence(const char *s, size_t len) {
    size_t i = 2;
    for (; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        switch (c) {
        case '0': case '1': case '2': case '3': case '4':
        case '5': case '6': case '7': case '8': case '9':
        case ';': case ':': case '?':
            break;
        default:
            if (('a' <= c && c <= 'z') || ('A' <= c && c <= 'Z') || c == '@')
                return (int)(i + 1);
            return -1;
        }
    }
    return -1;
}

/* matchOperatingSystemCommand: s[0] is '\x1b', start = index after prefix. */
static int match_operating_system_command(const char *s, size_t len, int start) {
    int i = start;
    int idx = -1;
    for (int k = i; k < (int)len; k++) {
        if (s[k] == '\x07' || s[k] == '\x1b') { idx = k - i; break; }
    }
    if (idx < 0) return -1;
    i += idx;
    if (s[i] == '\x07') return i + 1;
    if (i < (int)len - 1 && s[i + 1] == '\\') return i + 2;
    /* "\x1b]8;;\x1b" reset form */
    if (s[0] == '\x1b' && i + 1 == 5 && memcmp(s, "\x1b]8;;\x1b", 5) == 0)
        return i + 1;
    return -1;
}

void mm_next_ansi_escape_sequence(const char *s, size_t len,
                                  int *out_start, int *out_end) {
    size_t i = 0;
    for (; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        if (c == '\x0e' || c == '\x0f' || c == '\x1b' || c == '\x08' || c == '\n')
            goto loop;
    }
    *out_start = -1; *out_end = -1;
    return;

loop:
    for (; i < len; i++) {
        uint8_t c = (uint8_t)s[i];
        switch (c) {
        case '\n':
            *out_start = (int)i; *out_end = (int)i + 1; return;
        case '\x08':
            if (i > 0 && s[i - 1] != '\n') {
                if ((uint8_t)s[i - 1] < RUNE_SELF) {
                    *out_start = (int)i - 1; *out_end = (int)i + 1; return;
                }
                int n = decode_last_rune(s, i);
                *out_start = (int)i - n; *out_end = (int)i + 1; return;
            }
            break;
        case '\x1b':
            if (i + 2 < len && is_ctrl_seq_start((uint8_t)s[i + 1])) {
                int j = match_control_sequence(s + i, len - i);
                if (j != -1) { *out_start = (int)i; *out_end = (int)(i + j); return; }
            }
            if (i + 5 < len && s[i + 1] == ']') {
                int j = 2;
                for (; i + j < len && is_numeric((uint8_t)s[i + j]); j++) {}
                if (j > 2 && i + j + 1 < len &&
                    (s[i + j] == ';' || s[i + j] == ':') &&
                    (uint8_t)s[i + j + 1] >= 0x20) {
                    int k = match_operating_system_command(s + i, len - i, j + 2);
                    if (k != -1) { *out_start = (int)i; *out_end = (int)(i + k); return; }
                }
            }
            if (i + 1 < len && s[i + 1] != '\n') {
                if ((uint8_t)s[i + 1] < RUNE_SELF) {
                    *out_start = (int)i; *out_end = (int)i + 2; return;
                }
                int n = decode_rune_len(s + i + 1, len - i - 1);
                *out_start = (int)i; *out_end = (int)(i + n + 1); return;
            }
            break;
        case '\x0e':
        case '\x0f':
            *out_start = (int)i; *out_end = (int)i + 1; return;
        }
    }
    *out_start = -1; *out_end = -1;
}

/* parseAnsiCode: split leading integer on first ';'/':'. */
static int parse_ansi_code(const char *s, size_t len,
                           int *out_num, char *out_sep,
                           const char **out_remaining, size_t *out_rem_len) {
    int i = -1;
    for (size_t j = 0; j < len; j++) {
        if (s[j] == ';' || s[j] == ':') { i = (int)j; break; }
    }
    size_t head_len = len;
    *out_sep = 0;
    *out_remaining = s + len;
    *out_rem_len = 0;
    if (i >= 0) {
        *out_sep = s[i];
        *out_remaining = s + i + 1;
        *out_rem_len = len - (size_t)i - 1;
        head_len = (size_t)i;
    }
    *out_num = -1;
    if (head_len > 0) {
        int code = 0;
        for (size_t k = 0; k < head_len; k++) {
            int ch = (uint8_t)s[k] - '0';
            if (ch > 9) return 0;
            code = code * 10 + ch;
        }
        *out_num = code;
    }
    return 0;
}

static mm_ansi_state_t state_default(void) {
    mm_ansi_state_t s = { MM_COL_DEFAULT, MM_COL_DEFAULT, MM_COL_DEFAULT, 0, MM_COL_DEFAULT, NULL };
    return s;
}

static mm_ansi_state_t interpret_code_impl(const char *ansiCode, size_t clen,
                                           const mm_ansi_state_t *prevState) {
    if (clen == 1 && ansiCode[0] == '\n') {
        if (prevState) return *prevState;
        return state_default();
    }

    mm_ansi_state_t state;
    if (!prevState) {
        state = state_default();
    } else {
        state = *prevState;
    }

    if (clen < 2 || ansiCode[0] != '\x1b' || ansiCode[1] != '[' ||
        ansiCode[clen - 1] != 'm') {
        if (prevState) {
            if ((clen >= 2 && ansiCode[clen - 2] == '0' && ansiCode[clen - 1] == 'K') ||
                (clen >= 1 && ansiCode[clen - 1] == 'K')) {
                state.lbg = prevState->bg;
            } else if (clen >= 4 && memcmp(ansiCode, "\x1b]8;", 4) == 0 &&
                       ((clen >= 2 && ansiCode[clen - 2] == '\x1b' && ansiCode[clen - 1] == '\\') ||
                        ansiCode[clen - 1] == '\a')) {
                int stLen = 2;
                if (ansiCode[clen - 1] == '\a') stLen = 1;
                if ((int)clen == 5 + stLen && ansiCode[4] == ';') {
                    state.url = NULL;
                } else {
                    int paramsEnd = -1;
                    for (size_t k = 4; k < clen - (size_t)stLen; k++) {
                        if (ansiCode[k] == ';') { paramsEnd = (int)k - 4; break; }
                    }
                    if (paramsEnd >= 0) {
                        /* uri/params would need allocation; we store pointers into
                           the code buffer only when it is stable. For parity of
                           rendering we omit URL state (rare); leave NULL. */
                        state.url = NULL;
                    }
                }
            }
        }
        return state;
    }

    #define RESET_STATE() do { \
        state.fg = MM_COL_DEFAULT; state.bg = MM_COL_DEFAULT; \
        state.ul = MM_COL_DEFAULT; state.attr = 0; } while (0)

    if (clen <= 3) { RESET_STATE(); return state; }

    const char *p = ansiCode + 2;
    size_t plen = clen - 3; /* drop "\x1b[" and trailing 'm' */

    int state256 = 0;
    mm_color_t *ptr = &state.fg;
    int count = 0;

    while (plen != 0) {
        int num; char sep; const char *remaining; size_t rem_len;
        parse_ansi_code(p, plen, &num, &sep, &remaining, &rem_len);
        if (rem_len == plen && num == -1) break; /* no progress guard */
        p = remaining; plen = rem_len;
        if (num == -1) continue;
        count++;
        switch (state256) {
        case 0:
            switch (num) {
            case 38: ptr = &state.fg; state256++; break;
            case 48: ptr = &state.bg; state256++; break;
            case 58: ptr = &state.ul; state256++; break;
            case 39: state.fg = MM_COL_DEFAULT; break;
            case 49: state.bg = MM_COL_DEFAULT; break;
            case 59: state.ul = MM_COL_DEFAULT; break;
            case 1: state.attr |= MM_BOLD; break;
            case 2: state.attr |= MM_DIM; break;
            case 3: state.attr |= MM_ITALIC; break;
            case 4:
                if (sep == ':') {
                    int subNum; char s2; const char *r2; size_t r2len;
                    parse_ansi_code(p, plen, &subNum, &s2, &r2, &r2len);
                    p = r2; plen = r2len;
                    state.attr &= ~MM_ULSTYLE_MASK;
                    switch (subNum) {
                    case 0: state.attr &= ~MM_UNDERLINE; break;
                    case 1: state.attr |= MM_UNDERLINE; break;
                    case 2: state.attr |= MM_UNDERLINE | MM_ULSTYLE_DOUBLE; break;
                    case 3: state.attr |= MM_UNDERLINE | MM_ULSTYLE_CURLY; break;
                    case 4: state.attr |= MM_UNDERLINE | MM_ULSTYLE_DOTTED; break;
                    case 5: state.attr |= MM_UNDERLINE | MM_ULSTYLE_DASHED; break;
                    default: state.attr |= MM_UNDERLINE; break;
                    }
                } else {
                    state.attr |= MM_UNDERLINE;
                }
                break;
            case 5: state.attr |= MM_BLINK; break;
            case 7: state.attr |= MM_REVERSE; break;
            case 9: state.attr |= MM_STRIKETHROUGH; break;
            case 22: state.attr &= ~MM_BOLD; state.attr &= ~MM_DIM; break;
            case 23: state.attr &= ~MM_ITALIC; break;
            case 24: state.attr &= ~MM_UNDERLINE; state.attr &= ~MM_ULSTYLE_MASK; break;
            case 25: state.attr &= ~MM_BLINK; break;
            case 27: state.attr &= ~MM_REVERSE; break;
            case 29: state.attr &= ~MM_STRIKETHROUGH; break;
            case 0: RESET_STATE(); state256 = 0; break;
            default:
                if (num >= 30 && num <= 37) state.fg = num - 30;
                else if (num >= 40 && num <= 47) state.bg = num - 40;
                else if (num >= 90 && num <= 97) state.fg = num - 90 + 8;
                else if (num >= 100 && num <= 107) state.bg = num - 100 + 8;
                break;
            }
            break;
        case 1:
            switch (num) {
            case 2: state256 = 10; break;
            case 5: state256++; break;
            default: state256 = 0; break;
            }
            break;
        case 2: *ptr = num; state256 = 0; break;
        case 10: *ptr = (1 << 24) | (num << 16); state256++; break;
        case 11: *ptr = *ptr | (num << 8); state256++; break;
        case 12: *ptr = *ptr | num; state256 = 0; break;
        default: break;
        }
    }

    if (count == 0) RESET_STATE();
    if (state256 > 0) *ptr = MM_COL_DEFAULT;
    #undef RESET_STATE
    return state;
}

mm_ansi_state_t mm_interpret_code(const char *code, size_t code_len,
                                  const mm_ansi_state_t *prev_state) {
    return interpret_code_impl(code, code_len, prev_state);
}

bool mm_ansi_state_colored(const mm_ansi_state_t *s) {
    return s->fg != MM_COL_DEFAULT || s->bg != MM_COL_DEFAULT ||
           s->ul != MM_COL_DEFAULT || s->attr > 0 ||
           s->lbg >= 0 || s->url != NULL;
}

bool mm_ansi_state_equals(const mm_ansi_state_t *s, const mm_ansi_state_t *t) {
    if (!t) return !mm_ansi_state_colored(s);
    return s->fg == t->fg && s->bg == t->bg && s->ul == t->ul &&
           s->attr == t->attr && s->lbg == t->lbg && s->url == t->url;
}

static void offsets_push(mm_ansi_offsets_t *o, mm_ansi_offset_t v) {
    if (o->len == o->cap) {
        o->cap = o->cap ? o->cap * 2 : 32;
        o->ptr = realloc(o->ptr, (size_t)o->cap * sizeof(mm_ansi_offset_t));
    }
    o->ptr[o->len++] = v;
}

void mm_ansi_offsets_free(mm_ansi_offsets_t *o) {
    free(o->ptr);
    o->ptr = NULL; o->len = o->cap = 0;
}

void mm_extract_color(const char *str, size_t len, mm_ansi_state_t *state,
                      bool (*proc)(const char *seg, size_t seg_len,
                                   mm_ansi_state_t *st, void *ud),
                      void *ud,
                      char **out_trimmed, size_t *out_trimmed_len,
                      mm_ansi_offsets_t *out_offsets,
                      bool *out_have_state, mm_ansi_state_t *out_state) {
    mm_ansi_offsets_t offsets = { NULL, 0, 0 };
    if (state) {
        mm_ansi_offset_t off = { { 0, 0 }, *state };
        offsets_push(&offsets, off);
    }

    mm_ansi_state_t pstate;
    bool have_pstate = false;
    size_t outcap = len + 16;
    char *output = malloc(outcap);
    size_t outlen = 0;
    size_t prevIdx = 0;
    int runeCount = 0;

    size_t idx = 0;
    while (idx < len) {
        int start, end;
        mm_next_ansi_escape_sequence(str + idx, len - idx, &start, &end);
        if (start == -1) break;
        start += (int)idx;
        idx += (size_t)end;

        const char *prev = str + prevIdx;
        size_t prev_len = (size_t)start - prevIdx;
        if (proc && !proc(prev, prev_len, state, ud)) {
            free(output);
            mm_ansi_offsets_free(&offsets);
            *out_trimmed = NULL; *out_trimmed_len = 0;
            out_offsets->ptr = NULL; out_offsets->len = 0; out_offsets->cap = 0;
            if (out_have_state) *out_have_state = false;
            return;
        }
        prevIdx = idx;

        if (prev_len != 0) {
            runeCount += string_rune_count(prev, prev_len);
            if (outlen + prev_len > outcap) {
                while (outlen + prev_len > outcap) outcap *= 2;
                output = realloc(output, outcap);
            }
            memcpy(output + outlen, prev, prev_len);
            outlen += prev_len;
        }

        const char *code = str + start;
        size_t code_len = idx - (size_t)start;
        mm_ansi_state_t newState = interpret_code_impl(code, code_len, state);
        bool is_nl = (code_len == 1 && code[0] == '\n');
        bool changed = !mm_ansi_state_equals(&newState, state);
        if (is_nl || changed) {
            if (state && offsets.len > 0)
                offsets.ptr[offsets.len - 1].offset[1] = runeCount;

            if (is_nl) {
                if (outlen + 1 > outcap) { outcap *= 2; output = realloc(output, outcap); }
                output[outlen++] = '\n';
                runeCount++;
                if (newState.lbg >= 0) {
                    mm_ansi_state_t marker = newState;
                    marker.attr |= MM_FULLBG;
                    mm_ansi_offset_t off = { { runeCount, runeCount }, marker };
                    offsets_push(&offsets, off);
                    newState.lbg = MM_COL_DEFAULT;
                }
            }

            if (mm_ansi_state_colored(&newState)) {
                pstate = newState;
                have_pstate = true;
                state = &pstate;
                mm_ansi_offset_t off = { { runeCount, runeCount }, newState };
                offsets_push(&offsets, off);
            } else {
                state = NULL;
            }
        }
    }
    (void)have_pstate;

    const char *rest;
    size_t rest_len;
    if (prevIdx == 0) {
        free(output);
        output = NULL;
        *out_trimmed = malloc(len + 1);
        memcpy(*out_trimmed, str, len);
        (*out_trimmed)[len] = 0;
        *out_trimmed_len = len;
        rest = str; rest_len = len;
    } else {
        rest = str + prevIdx;
        rest_len = len - prevIdx;
        if (outlen + rest_len > outcap) {
            while (outlen + rest_len > outcap) outcap *= 2;
            output = realloc(output, outcap);
        }
        memcpy(output + outlen, rest, rest_len);
        outlen += rest_len;
        output[outlen] = 0;
        *out_trimmed = output;
        *out_trimmed_len = outlen;
    }
    if (proc) proc(rest, rest_len, state, ud);

    if (offsets.len > 0) {
        if (rest_len > 0 && state) {
            runeCount += string_rune_count(rest, rest_len);
            offsets.ptr[offsets.len - 1].offset[1] = runeCount;
        }
        *out_offsets = offsets;
    } else {
        out_offsets->ptr = NULL; out_offsets->len = 0; out_offsets->cap = 0;
    }
    if (out_have_state) *out_have_state = (state != NULL);
    if (out_state && state) *out_state = *state;
}
