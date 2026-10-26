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
#include <stdio.h>

static bool mm_accept_nth_chars(const char *s) {
    if (!s || !*s) return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++)
        if (!((*p >= '0' && *p <= '9') || *p == ',' ||
              *p == '-' || *p == '.')) return false;
    return true;
}

bool mm_accept_nth_validate(const char *spec) {
    if (!spec || !*spec) {
        fprintf(stderr, "invalid --accept-nth: empty expression\n");
        return false;
    }
    if (mm_accept_nth_chars(spec)) {
        mm_range_t *ranges = NULL;
        int nranges = 0;
        bool valid = mm_parse_ranges(spec, &ranges, &nranges);
        free(ranges);
        if (!valid)
            fprintf(stderr, "invalid --accept-nth: %s\n", spec);
        return valid;
    }
    bool found = false;
    for (const char *p = spec; *p; p++) {
        if (*p != '{') continue;
        const char *end = strchr(p + 1, '}');
        if (!end) continue;
        size_t len = (size_t)(end - (p + 1));
        if (len == 1 && p[1] == 'n') {
            found = true;
            break;
        }
        bool numeric = len > 0;
        for (size_t i = 0; i < len; i++) {
            char c = p[1 + i];
            if (!((c >= '0' && c <= '9') || c == ',' ||
                  c == '-' || c == '.')) {
                numeric = false;
                break;
            }
        }
        if (numeric) { found = true; break; }
    }
    if (!found)
        fprintf(stderr, "invalid --accept-nth: template should include at least 1 placeholder: %s\n", spec);
    return found;
}

static bool append_text(char **buf, size_t *length, size_t *capacity,
                        const char *text, size_t count) {
    if (*length == SIZE_MAX || count > SIZE_MAX - *length - 1) return false;
    size_t needed = *length + count + 1;
    if (needed > *capacity) {
        size_t next = *capacity ? *capacity : 64;
        while (next < needed) {
            if (next > SIZE_MAX / 2) { next = needed; break; }
            next *= 2;
        }
        char *grown = realloc(*buf, next);
        if (!grown) return false;
        *buf = grown;
        *capacity = next;
    }
    if (count) memcpy(*buf + *length, text, count);
    *length += count;
    (*buf)[*length] = 0;
    return true;
}

static char *accept_nth_fields(const char *line, mm_delimiter_t delim,
                               const char *expression, bool strip_last) {
    mm_range_t *ranges = NULL;
    int nranges = 0;
    if (!mm_parse_ranges(expression, &ranges, &nranges)) return NULL;
    int *prefix = NULL, ntokens = 0;
    char **tokens = mm_tokenize(line, delim, &prefix, &ntokens);
    if (!tokens) {
        free(ranges);
        free(prefix);
        return NULL;
    }
    int *out_prefix = NULL, nout = 0;
    char **out = mm_transform(tokens, ntokens, prefix, ranges, nranges,
                              &out_prefix, &nout);
    free(ranges);
    free(prefix);
    for (int i = 0; i < ntokens; i++) free(tokens[i]);
    free(tokens);
    free(out_prefix);
    if (!out) return NULL;

    size_t length = 0;
    for (int i = 0; i < nout; i++) {
        size_t part = strlen(out[i]);
        if (length == SIZE_MAX || part > SIZE_MAX - length - 1) {
            for (int j = 0; j < nout; j++) free(out[j]);
            free(out);
            return NULL;
        }
        length += part;
    }
    char *joined = malloc(length + 1);
    if (!joined) {
        for (int i = 0; i < nout; i++) free(out[i]);
        free(out);
        return NULL;
    }
    size_t offset = 0;
    for (int i = 0; i < nout; i++) {
        size_t part = strlen(out[i]);
        memcpy(joined + offset, out[i], part);
        offset += part;
        free(out[i]);
    }
    joined[offset] = 0;
    free(out);
    if (!strip_last) return joined;
    char *stripped = mm_strip_last_delimiter(joined, delim);
    free(joined);
    return stripped;
}

char *mm_accept_nth_expand(const char *spec, const char *line,
                               mm_delimiter_t delim, int index) {
    if (mm_accept_nth_chars(spec))
        return accept_nth_fields(line, delim, spec, false);

    char *out = NULL;
    size_t length = 0, capacity = 0;
    for (size_t i = 0; spec[i];) {
        if (spec[i] == '{') {
            const char *end = strchr(spec + i + 1, '}');
            if (end) {
                size_t token_len = (size_t)(end - (spec + i + 1));
                const char *token = spec + i + 1;
                if (token_len == 1 && token[0] == 'n') {
                    char number[32];
                    int n = snprintf(number, sizeof(number), "%d", index);
                    if (n < 0 || !append_text(&out, &length, &capacity,
                                             number, (size_t)n)) goto fail;
                    i = (size_t)(end - spec) + 1;
                    continue;
                }
                bool numeric = token_len > 0;
                for (size_t j = 0; j < token_len; j++)
                    if (!((token[j] >= '0' && token[j] <= '9') ||
                          token[j] == ',' || token[j] == '-' ||
                          token[j] == '.')) {
                        numeric = false;
                        break;
                    }
                if (numeric) {
                    char *expression = strndup(token, token_len);
                    if (!expression) goto fail;
                    mm_range_t *ranges = NULL;
                    int nranges = 0;
                    bool valid = mm_parse_ranges(expression, &ranges, &nranges);
                    free(ranges);
                    if (valid) {
                        char *fields = accept_nth_fields(line, delim,
                                                         expression, true);
                        if (!fields) {
                            free(expression);
                            goto fail;
                        }
                        bool ok = append_text(&out, &length, &capacity,
                                              fields, strlen(fields));
                        free(fields);
                        if (!ok) {
                            free(expression);
                            goto fail;
                        }
                    }
                    free(expression);
                    i = (size_t)(end - spec) + 1;
                    continue;
                }
            }
        }
        if (!append_text(&out, &length, &capacity, spec + i, 1)) goto fail;
        i++;
    }
    if (!out) out = strdup("");
    return out;
fail:
    free(out);
    return NULL;
}

static mm_range_t normalize_range(int begin, int end) {
    if (begin == 1 && end != 1) begin = MM_RANGE_ELLIPSIS;
    if (end == -1) end = MM_RANGE_ELLIPSIS;
    return (mm_range_t){.begin = begin, .end = end};
}

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
    const char *dots = strstr(s, "..");
    if (!dots) {
        int value;
        if (!parse_range_int(s, strlen(s), &value) || value == 0) return false;
        *out = normalize_range(value, value);
        return true;
    }

    /* More than one range separator is not a valid nth expression. */
    if (strstr(dots + 2, "..")) return false;
    size_t left_len = (size_t)(dots - s);
    const char *right = dots + 2;
    int begin = MM_RANGE_ELLIPSIS, end = MM_RANGE_ELLIPSIS;
    if (left_len != 0 &&
        (!parse_range_int(s, left_len, &begin) || begin == 0))
        return false;
    if (*right && (!parse_range_int(right, strlen(right), &end) || end == 0))
        return false;
    if (left_len != 0 && *right && begin < 0 && end > 0)
        return false;
    *out = normalize_range(begin, end);
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

static bool is_awk_token_white(char c) {
    return c == '\t' || c == ' ' || c == '\n';
}

static bool is_unicode_space(wchar_t c) {
    return (c >= 0x0009 && c <= 0x000D) || c == 0x0020 ||
           c == 0x0085 || c == 0x00A0 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A) || c == 0x2028 ||
           c == 0x2029 || c == 0x202F || c == 0x205F ||
           c == 0x3000;
}

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
    int cap = 8, n = 0, prefix = 0;
    char **ret = malloc((size_t)cap * sizeof(*ret));
    if (!ret) {
        *prefix_length = 0;
        *outn = 0;
        return NULL;
    }
    int state = 0; /* 0=nil 1=black 2=white */
    size_t begin = 0, end = 0, length = strlen(input);
    for (size_t idx = 0; idx < length; idx++) {
        char c = input[idx];
        bool white = is_awk_token_white(c);
        switch (state) {
        case 0:
            if (white) {
                if (prefix == INT_MAX) {
                    free_tokens(ret, n);
                    *prefix_length = 0;
                    *outn = 0;
                    return NULL;
                }
                prefix++;
            } else {
                state = 1;
                begin = idx;
                end = idx + 1;
            }
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
        const size_t text_len = strlen(text);
        size_t token_begin = 0, search = 0, last_end = 0;
        bool last_match_nonempty = false;
        regmatch_t match[1];
        for (;;) {
            int rc = regexec(rx, text + search, 1, match,
                             search == 0 ? 0 : REG_NOTBOL);
            if (rc == REG_NOMATCH) break;
            if (rc != 0 || match[0].rm_so < 0 || match[0].rm_eo < 0) {
                free_tokens(tokens, n);
                return NULL;
            }
            size_t match_start = search + (size_t)match[0].rm_so;
            size_t match_end = search + (size_t)match[0].rm_eo;
            if (match_start < search || match_end < match_start ||
                match_end > text_len) {
                free_tokens(tokens, n);
                return NULL;
            }
            bool empty_match = match_start == match_end;
            if (empty_match && last_match_nonempty &&
                match_start == last_end) {
                if (match_start == text_len) break;
                wchar_t rune;
                size_t used = mm_utf8_decode(text + match_start,
                                             text_len - match_start, &rune);
                search = match_start + (used && used <= text_len - match_start
                                        ? used : 1);
                last_match_nonempty = false;
                continue;
            }
            if (!append_token(&tokens, &n, &cap, text + token_begin,
                              match_end - token_begin)) {
                free_tokens(tokens, n);
                return NULL;
            }
            token_begin = match_end;
            last_end = match_end;
            last_match_nonempty = !empty_match;
            if (match_end == text_len) {
                if (empty_match) break;
                break;
            }
            if (empty_match) {
                wchar_t rune;
                size_t used = mm_utf8_decode(text + match_end,
                                             text_len - match_end, &rune);
                search = match_end + (used && used <= text_len - match_end
                                      ? used : 1);
            } else {
                search = match_end;
            }
        }
        if (token_begin < text_len &&
            !append_token(&tokens, &n, &cap, text + token_begin,
                          text_len - token_begin)) {
            free_tokens(tokens, n);
            return NULL;
        }
    } else if (!delim.str) {
        int leading = 0;
        tokens = awk_tokenize(text, &leading, &n);
        if (!tokens) return NULL;
        int *prefix = malloc((size_t)(n ? n : 1) * sizeof(*prefix));
        if (!prefix) {
            free_tokens(tokens, n);
            return NULL;
        }
        int length = leading;
        for (int i = 0; i < n; i++) {
            int token_length = mm_utf8_count(tokens[i]);
            if (token_length < 0 || token_length > INT_MAX - length) {
                free(prefix);
                free_tokens(tokens, n);
                return NULL;
            }
            prefix[i] = length;
            length += token_length;
        }
        *prefix_lengths = prefix;
        *outn = n;
        return tokens;
    } else {
        size_t delimiter_len = (size_t)delim.slen;
        if (delimiter_len == 0) {
            /* strings.SplitAfter(text, "") splits at UTF-8 rune boundaries. */
            size_t length = strlen(text), offset = 0;
            while (offset < length) {
                wchar_t rune;
                size_t used = mm_utf8_decode(text + offset, length - offset, &rune);
                if (used == 0 || used > length - offset) used = 1;
                if (!append_token(&tokens, &n, &cap, text + offset, used)) {
                    free_tokens(tokens, n);
                    return NULL;
                }
                offset += used;
            }
        } else {
            const char *p = text;
            for (;;) {
                const char *hit = strstr(p, delim.str);
                size_t segment = hit ? (size_t)(hit - p) + delimiter_len : strlen(p);
                if (!append_token(&tokens, &n, &cap, p, segment)) {
                    free_tokens(tokens, n);
                    return NULL;
                }
                if (!hit) break;
                p += segment;
                if (!*p) {
                    if (!append_token(&tokens, &n, &cap, p, 0)) {
                        free_tokens(tokens, n);
                        return NULL;
                    }
                    break;
                }
            }
        }
    }

    if (!tokens) {
        tokens = calloc(1, sizeof(*tokens));
        if (!tokens) return NULL;
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
        if (token_length < 0 || token_length > INT_MAX - length) {
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
    if (delim.is_regex && delim.rx) {
        regex_t *rx = (regex_t *)delim.rx;
        size_t length = strlen(str), begin = 0;
        size_t last_start = 0, last_end = 0;
        bool found = false;
        regmatch_t match[1];
        while (begin <= length &&
               regexec(rx, str + begin, 1, match,
                       begin == 0 ? 0 : REG_NOTBOL) == 0) {
            if (match[0].rm_so < 0 || match[0].rm_eo < 0) break;
            size_t start = begin + (size_t)match[0].rm_so;
            size_t end = begin + (size_t)match[0].rm_eo;
            if (start > length || end > length || end < start) break;
            last_start = start;
            last_end = end;
            found = true;
            if (end > begin) {
                begin = end;
            } else if (start < length) {
                wchar_t rune;
                size_t used = mm_utf8_decode(str + start, length - start, &rune);
                begin = start + (used ? used : 1);
            } else {
                break;
            }
        }
        return found && last_end == length
            ? strndup(str, last_start) : strdup(str);
    }
    if (delim.str) {
        size_t n = strlen(str), d = (size_t)delim.slen;
        if (d > 0 && n >= d && strncmp(str + n - d, delim.str, d) == 0)
            return strndup(str, n - d);
        return strdup(str);
    }
    /* awk: TrimRight Unicode whitespace, matching Go's unicode.IsSpace. */
    size_t length = strlen(str), idx = 0, trim_end = 0;
    while (idx < length) {
        wchar_t rune = 0;
        size_t used = mm_utf8_decode(str + idx, length - idx, &rune);
        if (used == 0 || used > length - idx) used = 1;
        if (!is_unicode_space(rune)) trim_end = idx + used;
        idx += used;
    }
    return strndup(str, trim_end);
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
