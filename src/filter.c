/* filter.c - see filter.h */
#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <limits.h>

typedef struct {
    mm_input_t input;
    mm_pattern_t *pat;
    const mm_sort_criteria_t *crit;
    int *indices;
    int count;
    mm_match_pos_t *positions;
    bool want_positions;
    mm_result_t2 *ranked_results;
    mm_item_t *ranked_items;
    bool ranked_ok;
} mm_filter_worker_t;

static int *mm_batch_search_internal(mm_input_t *, mm_pattern_t *,
    const mm_sort_criteria_t *, bool, int *, mm_match_pos_t **,
    mm_result_t2 **, mm_item_t **, int *, bool *);

static void *mm_filter_worker_run(void *arg) {
    mm_filter_worker_t *w = arg;
    w->indices = mm_batch_search(&w->input, w->pat, w->crit, false,
                                 &w->count,
                                 w->want_positions ? &w->positions : NULL);
    return NULL;
}

static void *mm_filter_ranked_worker_run(void *arg) {
    mm_filter_worker_t *w = arg;
    int ignored = 0;
    w->ranked_ok = false;
    (void)mm_batch_search_internal(&w->input, w->pat, w->crit, false,
        &ignored, NULL, &w->ranked_results, &w->ranked_items,
        &w->count, &w->ranked_ok);
    return NULL;
}

static int *mm_batch_search_internal(mm_input_t *input, mm_pattern_t *pat,
                     const mm_sort_criteria_t *crit, bool tac, int *out_n,
                     mm_match_pos_t **out_pos, mm_result_t2 **raw_results,
                     mm_item_t **raw_items, int *raw_count, bool *raw_ok) {
    if (raw_ok) *raw_ok = false;
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
            for (int i = 0; i < total; i++) {
                int j = total - 1 - i;
                out[i] = input->items ? input->items[j].index : j + input->base;
            }
        } else {
            for (int i = 0; i < total; i++)
                out[i] = input->items ? input->items[i].index : i + input->base;
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

    /* Extended queries are not score-sorted. Process disjoint input chunks
       concurrently, then concatenate them in original order. Each child stays
       below the threshold and uses the ordinary single-threaded implementation. */
    if (!pat->sortable && total >= 10000) {
        int nthreads = 4;
        const char *te = getenv("FZF_NUM_THREADS");
        if (te && *te) nthreads = atoi(te);
        if (nthreads < 1) nthreads = 1;
        if (nthreads > 16) nthreads = 16;
        if (nthreads > total / 2) nthreads = total / 2;
        if (nthreads > 1) {
            mm_filter_worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
            pthread_t *threads = calloc((size_t)nthreads, sizeof(*threads));
            bool *started = calloc((size_t)nthreads, sizeof(*started));
            if (!workers || !threads || !started) {
                free(workers); free(threads); free(started);
                *out_n = 0;
                if (out_pos) *out_pos = NULL;
                return NULL;
            }
            int offset = 0;
            for (int k = 0; k < nthreads; k++) {
                int count = total / nthreads + (k < total % nthreads);
                workers[k].input = *input;
                workers[k].input.lines = input->lines ? input->lines + offset : NULL;
                workers[k].input.items = input->items ? input->items + offset : NULL;
                workers[k].input.count = count;
                workers[k].input.base = input->base + offset;
                workers[k].pat = pat;
                workers[k].crit = crit;
                workers[k].want_positions = out_pos != NULL;
                if (pthread_create(&threads[k], NULL, mm_filter_worker_run, &workers[k]) == 0)
                    started[k] = true;
                else
                    mm_filter_worker_run(&workers[k]);
                offset += count;
            }
            bool failed = false;
            for (int k = 0; k < nthreads; k++) {
                if (started[k]) pthread_join(threads[k], NULL);
                if (!workers[k].indices ||
                    (out_pos && !workers[k].positions && workers[k].count > 0))
                    failed = true;
            }
            if (!failed) {
                int n = 0;
                for (int k = 0; k < nthreads; k++) n += workers[k].count;
                int *out = malloc((size_t)(n ? n : 1) * sizeof(*out));
                mm_match_pos_t *mp = out_pos
                    ? calloc((size_t)(n ? n : 1), sizeof(*mp)) : NULL;
                if (out && (!out_pos || mp)) {
                    int at = 0;
                    for (int k = 0; k < nthreads; k++) {
                        for (int j = 0; j < workers[k].count; j++, at++) {
                            out[at] = workers[k].indices[j];
                            if (mp) {
                                mp[at] = workers[k].positions[j];
                                workers[k].positions[j].pos = NULL;
                            }
                        }
                    }
                    if (tac) {
                        for (int i = 0, j = n - 1; i < j; i++, j--) {
                            int tmp = out[i]; out[i] = out[j]; out[j] = tmp;
                            if (mp) {
                                mm_match_pos_t ptmp = mp[i]; mp[i] = mp[j]; mp[j] = ptmp;
                            }
                        }
                    }
                    for (int k = 0; k < nthreads; k++) {
                        free(workers[k].indices);
                        mm_match_pos_free(workers[k].positions, workers[k].count);
                    }
                    free(workers); free(threads); free(started);
                    *out_n = n;
                    if (out_pos) *out_pos = mp;
                    return out;
                }
                free(out);
                free(mp);
            }
            for (int k = 0; k < nthreads; k++) {
                free(workers[k].indices);
                mm_match_pos_free(workers[k].positions, workers[k].count);
            }
            free(workers); free(threads); free(started);
            *out_n = 0;
            if (out_pos) *out_pos = NULL;
            return NULL;
        }
    }

    /* Try to use threading if beneficial */
    int nthreads = 4;
    const char *te = getenv("FZF_NUM_THREADS");
    if (te && *te) nthreads = atoi(te);
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    if (total < 10000) nthreads = 1;

    /* Workers retain complete result records; sort once globally after joining. */
    if (pat->sortable && total >= 10000 && nthreads > 1 && !raw_results) {
        mm_filter_worker_t *w = calloc((size_t)nthreads, sizeof(*w));
        pthread_t *th = calloc((size_t)nthreads, sizeof(*th));
        bool *started = calloc((size_t)nthreads, sizeof(*started));
        if (!w || !th || !started) {
            free(w); free(th); free(started);
            *out_n = 0; if (out_pos) *out_pos = NULL; return NULL;
        }
        int offset = 0;
        for (int k = 0; k < nthreads; k++) {
            int count = total / nthreads + (k < total % nthreads);
            w[k].input = *input;
            w[k].input.lines = input->lines ? input->lines + offset : NULL;
            w[k].input.items = input->items ? input->items + offset : NULL;
            w[k].input.count = count;
            w[k].input.base = input->base + offset;
            w[k].pat = pat; w[k].crit = crit;
            if (pthread_create(&th[k], NULL, mm_filter_ranked_worker_run, &w[k]) == 0)
                started[k] = true;
            else mm_filter_ranked_worker_run(&w[k]);
            offset += count;
        }
        bool failed = false;
        int n = 0;
        for (int k = 0; k < nthreads; k++) {
            if (started[k]) pthread_join(th[k], NULL);
            if (!w[k].ranked_ok || !w[k].ranked_results || !w[k].ranked_items)
                failed = true;
            if (w[k].count > 0 && n <= INT_MAX - w[k].count) n += w[k].count;
            else if (w[k].count > 0) failed = true;
        }
        mm_result_t2 *all = failed ? NULL : malloc((size_t)(n ? n : 1) * sizeof(*all));
        if (!all) failed = true;
        if (!failed) {
            int at = 0;
            for (int k = 0; k < nthreads; k++) {
                memcpy(all + at, w[k].ranked_results, (size_t)w[k].count * sizeof(*all));
                at += w[k].count;
                free(w[k].ranked_results); w[k].ranked_results = NULL;
            }
            mm_results_set_tac(tac);
            mm_results_sort(all, n);
            int *out = malloc((size_t)(n ? n : 1) * sizeof(*out));
            mm_match_pos_t *mp = out_pos ? calloc((size_t)(n ? n : 1), sizeof(*mp)) : NULL;
            if (out && (!out_pos || mp)) {
                for (int i = 0; i < n; i++) {
                    out[i] = all[i].index;
                    if (mp) { mp[i].pos = all[i].pos; mp[i].npos = all[i].npos; all[i].pos = NULL; }
                }
                for (int i = 0; i < n; i++) free(all[i].pos);
                free(all);
                for (int k = 0; k < nthreads; k++) {
                    for (int i = 0; i < w[k].input.count; i++) mm_item_free(&w[k].ranked_items[i]);
                    free(w[k].ranked_items);
                }
                free(w); free(th); free(started);
                *out_n = n; if (out_pos) *out_pos = mp; return out;
            }
            free(out); free(mp); failed = true;
        }
        free(all);
        for (int k = 0; k < nthreads; k++) {
            free(w[k].ranked_results);
            if (w[k].ranked_items) {
                for (int i = 0; i < w[k].input.count; i++) mm_item_free(&w[k].ranked_items[i]);
                free(w[k].ranked_items);
            }
        }
        free(w); free(th); free(started);
        *out_n = 0; if (out_pos) *out_pos = NULL; return NULL;
    }

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
        bool own_items = input->items == NULL;
        /* UI items are already decoded; avoid scanning every line to size an
           unused decode buffer on every search refresh. */
        size_t max_runes = 0;
        if (own_items) {
            for (int i = 0; i < total; i++) {
                size_t n2 = (size_t)strlen(input->lines[i]) + 1;
                if (n2 > max_runes) max_runes = n2;
            }
        }
        wchar_t *scratch = own_items && max_runes
            ? malloc(max_runes * sizeof(wchar_t)) : NULL;
        if (own_items && max_runes && !scratch) {
            free(results);
            free(items);
            *out_n = 0;
            return NULL;
        }
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

        if (raw_results) {
            *raw_results = results; *raw_items = items; *raw_count = n;
            if (raw_ok) *raw_ok = true;
            free(scratch); *out_n = n; return NULL;
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
    bool own_items = input->items == NULL;
    /* UI items are already decoded; avoid scanning every line to size an
       unused decode buffer on every search refresh. */
    size_t max_runes = 0;
    if (own_items) {
        for (int i = 0; i < total; i++) {
            size_t n2 = (size_t)strlen(input->lines[i]) + 1;
            if (n2 > max_runes) max_runes = n2;
        }
    }
    wchar_t *scratch = own_items && max_runes
        ? malloc(max_runes * sizeof(wchar_t)) : NULL;
    if (own_items && max_runes && !scratch) {
        free(results);
        free(items);
        *out_n = 0;
        return NULL;
    }
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

int *mm_batch_search(mm_input_t *input, mm_pattern_t *pat,
                     const mm_sort_criteria_t *crit, bool tac, int *out_n,
                     mm_match_pos_t **out_pos) {
    return mm_batch_search_internal(input, pat, crit, tac, out_n, out_pos,
                                    NULL, NULL, NULL, NULL);
}

void mm_match_pos_free(mm_match_pos_t *pos, int n) {
    if (!pos) return;
    for (int i = 0; i < n; i++) free(pos[i].pos);
    free(pos);
}
