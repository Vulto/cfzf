/* filter.c - see filter.h */
#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

int *mm_batch_search(mm_input_t *input, mm_pattern_t *pat, const mm_sort_criteria_t *crit,
                     bool tac, int *out_n, mm_match_pos_t **out_pos) {
    int total = input->count;
    if (total == 0) {
        *out_n = 0;
        if (out_pos) *out_pos = NULL;
        return malloc(sizeof(int));
    }

    /* An empty query matches every item without scoring. Preserve input order
       (or reverse it for --tac) directly: decoding, allocating one result per
       item, computing sort keys, and sorting cannot change the result. This
       path is especially important while a live input stream is still adding
       items, because the UI refreshes the result list as the stream grows. */
    if (mm_pattern_is_empty(pat)) {
        int *out = malloc((size_t)total * sizeof(*out));
        if (!out) {
            *out_n = 0;
            if (out_pos) *out_pos = NULL;
            return NULL;
        }
        if (tac) {
            for (int i = 0; i < total; i++) out[i] = total - 1 - i + input->base;
        } else {
            for (int i = 0; i < total; i++) out[i] = i + input->base;
        }
        if (out_pos) {
            *out_pos = calloc((size_t)total, sizeof(**out_pos));
            if (!*out_pos) {
                free(out);
                *out_n = 0;
                return NULL;
            }
        }
        *out_n = total;
        return out;
    }

    /* Try to use threading if beneficial */
    int nthreads = 4;
    const char *te = getenv("FZF_NUM_THREADS");
    if (te && *te) nthreads = atoi(te);
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    if (total < 10000) nthreads = 1;

    /* Single threaded path for simplicity and correctness */
    if (nthreads == 1) {
        mm_result_t2 *results = malloc((size_t)total * sizeof(mm_result_t2));
        mm_item_t *items = malloc((size_t)total * sizeof(mm_item_t));
        int n = 0;
        if (!results || !items) {
            free(results);
            free(items);
            *out_n = 0;
            return NULL;
        }
        size_t max_runes = 0;
        for (int i = 0; i < total; i++) {
            size_t n2 = (size_t)strlen(input->lines[i]) + 1;
            if (n2 > max_runes) max_runes = n2;
        }
        wchar_t *scratch = max_runes ? malloc(max_runes * sizeof(wchar_t)) : NULL;
        if (max_runes && !scratch) {
            free(results);
            free(items);
            *out_n = 0;
            return NULL;
        }
        bool own_items = input->items == NULL;
        for (int i = 0; i < total; i++) {
            if (own_items) {
                mm_item_init(&items[i], input->lines[i], i + input->base);
                if (!mchars_decode_into(&items[i].text, scratch, (int)max_runes)) {
                    for (int k = 0; k < i; k++) mm_item_free(&items[k]);
                    free(results);
                    free(items);
                    free(scratch);
                    *out_n = 0;
                    return NULL;
                }
            } else {
                items[i] = input->items[i];
                items[i].colors = NULL;
                items[i].text.borrowed = 1;
            }
            char *joined = NULL;

            mm_token_t stack_toks[32];
            mm_token_t *toks = stack_toks;
            mchars_t stk_text[32];
            int ntoks = 1;
            bool heap = false;

            if (input->nth_ranges && input->nth_n > 0) {
                int ntok_in = 0, *tpl = NULL;
                char **tokens = mm_tokenize(input->lines[i], input->delim, &tpl, &ntok_in);
                int nout = 0, *opl = NULL;
                char **outs = mm_transform(tokens, ntok_in, tpl, input->nth_ranges, input->nth_n, &opl, &nout);
                ntoks = nout;
                if (ntoks > 32) { toks = malloc((size_t)ntoks * sizeof(mm_token_t)); heap = true; }
                for (int k = 0; k < nout; k++) {
                    mchars_init(&stk_text[k], outs[k]);
                    mchars_decode(&stk_text[k]);
                    toks[k].text.runes = stk_text[k].runes;
                    toks[k].text.length = stk_text[k].length;
                    toks[k].prefix_length = opl ? opl[k] : 0;
                }
                for (int k = 0; k < ntok_in; k++) free(tokens[k]);
                free(tokens); free(tpl);
                if (input->length_from_transform) {
                    size_t tot = 0;
                    for (int k = 0; k < nout; k++) tot += strlen(outs[k]);
                    joined = malloc(tot + 1);
                    if (!joined) { *out_n = 0; free(results); free(items); return NULL; }
                    joined[0] = 0;
                    for (int k = 0; k < nout; k++) strcat(joined, outs[k]);
                    mchars_free(&items[i].text);
                    mchars_init(&items[i].text, joined);
                    mchars_decode(&items[i].text);
                }
                int score = 0;
                int *pos = NULL, npos = 0;
                bool ok = mm_pattern_match_tokens(pat, toks, ntoks, &score, &pos, &npos);
                if (ok) {
                    int minb = 0xFFFF, mine = 0xFFFF, maxe = 0, valid = false;
                    if (pos && npos > 0) {
                        minb = pos[0];
                        for (int k = 0; k < npos; k++) {
                            if (pos[k] < minb) minb = pos[k];
                            if (pos[k] + 1 < mine) mine = pos[k] + 1;
                            if (pos[k] + 1 > maxe) maxe = pos[k] + 1;
                        }
                        valid = true;
                    }
                    mm_result_set_bounds(&results[n], &items[i], score, minb, mine, maxe, valid, crit);
                    for (int a = 1; a < npos; a++) {
                        int v = pos[a], b = a - 1;
                        while (b >= 0 && pos[b] > v) { pos[b + 1] = pos[b]; b--; }
                        pos[b + 1] = v;
                    }
                    results[n].pos = pos;
                    results[n].npos = npos;
                    pos = NULL;
                    n++;
                }
                free(pos);
                for (int k = 0; k < nout; k++) { mchars_free(&stk_text[k]); free(outs[k]); }
                free(outs); free(opl);
                if (heap) free(toks);
                free(joined);
                continue;
            }

            mm_text_t t = { items[i].text.runes, items[i].text.length };
            int score = 0;
            int *pos = NULL, npos = 0;
            bool ok = mm_pattern_match(pat, &t, &score, &pos, &npos);
            if (ok) {
                int minb = 0xFFFF, mine = 0xFFFF, maxe = 0, valid = false;
                if (pos && npos > 0) {
                    minb = pos[0];
                    for (int k = 0; k < npos; k++) {
                        if (pos[k] < minb) minb = pos[k];
                        if (pos[k] + 1 < mine) mine = pos[k] + 1;
                        if (pos[k] + 1 > maxe) maxe = pos[k] + 1;
                    }
                    valid = true;
                }
                mm_result_set_bounds(&results[n], &items[i], score, minb, mine, maxe, valid, crit);
                for (int a = 1; a < npos; a++) {
                    int v = pos[a], b = a - 1;
                    while (b >= 0 && pos[b] > v) { pos[b + 1] = pos[b]; b--; }
                    pos[b + 1] = v;
                }
                results[n].pos = pos;
                results[n].npos = npos;
                pos = NULL;
                n++;
            }
            free(pos);
        }

        if (pat->sortable) {
            mm_results_set_tac(tac);
            mm_results_sort(results, n);
        } else if (tac) {
            for (int i = 0, j = n - 1; i < j; i++, j--) {
                mm_result_t2 tmp = results[i]; results[i] = results[j]; results[j] = tmp;
            }
        }

        int *out = malloc((size_t)(n ? n : 1) * sizeof(int));
        if (!out) { *out_n = 0; free(results); for (int i = 0; i < total; i++) mm_item_free(&items[i]); free(items); free(scratch); return NULL; }
        for (int i = 0; i < n; i++) out[i] = results[i].index;
        if (out_pos) {
            mm_match_pos_t *mp = calloc((size_t)(n ? n : 1), sizeof(mm_match_pos_t));
            if (mp) {
                for (int i = 0; i < n; i++) {
                    mp[i].pos = results[i].pos;
                    mp[i].npos = results[i].npos;
                    results[i].pos = NULL;
                }
                *out_pos = mp;
            }
        }
        for (int i = 0; i < n; i++) free(results[i].pos);
        *out_n = n;
        free(results);
        for (int i = 0; i < total; i++) mm_item_free(&items[i]);
        free(items);
        free(scratch);
        return out;
    }

    /* Multi-threaded path - simplified, fall back to single threaded for correctness */
    /* Use single threaded for now to ensure correctness */
    nthreads = 1;
    /* repeat single threaded */
    mm_result_t2 *results = malloc((size_t)total * sizeof(mm_result_t2));
    mm_item_t *items = malloc((size_t)total * sizeof(mm_item_t));
    int n = 0;
    if (!results || !items) {
        free(results);
        free(items);
        *out_n = 0;
        return NULL;
    }
    size_t max_runes = 0;
    for (int i = 0; i < total; i++) {
        size_t n2 = (size_t)strlen(input->lines[i]) + 1;
        if (n2 > max_runes) max_runes = n2;
    }
    wchar_t *scratch = max_runes ? malloc(max_runes * sizeof(wchar_t)) : NULL;
    if (max_runes && !scratch) {
        free(results);
        free(items);
        *out_n = 0;
        return NULL;
    }
    bool own_items = input->items == NULL;
    for (int i = 0; i < total; i++) {
        if (own_items) {
            mm_item_init(&items[i], input->lines[i], i + input->base);
            if (!mchars_decode_into(&items[i].text, scratch, (int)max_runes)) {
                for (int k = 0; k < i; k++) mm_item_free(&items[k]);
                free(results);
                free(items);
                free(scratch);
                *out_n = 0;
                return NULL;
            }
        } else {
            items[i] = input->items[i];
            items[i].colors = NULL;
            items[i].text.borrowed = 1;
        }
        char *joined = NULL;

        mm_token_t stack_toks[32];
        mm_token_t *toks = stack_toks;
        mchars_t stk_text[32];
        int ntoks = 1;
        bool heap = false;

        if (input->nth_ranges && input->nth_n > 0) {
            int ntok_in = 0, *tpl = NULL;
            char **tokens = mm_tokenize(input->lines[i], input->delim, &tpl, &ntok_in);
            int nout = 0, *opl = NULL;
            char **outs = mm_transform(tokens, ntok_in, tpl, input->nth_ranges, input->nth_n, &opl, &nout);
            ntoks = nout;
            if (ntoks > 32) { toks = malloc((size_t)ntoks * sizeof(mm_token_t)); heap = true; }
            for (int k = 0; k < nout; k++) {
                mchars_init(&stk_text[k], outs[k]);
                mchars_decode(&stk_text[k]);
                toks[k].text.runes = stk_text[k].runes;
                toks[k].text.length = stk_text[k].length;
                toks[k].prefix_length = opl ? opl[k] : 0;
            }
            for (int k = 0; k < ntok_in; k++) free(tokens[k]);
            free(tokens); free(tpl);
            if (input->length_from_transform) {
                size_t tot = 0;
                for (int k = 0; k < nout; k++) tot += strlen(outs[k]);
                joined = malloc(tot + 1);
                if (!joined) { *out_n = 0; free(results); free(items); return NULL; }
                joined[0] = 0;
                for (int k = 0; k < nout; k++) strcat(joined, outs[k]);
                mchars_free(&items[i].text);
                mchars_init(&items[i].text, joined);
                mchars_decode(&items[i].text);
            }
            int score = 0;
            int *pos = NULL, npos = 0;
            bool ok = mm_pattern_match_tokens(pat, toks, ntoks, &score, &pos, &npos);
            if (ok) {
                int minb = 0xFFFF, mine = 0xFFFF, maxe = 0, valid = false;
                if (pos && npos > 0) {
                    minb = pos[0];
                    for (int k = 0; k < npos; k++) {
                        if (pos[k] < minb) minb = pos[k];
                        if (pos[k] + 1 < mine) mine = pos[k] + 1;
                        if (pos[k] + 1 > maxe) maxe = pos[k] + 1;
                    }
                    valid = true;
                }
                mm_result_set_bounds(&results[n], &items[i], score, minb, mine, maxe, valid, crit);
                for (int a = 1; a < npos; a++) {
                    int v = pos[a], b = a - 1;
                    while (b >= 0 && pos[b] > v) { pos[b + 1] = pos[b]; b--; }
                    pos[b + 1] = v;
                }
                results[n].pos = pos;
                results[n].npos = npos;
                pos = NULL;
                n++;
            }
            free(pos);
            for (int k = 0; k < nout; k++) { mchars_free(&stk_text[k]); free(outs[k]); }
            free(outs); free(opl);
            if (heap) free(toks);
            free(joined);
            continue;
        }

        mm_text_t t = { items[i].text.runes, items[i].text.length };
        int score = 0;
        int *pos = NULL, npos = 0;
        bool ok = mm_pattern_match(pat, &t, &score, &pos, &npos);
        if (ok) {
            int minb = 0xFFFF, mine = 0xFFFF, maxe = 0, valid = false;
            if (pos && npos > 0) {
                minb = pos[0];
                for (int k = 0; k < npos; k++) {
                    if (pos[k] < minb) minb = pos[k];
                    if (pos[k] + 1 < mine) mine = pos[k] + 1;
                    if (pos[k] + 1 > maxe) maxe = pos[k] + 1;
                }
                valid = true;
            }
            mm_result_set_bounds(&results[n], &items[i], score, minb, mine, maxe, valid, crit);
            for (int a = 1; a < npos; a++) {
                int v = pos[a], b = a - 1;
                while (b >= 0 && pos[b] > v) { pos[b + 1] = pos[b]; b--; }
                pos[b + 1] = v;
            }
            results[n].pos = pos;
            results[n].npos = npos;
            pos = NULL;
            n++;
        }
        free(pos);
    }

    if (pat->sortable) {
        mm_results_set_tac(tac);
        mm_results_sort(results, n);
    } else if (tac) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            mm_result_t2 tmp = results[i]; results[i] = results[j]; results[j] = tmp;
        }
    }

    int *out = malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!out) { *out_n = 0; free(results); for (int i = 0; i < total; i++) mm_item_free(&items[i]); free(items); free(scratch); return NULL; }
    for (int i = 0; i < n; i++) out[i] = results[i].index;
    if (out_pos) {
        mm_match_pos_t *mp = calloc((size_t)(n ? n : 1), sizeof(mm_match_pos_t));
        if (mp) {
            for (int i = 0; i < n; i++) {
                mp[i].pos = results[i].pos;
                mp[i].npos = results[i].npos;
                results[i].pos = NULL;
            }
            *out_pos = mp;
        }
    }
    for (int i = 0; i < n; i++) free(results[i].pos);
    *out_n = n;
    free(results);
    for (int i = 0; i < total; i++) mm_item_free(&items[i]);
    free(items);
    free(scratch);
    return out;
}

void mm_match_pos_free(mm_match_pos_t *pos, int n) {
    if (!pos) return;
    for (int i = 0; i < n; i++) free(pos[i].pos);
    free(pos);
}
