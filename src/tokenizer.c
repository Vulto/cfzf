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

static void free_tokens(char **tokens, int n) {
    if (!tokens) return;
    for (int i = 0; i < n; i++) free(tokens[i]);
    free(tokens);
}

static bool append_token(char ***tokens, int *count, int *capacity,
                         const char *text, size_t length) {
    if (*count == *capacity) {
        if (*capacity > INT_MAX / 2) return false;
        int next = *capacity ? *capacity * 2 : 8;
        if ((size_t)next > SIZE_MAX / sizeof(**tokens)) return false;
        char **grown = realloc(*tokens, (size_t)next * sizeof(**tokens));
        if (!grown) return false;
        *tokens = grown;
        *capacity = next;
    }
    char *token = strndup(text, length);
    if (!token) return false;
    (*tokens)[(*count)++] = token;
    return true;
}

/* awkTokenizer: tokens are \S+\s* ; returns prefixLength = leading whitespace
   rune count. */
static char **awk_tokenize(const char *input, int *prefix_length, int *outn) {
    int cap = 0, n = 0, prefix = 0;
    char **ret = NULL;
    int state = 0; /* 0=nil 1=black 2=white */
    size_t begin = 0, end = 0, len = strlen(input);
    for (size_t idx = 0; idx < len; idx++) {
        char r = input[idx];
        bool white = is_awk_white(r);
        switch (state) {
        case 0:
            if (white) prefix++;
            else { state = 1; begin = idx; end = idx + 1; }
            break;
        case 1:
            end = idx + 1;
            if (white) state = 2;
            break;
        case 2:
            if (white) end = idx + 1;
            else {
                if (!append_token(&ret, &n, &cap, input + begin, end - begin)) {
                    free_tokens(ret, n);
                    *prefix_length = 0;
                    *outn = 0;
                    return NULL;
                }
                state = 1;
                begin = idx;
                end = idx + 1;
            }
            break;
        }
    }
    if (begin < end && !append_token(&ret, &n, &cap, input + begin, end - begin)) {
        free_tokens(ret, n);
        *prefix_length = 0;
        *outn = 0;
        return NULL;
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

char **mm_tokenize(const char *text, mm_delimiter_t delim,
                   int **prefix_lengths, int *outn) {
    if (!prefix_lengths || !outn || !text) return NULL;
    *prefix_lengths = NULL;
    *outn = 0;

    int cap = 0, n = 0;
    char **tokens = NULL;
    if (delim.is_regex && delim.rx) {
        regex_t *rx = (regex_t *)delim.rx;
        size_t begin = 0, text_len = strlen(text);
        regmatch_t match[1];
        while (begin < text_len &&
               regexec(rx, text + begin, 1, match,
                       begin == 0 ? 0 : REG_NOTBOL) == 0) {
            if (match[0].rm_eo < 0) break;
            size_t end = begin + (size_t)match[0].rm_eo;
            if (end < begin || end > text_len ||
                !append_token(&tokens, &n, &cap, text + begin, end - begin)) {
                free_tokens(tokens, n);
                return NULL;
            }
            if (end == begin) break;
            begin = end;
        }
        if (begin < text_len &&
            !append_token(&tokens, &n, &cap, text + begin, text_len - begin)) {
            free_tokens(tokens, n);
            return NULL;
        }
    } else if (!delim.str) {
        int leading = 0;
        tokens = awk_tokenize(text, &leading, &n);
        if (n > 0 && !tokens) return NULL;
        int *prefix = malloc((size_t)(n ? n : 1) * sizeof(*prefix));
        if (!prefix) {
            free_tokens(tokens, n);
            return NULL;
        }
        int length = leading;
        for (int i = 0; i < n; i++) {
            prefix[i] = length;
            length += mm_utf8_count(tokens[i]);
        }
        *prefix_lengths = prefix;
        *outn = n;
        return tokens;
    } else {
        size_t delimiter_len = (size_t)delim.slen;
        if (delimiter_len == 0) {
            /* Empty literal delimiters cannot advance; treat the input as one token. */
            if (*text && !append_token(&tokens, &n, &cap, text, strlen(text)))
                return NULL;
        } else {
            const char *p = text;
            while (*p) {
                const char *hit = strstr(p, delim.str);
                size_t segment = hit ? (size_t)(hit - p) + delimiter_len : strlen(p);
                if (!append_token(&tokens, &n, &cap, p, segment)) {
                    free_tokens(tokens, n);
                    return NULL;
                }
                p += segment;
            }
        }
    }

    int *prefix = malloc((size_t)(n ? n : 1) * sizeof(*prefix));
    if (!prefix) {
        free_tokens(tokens, n);
        return NULL;
    }
    int length = 0;
    for (int i = 0; i < n; i++) {
        prefix[i] = length;
        int token_length = mm_utf8_count(tokens[i]);
        if (token_length > INT_MAX - length) {
            free(prefix);
            free_tokens(tokens, n);
            return NULL;
        }
        length += token_length;
    }
    *prefix_lengths = prefix;
    *outn = n;
    return tokens;
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

char **mm_transform(char **tokens, int ntokens, int *tok_prefix,
                    const mm_range_t *ranges, int nranges,
                    int **prefix_lengths, int *outn) {
    if (!prefix_lengths || !outn || nranges < 0 ||
        (nranges > 0 && !ranges) || ntokens < 0 ||
        (ntokens > 0 && !tokens)) return NULL;
    *prefix_lengths = NULL;
    *outn = 0;
    if (nranges == 0) return NULL;
    if ((size_t)nranges > SIZE_MAX / sizeof(char *) ||
        (size_t)nranges > SIZE_MAX / sizeof(int)) return NULL;

    char **out = calloc((size_t)nranges, sizeof(*out));
    int *prefix = calloc((size_t)nranges, sizeof(*prefix));
    if (!out || !prefix) {
        free(out);
        free(prefix);
        return NULL;
    }

    for (int idx = 0; idx < nranges; idx++) {
        int64_t begin, end;
        const mm_range_t range = ranges[idx];
        if (range.begin == range.end && range.begin != MM_RANGE_ELLIPSIS) {
            int64_t index = range.begin;
            if (index < 0) index += (int64_t)ntokens + 1;
            begin = index;
            end = index;
        } else {
            begin = range.begin == MM_RANGE_ELLIPSIS ? 1 : range.begin;
            end = range.end == MM_RANGE_ELLIPSIS ? ntokens : range.end;
            if (begin < 0) begin += (int64_t)ntokens + 1;
            if (end < 0) end += (int64_t)ntokens + 1;
        }

        if (range.begin == MM_RANGE_ELLIPSIS && range.end == MM_RANGE_ELLIPSIS) {
            begin = 1;
            end = ntokens;
        }
        int64_t prefix_index = 0;
        if (range.begin == range.end && range.begin != MM_RANGE_ELLIPSIS) {
            if (begin >= 1 && begin <= ntokens) prefix_index = begin - 1;
        } else if (begin > 1) {
            prefix_index = begin - 1;
        }
        if (tok_prefix && prefix_index < ntokens)
            prefix[idx] = tok_prefix[(int)prefix_index];

        int64_t first = begin < 1 ? 1 : begin;
        int64_t last = end > ntokens ? ntokens : end;
        size_t total = 0;
        if (first <= last) {
            for (int64_t i = first; i <= last; i++) {
                size_t len = strlen(tokens[i - 1]);
                if (len > SIZE_MAX - total) goto fail;
                total += len;
            }
        }
        if (total == SIZE_MAX) goto fail;
        out[idx] = malloc(total + 1);
        if (!out[idx]) goto fail;
        size_t used = 0;
        if (first <= last) {
            for (int64_t i = first; i <= last; i++) {
                size_t len = strlen(tokens[i - 1]);
                memcpy(out[idx] + used, tokens[i - 1], len);
                used += len;
            }
        }
        out[idx][used] = 0;
    }

    *prefix_lengths = prefix;
    *outn = nranges;
    return out;

fail:
    for (int i = 0; i < nranges; i++) free(out[i]);
    free(out);
    free(prefix);
    return NULL;
}
