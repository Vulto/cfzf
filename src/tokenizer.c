/* tokenizer.c - see tokenizer.h. Ported from fzf src/tokenizer.go (v0.74.3). */
#define _POSIX_C_SOURCE 200809L
#include "tokenizer.h"

#include <regex.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <limits.h>
#include <stdint.h>

static bool parse_range_int(const char *s, size_t len, int *out) {
    if (!s || !out || len == 0) return false;
    size_t digit = (s[0] == '-' || s[0] == '+') ? 1u : 0u;
    if (digit == len) return false;
    for (size_t i = digit; i < len; i++)
        if (s[i] < '0' || s[i] > '9') return false;
    char stack[64];
    char *buf = stack;
    if (len >= sizeof(stack)) {
        buf = malloc(len + 1);
        if (!buf) return false;
    }
    memcpy(buf, s, len);
    buf[len] = 0;
    char *end = NULL;
    errno = 0;
    long value = strtol(buf, &end, 10);
    bool ok = errno != ERANGE && end != buf && *end == 0 &&
              value >= INT_MIN && value <= INT_MAX;
    if (buf != stack) free(buf);
    if (!ok) return false;
    *out = (int)value;
    return true;
}

bool mm_parse_range(const char *s, mm_range_t *out) {
    if (!s || !out || !*s) return false;
    if (strcmp(s, "..") == 0) {
        out->begin = MM_RANGE_ELLIPSIS;
        out->end = MM_RANGE_ELLIPSIS;
        return true;
    }

    const char *dots = strstr(s, "..");
    if (!dots) {
        int value;
        if (!parse_range_int(s, strlen(s), &value) || value == 0) return false;
        out->begin = value;
        out->end = value;
        return true;
    }

    /* More than one range separator is not a valid nth expression. */
    if (strstr(dots + 2, "..")) return false;
    size_t left_len = (size_t)(dots - s);
    const char *right = dots + 2;
    if (left_len == 0) {
        int end;
        if (!*right) {
            out->begin = MM_RANGE_ELLIPSIS;
            out->end = MM_RANGE_ELLIPSIS;
            return true;
        }
        if (!parse_range_int(right, strlen(right), &end) || end == 0) return false;
        out->begin = MM_RANGE_ELLIPSIS;
        out->end = end;
        return true;
    }
    if (!*right) {
        int begin;
        if (!parse_range_int(s, left_len, &begin) || begin == 0) return false;
        out->begin = begin;
        out->end = MM_RANGE_ELLIPSIS;
        return true;
    }

    int begin, end;
    if (!parse_range_int(s, left_len, &begin) ||
        !parse_range_int(right, strlen(right), &end) ||
        begin == 0 || end == 0 || (begin < 0 && end > 0))
        return false;
    out->begin = begin;
    out->end = end;
    return true;
}

bool mm_parse_ranges(const char *str, mm_range_t **out, int *outn) {
    if (!out || !outn) return false;
    *out = NULL;
    *outn = 0;
    if (!str || !*str) return false;

    int cap = 4, n = 0;
    mm_range_t *ranges = malloc((size_t)cap * sizeof(*ranges));
    if (!ranges) return false;

    const char *start = str;
    for (const char *p = str;; p++) {
        if (*p != ',' && *p != 0) continue;
        mm_range_t range;
        size_t token_len = (size_t)(p - start);
        if (token_len == 0) {
            free(ranges);
            return false;
        }
        char *token = strndup(start, token_len);
        if (!token) {
            free(ranges);
            return false;
        }
        bool valid = mm_parse_range(token, &range);
        free(token);
        if (!valid) {
            free(ranges);
            return false;
        }
        if (n == cap) {
            if (cap > INT_MAX / 2) {
                free(ranges);
                return false;
            }
            int new_cap = cap * 2;
            if ((size_t)new_cap > SIZE_MAX / sizeof(*ranges)) {
                free(ranges);
                return false;
            }
            mm_range_t *grown = realloc(ranges, (size_t)new_cap * sizeof(*ranges));
            if (!grown) {
                free(ranges);
                return false;
            }
            ranges = grown;
            cap = new_cap;
        }
        ranges[n++] = range;
        if (*p == 0) break;
        start = p + 1;
    }
    *out = ranges;
    *outn = n;
    return n > 0;
}

static bool is_awk_white(char c) { return c == '\t' || c == ' ' || c == '\n'; }

/* awkTokenizer: tokens are \S+\s* ; returns prefixLength = leading whitespace
   rune count. */
static char **awk_tokenize(const char *input, int *prefix_length, int *outn) {
    int cap = 8, n = 0;
    char **ret = malloc((size_t)cap * sizeof(char *));
    if (!ret) { *outn = 0; return NULL; }
    int prefix = 0;
    int state = 0; /* 0=nil 1=black 2=white */
    long begin = 0, end = 0;
    size_t len = strlen(input);
    for (size_t idx = 0; idx < len; idx++) {
        char r = input[idx];
        bool white = is_awk_white(r);
        switch (state) {
        case 0:
            if (white) prefix++;
            else { state = 1; begin = (long)idx; end = (long)idx + 1; }
            break;
        case 1:
            end = (long)idx + 1;
            if (white) state = 2;
            break;
        case 2:
            if (white) end = (long)idx + 1;
            else {
                if (n == cap) { cap *= 2; ret = realloc(ret, (size_t)cap * sizeof(char *)); }
                ret[n++] = strndup(input + begin, (size_t)(end - begin));
                state = 1; begin = (long)idx; end = (long)idx + 1;
            }
            break;
        }
    }
    if (begin < end) {
        if (n == cap) { cap *= 2; ret = realloc(ret, (size_t)cap * sizeof(char *)); }
        ret[n++] = strndup(input + begin, (size_t)(end - begin));
    }
    *prefix_length = prefix;
    *outn = n;
    return ret;
}

/* True when the string contains a character that is special in a regular
   expression (fzf uses regexp.QuoteMeta for this test). */
static bool has_regex_special(const char *s) {
    for (const char *p = s; *p; p++) {
        if (strchr("\\.+*?()|[]{}^$", *p)) return true;
    }
    return false;
}

bool mm_delimiter_init(mm_delimiter_t *delim, const char *spec) {
    memset(delim, 0, sizeof(*delim));
    if (!spec) return true;
    /* fzf expands a literal "\t" into a tab. */
    char *str = NULL;
    if (strstr(spec, "\\t")) {
        size_t n = strlen(spec);
        str = malloc(n + 1);
        size_t j = 0;
        for (size_t i = 0; i < n; i++) {
            if (spec[i] == '\\' && i + 1 < n && spec[i + 1] == 't') { str[j++] = '\t'; i++; }
            else str[j++] = spec[i];
        }
        str[j] = 0;
    } else {
        str = strdup(spec);
    }
    if (!str) return false;

    size_t rune_len = (size_t)mm_utf8_count(str);
    if (rune_len == 1 || !has_regex_special(str)) {
        delim->str = str;
        delim->slen = (int)strlen(str);
        delim->is_regex = false;
        return true;
    }
    regex_t *rx = malloc(sizeof(regex_t));
    if (!rx) { free(str); return false; }
    if (regcomp(rx, str, REG_EXTENDED) != 0) {
        /* Not a valid regular expression: fzf falls back to a literal
           delimiter. */
        free(rx);
        delim->str = str;
        delim->slen = (int)strlen(str);
        delim->is_regex = false;
        return true;
    }
    delim->str = str;
    delim->slen = (int)strlen(str);
    delim->is_regex = true;
    delim->rx = rx;
    return true;
}

void mm_delimiter_free(mm_delimiter_t *delim) {
    if (delim->rx) {
        regfree((regex_t *)delim->rx);
        free(delim->rx);
        delim->rx = NULL;
    }
    free((void *)delim->str);
    delim->str = NULL;
    delim->slen = 0;
    delim->is_regex = false;
}

char **mm_tokenize(const char *text, mm_delimiter_t delim, int **prefix_lengths, int *outn) {
    if (delim.is_regex && delim.rx) {
        /* Regex delimiter: tokens include the matched delimiter, like
           fzf's Tokenize (src/tokenizer.go). */
        regex_t *rx = (regex_t *)delim.rx;
        int cap = 8, n = 0;
        char **ret = malloc((size_t)cap * sizeof(char *));
        if (!ret) { *outn = 0; *prefix_lengths = NULL; return NULL; }
        size_t begin = 0;
        size_t tlen = strlen(text);
        regmatch_t m[1];
        while (regexec(rx, text + begin, 1, m, begin == 0 ? 0 : REG_NOTBOL) == 0) {
            size_t mend = begin + (size_t)m[0].rm_eo;
            size_t seglen = mend - begin;
            if (n == cap) { cap *= 2; ret = realloc(ret, (size_t)cap * sizeof(char *)); }
            ret[n++] = strndup(text + begin, seglen);
            if (mend == begin) break; /* empty match, avoid looping */
            begin = mend;
            if (begin >= tlen) break;
        }
        if (begin < tlen) {
            if (n == cap) { cap *= 2; ret = realloc(ret, (size_t)cap * sizeof(char *)); }
            ret[n++] = strndup(text + begin, tlen - begin);
        }
        if (*text == 0) *outn = 0;
        else *outn = n;
        int *pl = malloc((size_t)(n ? n : 1) * sizeof(int));
        int plen = 0;
        for (int i = 0; i < n; i++) { pl[i] = plen; plen += mm_utf8_count(ret[i]); }
        *prefix_lengths = pl;
        return ret;
    }
    if (!delim.str) {
        int prefix = 0;
        char **toks = awk_tokenize(text, &prefix, outn);
        int *pl = malloc((size_t)(*outn ? *outn : 1) * sizeof(int));
        int plen = prefix;
        for (int i = 0; i < *outn; i++) {
            pl[i] = plen;
            plen += mm_utf8_count(toks[i]);
        }
        *prefix_lengths = pl;
        return toks;
    }
    /* string delimiter: SplitAfter */
    int cap = 8, n = 0;
    char **ret = malloc((size_t)cap * sizeof(char *));
    if (!ret) { *outn = 0; *prefix_lengths = NULL; return NULL; }
    const char *p = text;
    size_t dlen = (size_t)delim.slen;
    while (*p) {
        const char *hit = strstr(p, delim.str);
        size_t seg;
        if (hit) seg = (size_t)(hit - p) + dlen;
        else seg = strlen(p);
        if (n == cap) { cap *= 2; ret = realloc(ret, (size_t)cap * sizeof(char *)); }
        ret[n++] = strndup(p, seg);
        p += seg;
    }
    if (*text == 0) *outn = 0;
    else *outn = n;
    int *pl = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!pl) { for (int i = 0; i < n; i++) free(ret[i]); free(ret); *outn = 0; *prefix_lengths = NULL; return NULL; }
    int plen = 0;
    for (int i = 0; i < n; i++) { pl[i] = plen; plen += mm_utf8_count(ret[i]); }
    *prefix_lengths = pl;
    return ret;
}

char *mm_strip_last_delimiter(const char *str, mm_delimiter_t delim) {
    if (delim.str) {
        size_t n = strlen(str), d = (size_t)delim.slen;
        if (d > 0 && n >= d && strncmp(str + n - d, delim.str, d) == 0)
            return strndup(str, n - d);
        return strdup(str);
    }
    /* awk: TrimRight whitespace (truncate at first trailing non-space run) */
    size_t n = strlen(str);
    while (n > 0 && is_awk_white(str[n - 1])) n--;
    return strndup(str, n);
}

static int min_i(int a, int b) { return a < b ? a : b; }
static int max_i(int a, int b) { return a > b ? a : b; }

char **mm_transform(char **tokens, int ntokens, int *tok_prefix, const mm_range_t *ranges,
                    int nranges, int **prefix_lengths, int *outn) {
    char **out = malloc((size_t)nranges * sizeof(char *));
    int *pl = malloc((size_t)nranges * sizeof(int));
    if (!out || !pl) { free(out); free(pl); *outn = 0; *prefix_lengths = NULL; return NULL; }
    for (int idx = 0; idx < nranges; idx++) {
        mm_range_t r = ranges[idx];
        int minIdx = 0;
        char *merged = NULL;
        if (r.begin == r.end) {
            int i = r.begin;
            if (i == MM_RANGE_ELLIPSIS) {
                size_t tot = 0;
                for (int k = 0; k < ntokens; k++) tot += strlen(tokens[k]);
                merged = malloc(tot + 1);
                merged[0] = 0;
                for (int k = 0; k < ntokens; k++) strcat(merged, tokens[k]);
            } else {
                if (i < 0) i += ntokens + 1;
                if (i >= 1 && i <= ntokens) { minIdx = i - 1; merged = strdup(tokens[i - 1]); }
            }
        } else {
            int begin, end;
            if (r.begin == MM_RANGE_ELLIPSIS) { begin = 1; end = r.end; if (end < 0) end += ntokens + 1; }
            else if (r.end == MM_RANGE_ELLIPSIS) { begin = r.begin; end = ntokens; if (begin < 0) begin += ntokens + 1; }
            else { begin = r.begin; end = r.end; if (begin < 0) begin += ntokens + 1; if (end < 0) end += ntokens + 1; }
            minIdx = max_i(0, begin - 1);
            size_t tot = 0;
            for (int i = begin; i <= end; i++) if (i >= 1 && i <= ntokens) tot += strlen(tokens[i - 1]);
            merged = malloc(tot + 1);
            merged[0] = 0;
            for (int i = begin; i <= end; i++) if (i >= 1 && i <= ntokens) strcat(merged, tokens[i - 1]);
        }
        if (!merged) merged = strdup("");
        out[idx] = merged;
        pl[idx] = (minIdx < ntokens && tok_prefix) ? tok_prefix[minIdx] : 0;
    }
    (void)min_i;
    *outn = nranges;
    *prefix_lengths = pl;
    return out;
}
