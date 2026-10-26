/* pattern.c - see pattern.h. Ported from fzf src/pattern.go (v0.74.3). */
#include "pattern.h"
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <stdint.h>
#include <wchar.h>
#include <wctype.h>

static wchar_t *decode(const char *s, int *outlen) {
    int n = mm_utf8_count(s);
    wchar_t *r = malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!r) { *outlen = 0; return NULL; }
    const char *p = s;
    size_t rem = strlen(s);
    int i = 0;
    while (rem > 0) {
        wchar_t cp;
        size_t used = mm_utf8_decode(p, rem, &cp);
        r[i++] = cp;
        p += used;
        rem -= used;
    }
    r[i] = 0;
    *outlen = i;
    return r;
}

static wchar_t lower_ascii(wchar_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return (wchar_t)towlower((wint_t)c);
}

static wchar_t *normalize_runes(wchar_t *r, int n) {
    for (int i = 0; i < n; i++) r[i] = mm_algo_normalize_rune(r[i]);
    return r;
}

static bool runes_equal(const wchar_t *a, const wchar_t *b, int n) {
    for (int i = 0; i < n; i++) if (a[i] != b[i]) return false;
    return true;
}

/* Grow match-position buffers geometrically; extended queries can aggregate
   many term matches, so exact-size reallocations make construction quadratic. */
static bool int_buffer_reserve(int **buf, int *cap, int need) {
    if (need < 0) return false;
    if (need <= *cap) return true;
    size_t ncap = *cap > 0 ? (size_t)*cap : 16u;
    while (ncap < (size_t)need) {
        if (ncap > (size_t)INT_MAX / 2u) {
            ncap = (size_t)need;
            break;
        }
        ncap *= 2u;
    }
    if (ncap > (size_t)INT_MAX || ncap > SIZE_MAX / sizeof(**buf))
        return false;
    int *next = realloc(*buf, ncap * sizeof(**buf));
    if (!next) return false;
    *buf = next;
    *cap = (int)ncap;
    return true;
}

/* Split on runs of spaces ("> +"). Handles "\ " as literal (replaced by TAB
   first, then restored). Returns array of token strings (malloc'd wchar arrays)
   and count. */
static void free_tokens(wchar_t **t, int n);

static wchar_t **split_tokens(const wchar_t *s, int n, int *outcount) {
    /* Replace "\ " with TAB marker, then split on spaces. */
    wchar_t *buf = malloc((size_t)(n + 1) * sizeof(wchar_t));
    if (!buf) { *outcount = 0; return NULL; }
    int m = 0;
    for (int i = 0; i < n; i++) {
        if (s[i] == '\\' && i + 1 < n && s[i + 1] == ' ') { buf[m++] = 0x09; i++; }
        else buf[m++] = s[i];
    }

    int cap = 16, cnt = 0;
    wchar_t **toks = malloc((size_t)cap * sizeof(wchar_t *));
    if (!toks) { free(buf); *outcount = 0; return NULL; }
    int start = -1;
    for (int i = 0; i <= m; i++) {
        bool is_space = (i < m && buf[i] == ' ');
        if (!is_space && start < 0) start = i;
        if ((is_space || i == m) && start >= 0) {
            int len = i - start;
            wchar_t *t = malloc((size_t)(len + 1) * sizeof(wchar_t));
            if (!t) {
                free(buf);
                free_tokens(toks, cnt);
                *outcount = 0;
                return NULL;
            }
            for (int j = 0; j < len; j++) t[j] = buf[start + j] == 0x09 ? ' ' : buf[start + j];
            t[len] = 0;
            if (cnt == cap) {
                int ncap = cap * 2;
                wchar_t **nt = realloc(toks, (size_t)ncap * sizeof(wchar_t *));
                if (!nt) {
                    free(t);
                    free(buf);
                    free_tokens(toks, cnt);
                    *outcount = 0;
                    return NULL;
                }
                toks = nt; cap = ncap;
            }
            toks[cnt++] = t;
            start = -1;
        }
    }
    free(buf);
    *outcount = cnt;
    return toks;
}

static void free_tokens(wchar_t **t, int n) {
    for (int i = 0; i < n; i++) free(t[i]);
    free(t);
}

/* Release partially parsed query terms on every allocation-failure path. */
static void parse_terms_cleanup(mm_termset_t *sets, int setcnt,
                                mm_term_t *set, int setn,
                                wchar_t **tokens, int ntoks) {
    for (int i = 0; i < setcnt; i++) {
        for (int j = 0; j < sets[i].count; j++)
            free(sets[i].terms[j].text);
        free(sets[i].terms);
    }
    for (int j = 0; j < setn; j++)
        free(set[j].text);
    free(set);
    free(sets);
    free_tokens(tokens, ntoks);
}

/* parse terms from an extended query */
static bool parse_terms(bool fuzzy, mm_case_t case_mode, bool normalize,
                        const wchar_t *str, int slen,
                        mm_termset_t **out_sets, int *out_nsets) {
    int ntoks = 0;
    wchar_t **tokens = split_tokens(str, slen, &ntoks);
    if (!tokens) { *out_sets = NULL; *out_nsets = 0; return false; }

    int setcap = 8, setcnt = 0;
    mm_termset_t *sets = malloc((size_t)setcap * sizeof(mm_termset_t));
    if (!sets) { free_tokens(tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }

    mm_term_t *set = NULL;
    int setn = 0, setalloc = 0;
    bool switch_set = false;
    bool after_bar = false;

    for (int ti = 0; ti < ntoks; ti++) {
        wchar_t *text = tokens[ti];
        int tlen = (int)wcslen(text);
        /* lowerText */
        wchar_t *lower = malloc((size_t)(tlen + 1) * sizeof(wchar_t));
        if (!lower) { parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }
        for (int i = 0; i < tlen; i++) lower[i] = lower_ascii(text[i]);
        lower[tlen] = 0;

        mm_term_type_t typ = fuzzy ? TERM_FUZZY : TERM_EXACT;
        bool inv = false;
        bool case_sensitive = (case_mode == CASE_RESPECT) ||
            (case_mode == CASE_SMART && !runes_equal(text, lower, tlen));
        bool norm = false;
        if (normalize) {
            wchar_t *nn = malloc((size_t)(tlen + 1) * sizeof(wchar_t));
            if (!nn) { free(lower); parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }
            for (int i = 0; i < tlen; i++) nn[i] = mm_algo_normalize_rune(lower[i]);
            nn[tlen] = 0;
            norm = runes_equal(lower, nn, tlen);
            free(nn);
        } else norm = false;

        wchar_t *finaltext = text;
        int ftlen = tlen;
        wchar_t *owned = NULL;
        if (!case_sensitive) {
            owned = lower;
            finaltext = lower;
            ftlen = tlen;
        } else {
            free(lower);
            lower = NULL;
        }

        /* OR operator */
        if (setn > 0 && !after_bar && runes_equal(finaltext, L"|", ftlen) && ftlen == 1) {
            switch_set = false;
            after_bar = true;
            free(owned);
            continue;
        }
        after_bar = false;

        wchar_t *tbuf = malloc((size_t)(ftlen + 1) * sizeof(wchar_t));
        if (!tbuf) { free(owned); parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }
        wmemcpy(tbuf, finaltext, (size_t)ftlen);
        tbuf[ftlen] = 0;
        int tl = ftlen;

        if (tl > 0 && tbuf[0] == '!') {
            inv = true;
            typ = TERM_EXACT;
            wmemmove(tbuf, tbuf + 1, (size_t)(tl - 1));
            tl--;
        }

        if (tl > 0 && !(tl == 1 && tbuf[0] == '$') && tbuf[tl - 1] == '$') {
            typ = TERM_SUFFIX;
            tl--;
        }

        if (tl > 2 && tbuf[0] == '\'' && tbuf[tl - 1] == '\'') {
            typ = TERM_EXACT_BOUNDARY;
            wmemmove(tbuf, tbuf + 1, (size_t)(tl - 2));
            tl -= 2;
        } else if (tl > 0 && tbuf[0] == '\'') {
            if (fuzzy && !inv) typ = TERM_EXACT;
            else typ = TERM_FUZZY;
            wmemmove(tbuf, tbuf + 1, (size_t)(tl - 1));
            tl--;
        } else if (tl > 0 && tbuf[0] == '^') {
            if (typ == TERM_SUFFIX) typ = TERM_EQUAL;
            else typ = TERM_PREFIX;
            wmemmove(tbuf, tbuf + 1, (size_t)(tl - 1));
            tl--;
        }

        tbuf[tl] = 0;

        if (tl > 0) {
            if (switch_set) {
                if (setcnt == setcap) {
                    int ncap = setcap * 2;
                    mm_termset_t *ns = realloc(sets, (size_t)ncap * sizeof(mm_termset_t));
                    if (!ns) { parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }
                    sets = ns; setcap = ncap;
                }
                sets[setcnt].terms = set; sets[setcnt].count = setn;
                setcnt++;
                set = NULL; setn = 0; setalloc = 0;
            }
            wchar_t *textRunes = malloc((size_t)(tl + 1) * sizeof(wchar_t));
            if (!textRunes) {
                free(tbuf);
                free(owned);
                parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks);
                *out_sets = NULL;
                *out_nsets = 0;
                return false;
            }
            wmemcpy(textRunes, tbuf, (size_t)tl);
            textRunes[tl] = 0;
            if (norm) normalize_runes(textRunes, tl);
            if (setn == setalloc) {
                int nalloc = setalloc ? setalloc * 2 : 4;
                mm_term_t *nt = realloc(set, (size_t)nalloc * sizeof(mm_term_t));
                if (!nt) {
                    free(textRunes);
                    free(tbuf);
                    free(owned);
                    parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks);
                    *out_sets = NULL;
                    *out_nsets = 0;
                    return false;
                }
                set = nt; setalloc = nalloc;
            }
            set[setn].typ = typ;
            set[setn].inv = inv;
            set[setn].case_sensitive = case_sensitive;
            set[setn].normalize = norm;
            set[setn].text = textRunes;
            set[setn].tlen = tl;
            setn++;
            switch_set = true;
        }
        free(tbuf);
        free(owned);
    }
    if (setn > 0) {
        if (setcnt == setcap) {
            int ncap = setcap * 2;
            mm_termset_t *ns = realloc(sets, (size_t)ncap * sizeof(mm_termset_t));
            if (!ns) { parse_terms_cleanup(sets, setcnt, set, setn, tokens, ntoks); *out_sets = NULL; *out_nsets = 0; return false; }
            sets = ns; setcap = ncap;
        }
        sets[setcnt].terms = set; sets[setcnt].count = setn;
        setcnt++;
    }
    free_tokens(tokens, ntoks);
    *out_sets = sets;
    *out_nsets = setcnt;
    return true;
}

mm_pattern_t *mm_pattern_build(const char *query, bool fuzzy, bool extended,
                               mm_case_t case_mode, bool normalize, bool forward,
                               bool with_pos) {
    if (!query) return NULL;
    int qlen = 0;
    wchar_t *runes = decode(query, &qlen);
    if (!runes) return NULL;

    /* For extended mode, trim leading spaces and trailing spaces unless "\ ". */
    if (extended) {
        int start = 0;
        while (start < qlen && runes[start] == ' ') start++;
        int end = qlen;
        while (end > start && runes[end - 1] == ' ' &&
               !(end >= 2 && runes[end - 2] == '\\')) end--;
        wmemmove(runes, runes + start, (size_t)(end - start));
        qlen = end - start;
        runes[qlen] = 0;
    }

    mm_pattern_t *p = calloc(1, sizeof(mm_pattern_t));
    if (!p) { free(runes); return NULL; }
    p->fuzzy = fuzzy;
    p->extended = extended;
    p->normalize = normalize;
    p->forward = forward;
    p->with_pos = with_pos;
    p->text = runes;
    p->textlen = qlen;
    p->sortable = true;

    if (extended) {
        if (!parse_terms(fuzzy, case_mode, normalize, runes, qlen,
                         &p->term_sets, &p->ntermsets)) {
            mm_pattern_free(p);
            return NULL;
        }
        p->sortable = false;
        for (int i = 0; i < p->ntermsets; i++) {
            for (int j = 0; j < p->term_sets[i].count; j++) {
                if (!p->term_sets[i].terms[j].inv) { p->sortable = true; break; }
            }
            if (p->sortable) break;
        }
    } else {
        /* basic mode: lower the query unless case-sensitive */
        wchar_t *lower = malloc(((size_t)qlen + 1) * sizeof(wchar_t));
        if (!lower) {
            mm_pattern_free(p);
            return NULL;
        }
        for (int i = 0; i < qlen; i++) lower[i] = lower_ascii(runes[i]);
        lower[qlen] = 0;
        bool case_sensitive = (case_mode == CASE_RESPECT) ||
            (case_mode == CASE_SMART && !runes_equal(runes, lower, qlen));
        p->case_sensitive = case_sensitive;
        /* fzf only normalizes the text when the pattern itself is unchanged
           by normalization (src/pattern.go: normalize && lowerString ==
           string(algo.NormalizeRunes(...))). */
        p->normalize = false;
        if (normalize) {
            wchar_t *nn = malloc(((size_t)qlen + 1) * sizeof(wchar_t));
            if (!nn) {
                free(lower);
                mm_pattern_free(p);
                return NULL;
            }
            for (int i = 0; i < qlen; i++) nn[i] = mm_algo_normalize_rune(lower[i]);
            nn[qlen] = 0;
            p->normalize = runes_equal(lower, nn, qlen);
            free(nn);
        }
        if (!case_sensitive) {
            free(p->text);
            p->text = lower;
            p->textlen = qlen;
        } else {
            free(lower);
        }
    }
    return p;
}

void mm_pattern_free(mm_pattern_t *p) {
    if (!p) return;
    free(p->text);
    for (int i = 0; i < p->ntermsets; i++) {
        for (int j = 0; j < p->term_sets[i].count; j++)
            free(p->term_sets[i].terms[j].text);
        free(p->term_sets[i].terms);
    }
    free(p->term_sets);
    free(p);
}

void mm_pattern_set_sortable(mm_pattern_t *p, bool sortable) { p->sortable = sortable; }

bool mm_pattern_is_empty(const mm_pattern_t *p) {
    if (!p->extended) return p->textlen == 0;
    return p->ntermsets == 0;
}

/* Run one matcher by term type over the whole text. */
static mm_match_t run_term(const mm_term_t *t, const mm_text_t *text, bool forward, bool with_pos) {
    switch (t->typ) {
    case TERM_FUZZY:
        return mm_fuzzy_match(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    case TERM_EXACT:
        return mm_exact_match_naive(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    case TERM_EXACT_BOUNDARY:
        return mm_exact_match_boundary(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    case TERM_PREFIX:
        return mm_prefix_match(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    case TERM_SUFFIX:
        return mm_suffix_match(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    case TERM_EQUAL:
        return mm_equal_match(text, t->text, t->tlen, t->case_sensitive, t->normalize, forward, with_pos);
    }
    mm_match_t m = {0};
    m.result.start = -1;
    return m;
}

/* Run one term across a token list (fzf iter). On match, positions are shifted
   by the token's prefix_length. Returns first matching segment. */
static mm_match_t run_term_tokens(const mm_term_t *t, const mm_token_t *tokens, int ntokens,
                                  bool forward, bool with_pos) {
    mm_match_t best = {0};
    best.result.start = -1;
    for (int i = 0; i < ntokens; i++) {
        mm_match_t m = run_term(t, &tokens[i].text, forward, with_pos);
        if (m.result.start >= 0) {
            if (with_pos) {
                if (m.pos) {
                    for (int k = 0; k < m.npos; k++) m.pos[k] += tokens[i].prefix_length;
                } else {
                    int span = m.result.end - m.result.start;
                    m.pos = span > 0 ? malloc((size_t)span * sizeof(int)) : NULL;
                    if (span > 0 && !m.pos) {
                        mm_match_free(&m);
                        best.result.start = -2; /* distinguish OOM from no match */
                        return best;
                    }
                    for (int k = 0; k < span; k++)
                        m.pos[k] = m.result.start + k + tokens[i].prefix_length;
                    m.npos = span;
                }
            }
            m.result.start += tokens[i].prefix_length;
            m.result.end += tokens[i].prefix_length;
            return m;
        }
    }
    return best;
}

bool mm_pattern_match_tokens(const mm_pattern_t *p, const mm_token_t *tokens, int ntokens,
                             int *score, int **pos, int *npos) {
    if (pos) *pos = NULL;
    if (npos) *npos = 0;
    if (!score) return false;
    *score = 0;
    if (!p || ntokens < 0 || (ntokens > 0 && !tokens)) return false;

    if (mm_pattern_is_empty(p)) return true;

    if (!p->extended) {
        mm_term_t t = {0};
        t.typ = p->fuzzy ? TERM_FUZZY : TERM_EXACT;
        t.case_sensitive = p->case_sensitive;
        t.normalize = p->normalize;
        t.text = p->text;
        t.tlen = p->textlen;
        if (p->textlen == 0) { *score = 0; return true; }
        mm_match_t m = run_term_tokens(&t, tokens, ntokens, p->forward, p->with_pos);
        bool ok = m.result.start >= 0;
        if (ok) {
            *score = m.result.score;
            if (p->with_pos && pos && npos) { *pos = m.pos; *npos = m.npos; }
            else mm_match_free(&m);
        } else {
            mm_match_free(&m);
        }
        return ok;
    }

    int total = 0;
    int *allpos = NULL, alln = 0, allcap = 0;
    int matched_sets = 0;
    for (int i = 0; i < p->ntermsets; i++) {
        mm_termset_t *ts = &p->term_sets[i];
        bool matched = false;
        int cur_score = 0;
        int *curpos = NULL, curn = 0, curcap = 0;
        for (int j = 0; j < ts->count; j++) {
            mm_term_t *t = &ts->terms[j];
            mm_match_t m = run_term_tokens(t, tokens, ntokens, p->forward, p->with_pos);
            if (m.result.start == -2) {
                free(curpos);
                free(allpos);
                return false;
            }
            if (m.result.start >= 0) {
                if (t->inv) { mm_match_free(&m); continue; }
                matched = true;
                cur_score = m.result.score;
                if (p->with_pos) {
                    if (m.pos) {
                        if (m.npos > INT_MAX - curn ||
                            !int_buffer_reserve(&curpos, &curcap, curn + m.npos)) {
                            mm_match_free(&m); free(curpos); free(allpos); return false;
                        }
                        memcpy(curpos + curn, m.pos, (size_t)m.npos * sizeof(int));
                        curn += m.npos;
                    } else {
                        int span = m.result.end - m.result.start;
                        if (span < 0 || span > INT_MAX - curn ||
                            !int_buffer_reserve(&curpos, &curcap, curn + span)) {
                            mm_match_free(&m); free(curpos); free(allpos); return false;
                        }
                        for (int k = 0; k < span; k++) curpos[curn + k] = m.result.start + k;
                        curn += span;
                    }
                }
                mm_match_free(&m);
                break;
            } else if (t->inv) {
                matched = true;
                cur_score = 0;
                mm_match_free(&m);
                continue;
            } else {
                mm_match_free(&m);
            }
        }
        if (matched) {
            matched_sets++;
            total += cur_score;
            if (p->with_pos && curpos) {
                if (curn > INT_MAX - alln ||
                    !int_buffer_reserve(&allpos, &allcap, alln + curn)) {
                    free(curpos); free(allpos); return false;
                }
                memcpy(allpos + alln, curpos, (size_t)curn * sizeof(int));
                alln += curn;
            }
            free(curpos);
        }
    }
    bool ok = matched_sets == p->ntermsets && p->ntermsets > 0;
    if (ok) {
        *score = total;
        if (p->with_pos && pos && npos) { *pos = allpos; *npos = alln; }
        else free(allpos);
    } else {
        free(allpos);
    }
    return ok;
}

bool mm_pattern_match(const mm_pattern_t *p, const mm_text_t *text, int *score,
                      int **pos, int *npos) {
    if (!text) {
        if (score) *score = 0;
        if (pos) *pos = NULL;
        if (npos) *npos = 0;
        return false;
    }
    mm_token_t tok = { *text, 0 };
    return mm_pattern_match_tokens(p, &tok, 1, score, pos, npos);
}
