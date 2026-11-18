/* filter.c - see filter.h */
#include "filter.h"
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <limits.h>
#include <stdint.h>
#include <unistd.h>
#include <wchar.h>

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

static int requested_threads;

static int mm_int_compare(const void *a, const void *b) {
    int x = *(const int *)a;
    int y = *(const int *)b;
    return (x > y) - (x < y);
}

void mm_filter_set_threads(int threads) {
    requested_threads = threads > 0 ? threads : 0;
}

static int mm_filter_thread_count(int total) {
    int nthreads = requested_threads;
    if (nthreads <= 0) {
        const char *te = getenv("FZF_NUM_THREADS");
        nthreads = te && *te ? atoi(te) : 4;
    }
    if (nthreads <= 0) nthreads = 1;
    if (nthreads > 16) nthreads = 16;
    if (total < 10000) nthreads = 1;
    return nthreads;
}

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

/* --filter only needs the order of matching line indexes. A full mm_item_t
   and mm_result_t2 per input line, plus a heap position array per match,
   kept the resident set above fzf. The rank is the same 64-bit key
   mm_results_sort builds from points[3..0]. */
typedef struct {
    uint64_t key;
    int32_t index;
} mm_rank_t;

typedef struct {
    mm_input_t input;
    mm_pattern_t *pat;
    const mm_sort_criteria_t *crit;
    bool tac;
    mm_rank_t *ranks;
    int count;
    bool ok;
} mm_compact_worker_t;

static bool rank_before(const mm_rank_t *a, const mm_rank_t *b, bool tac) {
    if (a->key != b->key) return a->key < b->key;
    if (a->index == b->index) return false;
    if (tac) return a->index > b->index;
    return a->index < b->index;
}

/* Stable LSD radix, matching mm_results_sort. --tac reverses first so equal
   keys keep the later input line. */
static bool rank_sort(mm_rank_t *a, int n, bool tac) {
    if (n < 2) return true;
    mm_rank_t *tmp = malloc((size_t)n * sizeof(*tmp));
    if (!tmp) return false;
    if (tac) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            mm_rank_t t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    mm_rank_t *src = a, *dst = tmp;
    for (int shift = 0; shift < 64; shift += 8) {
        size_t count[256];
        memset(count, 0, sizeof(count));
        for (int i = 0; i < n; i++) count[(src[i].key >> shift) & 0xFF]++;
        size_t sum = 0;
        for (int b = 0; b < 256; b++) {
            size_t c = count[b];
            count[b] = sum;
            sum += c;
        }
        for (int i = 0; i < n; i++)
            dst[count[(src[i].key >> shift) & 0xFF]++] = src[i];
        mm_rank_t *swap = src;
        src = dst;
        dst = swap;
    }
    if (src != a) memcpy(a, src, (size_t)n * sizeof(*a));
    free(tmp);
    return true;
}

static bool mm_compact_rank(mm_pattern_t *pat, const mm_sort_criteria_t *crit,
                            wchar_t *runes, int nrunes, int index, mm_rank_t *out) {
    mm_text_t text = { runes, nrunes, NULL };
    int score = 0, begin = -1, end = -1;
    if (!mm_pattern_match_span(pat, &text, &score, &begin, &end)) return false;
    out->index = index;
    out->key = 0;
    if (!pat->sortable) return true;

    mm_item_t view;
    memset(&view, 0, sizeof(view));
    view.index = index;
    view.text.runes = runes;
    view.text.length = nrunes;
    view.text.decoded = 1;
    view.text.borrowed = 1;
    int minb = 0xFFFF, mine = 0xFFFF, maxe = 0;
    bool valid = false;
    if (begin >= 0 && end > begin) {
        minb = begin;
        mine = begin + 1;
        maxe = end;
        valid = true;
    }
    mm_result_t2 result;
    memset(&result, 0, sizeof(result));
    mm_result_set_bounds(&result, &view, score, minb, mine, maxe, valid, crit);
    const uint16_t *p = result.points;
    out->key = ((uint64_t)p[3] << 48) | ((uint64_t)p[2] << 32) |
               ((uint64_t)p[1] << 16) | (uint64_t)p[0];
    return true;
}

static void *mm_compact_worker_run(void *arg) {
    mm_compact_worker_t *w = arg;
    w->ok = false;
    w->count = 0;
    w->ranks = NULL;
    int total = w->input.count;
    if (total < 0) return NULL;
    w->ranks = malloc((size_t)(total ? total : 1) * sizeof(mm_rank_t));
    if (!w->ranks) return NULL;

    size_t max_runes = 1;
    for (int i = 0; i < total; i++) {
        size_t n2 = w->input.line_lens
            ? w->input.line_lens[i] + 1
            : (size_t)strlen(w->input.lines[i]) + 1;
        if (n2 > max_runes) max_runes = n2;
    }
    if (max_runes > (size_t)INT_MAX) {
        free(w->ranks);
        w->ranks = NULL;
        return NULL;
    }
    wchar_t *scratch = malloc(max_runes * sizeof(wchar_t));
    if (!scratch) {
        free(w->ranks);
        w->ranks = NULL;
        return NULL;
    }

    int n = 0;
    for (int i = 0; i < total; i++) {
        mchars_t mc;
        mchars_init(&mc, w->input.lines[i]);
        if (!mchars_decode_into(&mc, scratch, (int)max_runes)) {
            free(scratch);
            free(w->ranks);
            w->ranks = NULL;
            return NULL;
        }
        mm_rank_t rank;
        if (!mm_compact_rank(w->pat, w->crit, mc.runes, mc.length,
                             i + w->input.base, &rank))
            continue;
        w->ranks[n++] = rank;
    }
    free(scratch);
    w->count = n;
    if (w->pat->sortable && !rank_sort(w->ranks, n, w->tac)) {
        free(w->ranks);
        w->ranks = NULL;
        return NULL;
    }
    w->ok = true;
    return NULL;
}

static int *mm_batch_search_compact(mm_input_t *input, mm_pattern_t *pat,
                                    const mm_sort_criteria_t *crit, bool tac,
                                    int *out_n) {
    int total = input->count;
    int nthreads = mm_filter_thread_count(total);
    if (nthreads > total) nthreads = total;
    if (nthreads < 1) nthreads = 1;

    mm_compact_worker_t *workers = calloc((size_t)nthreads, sizeof(*workers));
    pthread_t *threads = nthreads > 1 ? calloc((size_t)nthreads, sizeof(*threads)) : NULL;
    bool *started = nthreads > 1 ? calloc((size_t)nthreads, sizeof(*started)) : NULL;
    if (!workers || (nthreads > 1 && (!threads || !started))) {
        free(workers);
        free(threads);
        free(started);
        *out_n = 0;
        return NULL;
    }

    int offset = 0;
    for (int k = 0; k < nthreads; k++) {
        int count = total / nthreads + (k < total % nthreads);
        workers[k].input = *input;
        workers[k].input.lines = input->lines ? input->lines + offset : NULL;
        workers[k].input.line_lens = input->line_lens ? input->line_lens + offset : NULL;
        workers[k].input.items = NULL;
        workers[k].input.count = count;
        workers[k].input.base = input->base + offset;
        workers[k].pat = pat;
        workers[k].crit = crit;
        workers[k].tac = tac;
        if (nthreads == 1) {
            mm_compact_worker_run(&workers[k]);
        } else if (pthread_create(&threads[k], NULL, mm_compact_worker_run, &workers[k]) == 0) {
            started[k] = true;
        } else {
            mm_compact_worker_run(&workers[k]);
        }
        offset += count;
    }

    bool failed = false;
    int n = 0;
    for (int k = 0; k < nthreads; k++) {
        if (started && started[k]) pthread_join(threads[k], NULL);
        if (!workers[k].ok || !workers[k].ranks) failed = true;
        if (!failed && workers[k].count > 0) {
            if (n > INT_MAX - workers[k].count) failed = true;
            else n += workers[k].count;
        }
    }
    int *out = failed ? NULL : malloc((size_t)(n ? n : 1) * sizeof(int));
    if (!out) failed = true;
    if (!failed && !pat->sortable) {
        int at = 0;
        for (int k = 0; k < nthreads; k++) {
            for (int j = 0; j < workers[k].count; j++)
                out[at++] = workers[k].ranks[j].index;
        }
        if (tac) {
            for (int i = 0, j = n - 1; i < j; i++, j--) {
                int tmp = out[i];
                out[i] = out[j];
                out[j] = tmp;
            }
        }
    } else if (!failed && nthreads == 1) {
        for (int i = 0; i < n; i++) out[i] = workers[0].ranks[i].index;
    } else if (!failed) {
        int at[16];
        for (int k = 0; k < nthreads; k++) at[k] = 0;
        for (int i = 0; i < n; i++) {
            int best = -1;
            for (int k = 0; k < nthreads; k++) {
                if (at[k] >= workers[k].count) continue;
                if (best < 0 || rank_before(&workers[k].ranks[at[k]],
                                            &workers[best].ranks[at[best]], tac))
                    best = k;
            }
            out[i] = workers[best].ranks[at[best]].index;
            at[best]++;
        }
    }

    for (int k = 0; k < nthreads; k++) free(workers[k].ranks);
    free(workers);
    free(threads);
    free(started);
    if (failed) {
        free(out);
        *out_n = 0;
        return NULL;
    }
    *out_n = n;
    return out;
}

static int *mm_batch_search_internal(mm_input_t *input, mm_pattern_t *pat,
                     const mm_sort_criteria_t *crit, bool tac, int *out_n,
                     mm_match_pos_t **out_pos, mm_result_t2 **raw_results,
                     mm_item_t **raw_items, int *raw_count, bool *raw_ok) {
    if (out_n) *out_n = 0;
    if (out_pos) *out_pos = NULL;
    if (raw_results) *raw_results = NULL;
    if (raw_items) *raw_items = NULL;
    if (raw_count) *raw_count = 0;
    if (raw_ok) *raw_ok = false;
    if (!input || !pat || !crit || !out_n || input->count < 0 ||
        (input->count > 0 && !input->lines && !input->items) ||
        (input->nth_ranges && input->nth_n > 0 && !input->lines))
        return NULL;
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
        /* An empty query has no highlighted characters. Skip the zeroed
           position array; callers treat a NULL list as "no highlights". */
        if (out_pos) *out_pos = NULL;
        *out_n = total;
        return out;
    }

    /* Positions and prebuilt items stay on the general path. --filter uses
       neither, so it only keeps a sort key and an index per match. */
    if (!out_pos && !raw_results && input->items == NULL &&
        !(input->nth_ranges && input->nth_n > 0) &&
        !(input->with_nth_ranges && input->with_nth_n > 0)) {
        return mm_batch_search_compact(input, pat, crit, tac, out_n);
    }

    /* Extended queries are not score-sorted. Process disjoint input chunks
       concurrently, then concatenate them in original order. Each child stays
       below the threshold and uses the ordinary single-threaded implementation. */
    if (!pat->sortable && total >= 10000) {
        int nthreads = mm_filter_thread_count(total);
        if (input->with_nth_ranges && input->with_nth_n > 0) nthreads = 1;
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
    int nthreads = mm_filter_thread_count(total);

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
            free(out); free(mp);
        }
        if (all) {
            for (int i = 0; i < n; i++) free(all[i].pos);
            free(all);
        }
        for (int k = 0; k < nthreads; k++) {
            if (w[k].ranked_results) {
                for (int i = 0; i < w[k].count; i++) free(w[k].ranked_results[i].pos);
                free(w[k].ranked_results);
            }
            if (w[k].ranked_items) {
                for (int i = 0; i < w[k].input.count; i++) mm_item_free(&w[k].ranked_items[i]);
                free(w[k].ranked_items);
            }
        }
        free(w); free(th); free(started);
        *out_n = 0; if (out_pos) *out_pos = NULL; return NULL;
    }

    /* Single threaded path for simplicity and correctness */
    if (nthreads == 1 || raw_results) {
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
        /* Owned lines are decoded into the scratch buffer below. UI items are
           decoded on first use so a streaming directory listing does not build
           a rune array for every path before the user types. */
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
        int items_initialized = 0;
        bool search_failed = false;
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
                if (!input->items[i].text.decoded &&
                    !mchars_decode(&input->items[i].text)) {
                    search_failed = true;
                    break;
                }
                items[i] = input->items[i];
                items[i].colors = NULL;
                items[i].text.borrowed = 1;
            }
            items_initialized = i + 1;
            char *joined = NULL;
            const char *search_line = input->lines[i];
            if (input->with_nth_ranges && input->with_nth_n > 0) {
                joined = mm_transform_line(input->lines[i], input->delim,
                                           input->with_nth_ranges,
                                           input->with_nth_n);
                if (!joined) {
                    search_failed = true;
                    break;
                }
                search_line = joined;
            }

            mm_token_t stack_toks[32];
            mm_token_t *toks = stack_toks;
            mchars_t stack_text[32];
            mchars_t *stk_text = stack_text;
            int ntoks = 1;
            bool heap = false;

            if (input->nth_ranges && input->nth_n > 0) {
                int ntok_in = 0, *tpl = NULL;
                char **tokens = mm_tokenize(search_line, input->delim, &tpl, &ntok_in);
                if (!tokens || !tpl) {
                    for (int k = 0; k < ntok_in; k++) free(tokens ? tokens[k] : NULL);
                    free(tokens);
                    free(tpl);
                    free(joined);
                    search_failed = true;
                    break;
                }

                int nout = 0, *opl = NULL;
                char **outs = mm_transform(tokens, ntok_in, tpl, input->nth_ranges,
                                            input->nth_n, &opl, &nout);
                for (int k = 0; k < ntok_in; k++) free(tokens[k]);
                free(tokens);
                free(tpl);
                if (!outs || !opl) {
                    if (outs) {
                        for (int k = 0; k < nout; k++) free(outs[k]);
                    }
                    free(outs);
                    free(opl);
                    free(joined);
                    search_failed = true;
                    break;
                }

                if (input->delim.str || input->delim.is_regex) {
                    char *stripped = mm_strip_last_delimiter(outs[nout - 1],
                                                             input->delim);
                    if (!stripped) {
                        if (outs) {
                            for (int k = 0; k < nout; k++) free(outs[k]);
                        }
                        free(outs);
                        free(opl);
                        free(joined);
                        search_failed = true;
                        break;
                    }
                    free(outs[nout - 1]);
                    outs[nout - 1] = stripped;
                }

                ntoks = nout;
                if (ntoks > 32) {
                    toks = malloc((size_t)ntoks * sizeof(*toks));
                    stk_text = calloc((size_t)ntoks, sizeof(*stk_text));
                    heap = true;
                    if (!toks || !stk_text) {
                        free(toks);
                        free(stk_text);
                        for (int k = 0; k < nout; k++) free(outs[k]);
                        free(outs);
                        free(opl);
                        free(joined);
                        search_failed = true;
                        break;
                    }
                }

                int decoded_count = 0;
                bool decoded_ok = true;
                for (int k = 0; k < nout; k++) {
                    mchars_init(&stk_text[k], outs[k]);
                    decoded_count++;
                    if (!mchars_decode(&stk_text[k])) {
                        decoded_ok = false;
                        break;
                    }
                    toks[k].text.runes = stk_text[k].runes;
                    toks[k].text.length = stk_text[k].length;
                    toks[k].prefix_length = opl[k];
                }
                if (!decoded_ok) {
                    for (int k = 0; k < decoded_count; k++) mchars_free(&stk_text[k]);
                    for (int k = 0; k < nout; k++) free(outs[k]);
                    free(outs);
                    free(opl);
                    if (heap) {
                        free(toks);
                        free(stk_text);
                    }
                    free(joined);
                    search_failed = true;
                    break;
                }

                int score = 0;
                int *pos = NULL, npos = 0;
                bool ok = mm_pattern_match_tokens(pat, toks, ntoks,
                                                  &score, &pos, &npos);
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
                    mm_result_set_bounds(&results[n], &items[i], score,
                                         minb, mine, maxe, valid, crit);
                    if (npos > 1) qsort(pos, (size_t)npos, sizeof(*pos), mm_int_compare);
                    results[n].pos = pos;
                    results[n].npos = npos;
                    pos = NULL;
                    n++;
                }
                free(pos);
                for (int k = 0; k < nout; k++) {
                    mchars_free(&stk_text[k]);
                    free(outs[k]);
                }
                free(outs);
                free(opl);
                if (heap) {
                    free(toks);
                    free(stk_text);
                }
                free(joined);
                continue;
            }

            if (input->with_nth_ranges && input->with_nth_n > 0) {
                mchars_t transformed_text = {0};
                if (!mchars_init(&transformed_text, joined) ||
                    !mchars_decode(&transformed_text)) {
                    mchars_free(&transformed_text);
                    free(joined);
                    search_failed = true;
                    break;
                }
                mm_text_t transformed = {
                    transformed_text.runes, transformed_text.length, NULL
                };
                int score = 0;
                int *pos = NULL, npos = 0;
                bool ok = mm_pattern_match(pat, &transformed, &score, &pos, &npos);
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
                    mm_item_t transformed_item = items[i];
                    transformed_item.text = transformed_text;
                    transformed_item.trim_known = false;
                    transformed_item.trim_length = 0;
                    mm_result_set_bounds(&results[n], &transformed_item, score,
                                         minb, mine, maxe, valid, crit);
                    results[n].item = &items[i];
                    if (npos > 1) qsort(pos, (size_t)npos, sizeof(*pos), mm_int_compare);
                    results[n].pos = pos;
                    results[n].npos = npos;
                    pos = NULL;
                    n++;
                }
                free(pos);
                mchars_free(&transformed_text);
                free(joined);
                continue;
            }

            mm_text_t t = { items[i].text.runes, items[i].text.length, NULL };
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
                if (npos > 1) qsort(pos, (size_t)npos, sizeof(*pos), mm_int_compare);
                results[n].pos = pos;
                results[n].npos = npos;
                pos = NULL;
                n++;
            }
            free(pos);
        }

        if (search_failed) {
            for (int i = 0; i < n; i++) free(results[i].pos);
            free(results);
            for (int i = 0; i < items_initialized; i++) mm_item_free(&items[i]);
            free(items);
            free(scratch);
            *out_n = 0;
            if (out_pos) *out_pos = NULL;
            return NULL;
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
            if (!mp) {
                free(out);
                *out_n = 0;
                for (int i = 0; i < n; i++) free(results[i].pos);
                free(results);
                for (int i = 0; i < total; i++) mm_item_free(&items[i]);
                free(items);
                free(scratch);
                return NULL;
            }
            for (int i = 0; i < n; i++) {
                mp[i].pos = results[i].pos;
                mp[i].npos = results[i].npos;
                results[i].pos = NULL;
            }
            *out_pos = mp;
        }
        for (int i = 0; i < n; i++) free(results[i].pos);
        *out_n = n;
        free(results);
        for (int i = 0; i < total; i++) mm_item_free(&items[i]);
        free(items);
        free(scratch);
        return out;
    }

    *out_n = 0;
    if (out_pos) *out_pos = NULL;
    return NULL;
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

/* One widening buffer per thread. Interactive workers each match a
   disjoint slice, so they must not share the scratch. */
static _Thread_local wchar_t *g_match_runes;
static _Thread_local int      g_match_runes_cap;

mm_text_t mm_item_match_text(const mm_item_t *it) {
    mm_text_t none = { NULL, 0, NULL };
    if (!it) return none;
    const mchars_t *c = &it->text;
    if (c->decoded && c->runes)
        return (mm_text_t){ c->runes, c->length, NULL };
    if (!c->bytes || c->length <= 0) return none;
    int need = c->length + 1;
    if (need > g_match_runes_cap) {
        int cap = g_match_runes_cap > 0 ? g_match_runes_cap : 128;
        while (cap < need) {
            if (cap > INT_MAX / 2) { cap = need; break; }
            cap *= 2;
        }
        wchar_t *grown = malloc((size_t)cap * sizeof(wchar_t));
        if (!grown) return (mm_text_t){ NULL, -1, NULL };
        free(g_match_runes);
        g_match_runes = grown;
        g_match_runes_cap = cap;
    }
    if (c->ascii) {
        const unsigned char *s = (const unsigned char *)c->bytes;
        for (int i = 0; i < c->length; i++) g_match_runes[i] = (wchar_t)s[i];
    } else {
        const char *p = c->bytes;
        size_t rem = (size_t)c->raw_bytes;
        int i = 0;
        while (rem > 0 && i < c->length) {
            wchar_t cp;
            size_t used = mm_utf8_decode(p, rem, &cp);
            if (used == 0) used = 1;
            g_match_runes[i++] = cp;
            p += used;
            rem -= used;
        }
        while (i < c->length) g_match_runes[i++] = 0;
    }
    g_match_runes[c->length] = 0;
    return (mm_text_t){ g_match_runes, c->length, NULL };
}

/* Sort key plus the item index. The interactive search never needs the
   64-byte result record: highlights are computed later, for visible rows. */
typedef struct {
    uint64_t key;
    int32_t index;
} ix_hit_t;

typedef struct {
    mm_item_t *items;
    const int *cands;
    int ncand;
    int range_lo;
    int begin;
    int end;
    const bool *excluded;
    int item_count;
    int header_lines;
    mm_pattern_t *pat;
    const mm_sort_criteria_t *crit;
    ix_hit_t *hits;
    const wchar_t *dpat;
    int dlen;
    bool direct;
    bool dcs;
    bool dnorm;
    bool forward;
    bool sortable;
    bool byte_pat;          /* direct pattern is ASCII; use the byte matcher */
    unsigned char b1;       /* pattern byte when dlen == 1 */
    int16_t best_bonus;     /* hoisted first-character bonus for b1 */
    int n;
    bool ok;
} mm_ix_worker_t;

static ix_hit_t *g_ix_slab;
static size_t g_ix_slab_cap;
static ix_hit_t *g_ix_tmp;
static size_t g_ix_tmp_cap;
static int g_ix_touch_n;
static uint32_t g_ix_hist[65536];

static ix_hit_t *ix_slab_get(int n) {
    if (n < 0) return NULL;
    if ((size_t)n <= g_ix_slab_cap) return g_ix_slab;
    size_t cap = g_ix_slab_cap ? g_ix_slab_cap : 1024;
    while (cap < (size_t)n) {
        if (cap > SIZE_MAX / 2) { cap = (size_t)n; break; }
        cap *= 2;
    }
    if (cap > SIZE_MAX / sizeof(ix_hit_t)) return NULL;
    ix_hit_t *p = malloc(cap * sizeof(*p));
    if (!p) return NULL;
    free(g_ix_slab);
    g_ix_slab = p;
    g_ix_slab_cap = cap;
    return g_ix_slab;
}

/* Length tiebreak without writing the item. Paths have no edge whitespace,
   and storing trim_known would dirty every matched record on the first key. */
static uint16_t ix_trim(mm_item_t *it) {
    if (it->trim_known) return it->trim_length;
    if (it->text.ascii && it->text.bytes) {
        const unsigned char *s = (const unsigned char *)it->text.bytes;
        int len = it->text.length;
        if (len > 0 && len <= 0xFFFF && s[0] > ' ' && s[len - 1] > ' ')
            return (uint16_t)len;
    }
    return mm_item_trim_length(it);
}

static uint64_t ix_key(mm_item_t *it, int score, int b, int e, bool valid,
                       const mm_sort_criteria_t *crit) {
    if (crit->count == 2 && crit->crit[0] == BY_SCORE && crit->crit[1] == BY_LENGTH) {
        uint16_t sk = (uint16_t)(0xFFFFu - (uint16_t)score);
        uint16_t trim = ix_trim(it);
        return ((uint64_t)sk << 48) | ((uint64_t)trim << 32);
    }
    /* Walker sessions use score, pathname, length. The low 16 bits stay 0. */
    if (crit->count == 3 && crit->crit[0] == BY_SCORE &&
        crit->crit[1] == BY_PATHNAME && crit->crit[2] == BY_LENGTH &&
        it->text.ascii && it->text.bytes) {
        uint16_t sk = (uint16_t)(0xFFFFu - (uint16_t)score);
        uint16_t trim = ix_trim(it);
        uint16_t path = 0xFFFF;
        if (valid && b >= 0 && b <= 0xFFFF) {
            const unsigned char *s = (const unsigned char *)it->text.bytes;
            int len = it->text.length;
            int last = -1;
            int i;
            for (i = len - 1; i >= 0; i--) {
                unsigned char c = s[i];
                if (c == '/' || c == '\\') { last = i; break; }
            }
            if (last <= b) path = (uint16_t)(b - last);
        }
        return ((uint64_t)sk << 48) | ((uint64_t)path << 32) | ((uint64_t)trim << 16);
    }
    mm_result_t2 tmp;
    mm_result_set_bounds(&tmp, it, score,
                         valid ? b : 0xFFFF,
                         valid ? b + 1 : 0xFFFF,
                         valid ? e : 0,
                         valid, crit);
    const uint16_t *p = tmp.points;
    return ((uint64_t)p[3] << 48) | ((uint64_t)p[2] << 32) |
           ((uint64_t)p[1] << 16) | (uint64_t)p[0];
}

static int ix_hit_cmp(const void *A, const void *B) {
    const ix_hit_t *a = A, *b = B;
    if (a->key < b->key) return -1;
    if (a->key > b->key) return 1;
    if (a->index < b->index) return -1;
    if (a->index > b->index) return 1;
    return 0;
}

static int ix_tmp_ensure(int n) {
    if (n < 0) return 0;
    if ((size_t)n <= g_ix_tmp_cap) return 1;
    size_t cap = g_ix_tmp_cap ? g_ix_tmp_cap : 1024;
    while (cap < (size_t)n) {
        if (cap > SIZE_MAX / 2) { cap = (size_t)n; break; }
        cap *= 2;
    }
    ix_hit_t *p = cap > SIZE_MAX / sizeof(ix_hit_t) ? NULL : malloc(cap * sizeof(*p));
    if (!p) return 0;
    free(g_ix_tmp);
    g_ix_tmp = p;
    g_ix_tmp_cap = cap;
    return 1;
}

/* Stable LSD radix on the same 64-bit key mm_results_sort uses. --tac
   reverses the input first so ties run the other way. */
/* shift0 skips key bytes that are always zero (the packed tail). */
static void ix_sort(ix_hit_t *a, int n, bool tac, int shift0) {
    if (n < 2) return;
    if (shift0 < 0 || shift0 > 56) shift0 = 0;
    if (tac) {
        for (int i = 0, j = n - 1; i < j; i++, j--) {
            ix_hit_t t = a[i];
            a[i] = a[j];
            a[j] = t;
        }
    }
    if (!ix_tmp_ensure(n)) {
        qsort(a, (size_t)n, sizeof(*a), ix_hit_cmp);
        return;
    }
    ix_hit_t *src = a, *dst = g_ix_tmp;
    /* Large lists sort two key bytes at a time. A digit shared by every
       row does not move them, which keeps the sort stable. */
    if (n >= 8192 && (shift0 & 15) == 0) {
        for (int shift = shift0; shift < 64; shift += 16) {
            memset(g_ix_hist, 0, sizeof(g_ix_hist));
            uint32_t lo = 65535, hi = 0;
            for (int i = 0; i < n; i++) {
                uint32_t d = (uint32_t)((src[i].key >> shift) & 0xFFFF);
                g_ix_hist[d]++;
                if (d < lo) lo = d;
                if (d > hi) hi = d;
            }
            if (lo == hi) continue;
            uint32_t sum = 0;
            for (int b = (int)lo; b <= (int)hi; b++) {
                uint32_t c = g_ix_hist[b];
                g_ix_hist[b] = sum;
                sum += c;
            }
            for (int i = 0; i < n; i++) {
                uint32_t d = (uint32_t)((src[i].key >> shift) & 0xFFFF);
                dst[g_ix_hist[d]++] = src[i];
            }
            ix_hit_t *swap = src;
            src = dst;
            dst = swap;
        }
    } else {
        size_t count[256];
        for (int shift = shift0; shift < 64; shift += 8) {
            memset(count, 0, sizeof(count));
            for (int i = 0; i < n; i++) count[(src[i].key >> shift) & 0xFF]++;
            size_t sum = 0;
            for (int b = 0; b < 256; b++) {
                size_t c = count[b];
                count[b] = sum;
                sum += c;
            }
            for (int i = 0; i < n; i++)
                dst[count[(src[i].key >> shift) & 0xFF]++] = src[i];
            ix_hit_t *swap = src;
            src = dst;
            dst = swap;
        }
    }
    if (src != a) memcpy(a, src, (size_t)n * sizeof(*a));
}

static int ix_index(const mm_ix_worker_t *w, int v) {
    if (w->cands && v < w->ncand) return w->cands[v];
    int tail = w->cands ? v - w->ncand : v;
    return w->range_lo + tail;
}

static int ix_thread_count(int total) {
    if (total < 10000) return 1;
    static int cpus;
    if (cpus <= 0) {
        long n = sysconf(_SC_NPROCESSORS_ONLN);
        cpus = n > 0 ? (int)n : 4;
        if (cpus > 16) cpus = 16;
        if (cpus < 1) cpus = 1;
    }
    int nthreads = cpus;
    if (nthreads > total / 2) nthreads = total / 2;
    if (nthreads < 1) nthreads = 1;
    return nthreads;
}

/* 0: unsorted, 1: score+length, 2: score+pathname+length, 3: other criteria. */
static int ix_key_mode(const mm_ix_worker_t *w) {
    if (!w->sortable || !w->crit) return 0;
    const mm_sort_criteria_t *c = w->crit;
    if (c->count == 2 && c->crit[0] == BY_SCORE && c->crit[1] == BY_LENGTH)
        return 1;
    if (c->count == 3 && c->crit[0] == BY_SCORE && c->crit[1] == BY_PATHNAME &&
        c->crit[2] == BY_LENGTH)
        return 2;
    return 3;
}

/* One ASCII byte over the slice. The sort key is built in the same pass.
   A non-ASCII line comes back with key UINT64_MAX and is matched here. */
static void ix_scan_ascii1(mm_ix_worker_t *w) {
    int span = w->end - w->begin;
    if (span <= 0) { w->n = 0; w->ok = true; return; }
    int mode = ix_key_mode(w);
    if (mode == 3) {
        int n = 0;
        for (int v = w->begin; v < w->end; v++) {
            int idx = ix_index(w, v);
            if (idx < w->header_lines || idx >= w->item_count) continue;
            if (w->excluded && w->excluded[idx]) continue;
            mm_item_t *it = &w->items[idx];
            int start = 0, end = 0, score = 0;
            bool matched = false;
            if (it->text.ascii && it->text.bytes && !it->text.decoded &&
                it->text.length >= 1) {
                matched = mm_fuzzy_match_ascii1(
                    (const unsigned char *)it->text.bytes, it->text.length,
                    w->b1, w->dcs, w->forward, w->best_bonus, &start, &score);
                end = start + 1;
            } else {
                mm_text_t text = mm_item_match_text(it);
                if (text.length < 0) { w->ok = false; w->n = n; return; }
                mm_match_t m = mm_fuzzy_match(&text, w->dpat, w->dlen, w->dcs,
                                             w->dnorm, w->forward, false);
                matched = m.result.start >= 0;
                if (matched) {
                    score = m.result.score;
                    start = m.result.start;
                    end = m.result.end;
                }
            }
            if (!matched) continue;
            w->hits[n].key = ix_key(it, score, start, end, true, w->crit);
            w->hits[n].index = it->index;
            n++;
        }
        w->n = n;
        w->ok = true;
        return;
    }
    _Static_assert(sizeof(ix_hit_t) == sizeof(mm_hit_key_t), "hit layout");
    int slow = 0;
    int n = mm_ascii1_rank(w->items, w->cands, w->ncand, w->range_lo,
                           w->begin, w->end, w->item_count, w->header_lines,
                           w->excluded, w->b1, w->dcs, w->forward,
                           w->best_bonus, mode, (mm_hit_key_t *)w->hits, &slow);
    if (n < 0) { w->ok = false; w->n = 0; return; }
    if (slow) {
        int wout = 0;
        for (int i = 0; i < n; i++) {
            if (w->hits[i].key != UINT64_MAX) {
                if (wout != i) w->hits[wout] = w->hits[i];
                wout++;
                continue;
            }
            int idx = w->hits[i].index;
            if (idx < 0 || idx >= w->item_count) continue;
            mm_item_t *it = &w->items[idx];
            mm_text_t text = mm_item_match_text(it);
            if (text.length < 0) { w->ok = false; w->n = wout; return; }
            mm_match_t m = mm_fuzzy_match(&text, w->dpat, w->dlen, w->dcs,
                                         w->dnorm, w->forward, false);
            if (m.result.start < 0) continue;
            if (w->sortable)
                w->hits[wout].key = ix_key(it, m.result.score, m.result.start,
                                          m.result.end, true, w->crit);
            else
                w->hits[wout].key = 0;
            w->hits[wout].index = it->index;
            wout++;
        }
        n = wout;
    }
    w->n = n;
    w->ok = true;
}

static void ix_fix_slow(mm_ix_worker_t *w, int n) {
    int wout = 0;
    for (int i = 0; i < n; i++) {
        if (w->hits[i].key != UINT64_MAX) {
            if (wout != i) w->hits[wout] = w->hits[i];
            wout++;
            continue;
        }
        int idx = w->hits[i].index;
        if (idx < 0 || idx >= w->item_count) continue;
        mm_item_t *it = &w->items[idx];
        mm_text_t text = mm_item_match_text(it);
        if (text.length < 0) { w->ok = false; w->n = wout; return; }
        mm_match_t m = mm_fuzzy_match(&text, w->dpat, w->dlen, w->dcs,
                                     w->dnorm, w->forward, false);
        if (m.result.start < 0) continue;
        if (w->sortable)
            w->hits[wout].key = ix_key(it, m.result.score, m.result.start,
                                      m.result.end, true, w->crit);
        else
            w->hits[wout].key = 0;
        w->hits[wout].index = it->index;
        wout++;
    }
    w->n = wout;
    w->ok = true;
}

static void ix_scan_ascii_n(mm_ix_worker_t *w) {
    int span = w->end - w->begin;
    if (span <= 0) { w->n = 0; w->ok = true; return; }
    int mode = ix_key_mode(w);
    if (mode == 3) return;
    int slow = 0;
    int n = mm_ascii_n_rank(w->items, w->cands, w->ncand, w->range_lo,
                            w->begin, w->end, w->item_count, w->header_lines,
                            w->excluded, w->dpat, w->dlen, w->dcs, w->forward,
                            mode, (mm_hit_key_t *)w->hits, &slow);
    if (n < 0) { w->ok = false; w->n = 0; return; }
    if (slow) ix_fix_slow(w, n);
    else { w->n = n; w->ok = true; }
}

/* The first large sort writes a cold destination. Fault those pages while
   the scan runs, one slice per worker, so the radix is not a random fault. */
static void ix_touch_tmp(int begin, int end) {
    if (!g_ix_tmp || g_ix_touch_n <= 0 || end <= begin) return;
    if (begin >= g_ix_touch_n) return;
    if (end > g_ix_touch_n) end = g_ix_touch_n;
    size_t b0 = (size_t)begin * sizeof(ix_hit_t);
    size_t b1 = (size_t)end * sizeof(ix_hit_t);
    for (size_t page = b0 & ~(size_t)4095; page < b1; page += 4096) {
        size_t idx = page / sizeof(ix_hit_t);
        g_ix_tmp[idx].key = 0;
    }
}

static void *ix_worker_run(void *arg) {
    mm_ix_worker_t *w = arg;
    if (!w->hits) { w->ok = false; w->n = 0; return NULL; }
    ix_touch_tmp(w->begin, w->end);
    if (w->byte_pat && w->dlen == 1) {
        ix_scan_ascii1(w);
        return NULL;
    }
    if (w->byte_pat && w->dlen > 1 && w->dlen <= 64 && ix_key_mode(w) != 3) {
        ix_scan_ascii_n(w);
        if (w->ok) return NULL;
    }
    int n = 0;
    for (int v = w->begin; v < w->end; v++) {
        int idx = ix_index(w, v);
        if (idx < w->header_lines || idx >= w->item_count) continue;
        if (w->excluded && w->excluded[idx]) continue;
        mm_item_t *it = &w->items[idx];
        /* A typed query is one ASCII fuzzy term. Skip the pattern walk and
           the match-struct return; the score is the one fuzzyMatchV2 uses. */
        if (w->byte_pat && it->text.ascii && it->text.bytes && !it->text.decoded) {
            const unsigned char *s = (const unsigned char *)it->text.bytes;
            int len = it->text.length;
            bool small = w->dlen == 1 ||
                         (w->dlen <= 64 && (int64_t)len * (int64_t)w->dlen <= 100000);
            if (small && len >= w->dlen) {
                int start = 0, end = 0, score = 0;
                bool matched;
                if (w->dlen == 1) {
                    matched = mm_fuzzy_match_ascii1(s, len, w->b1, w->dcs, w->forward,
                                                   w->best_bonus, &start, &score);
                    end = start + 1;
                } else {
                    matched = mm_fuzzy_match_ascii(s, len, w->dpat, w->dlen, w->dcs,
                                                  w->forward, &start, &end, &score);
                }
                if (!matched) continue;
                if (w->sortable)
                    w->hits[n].key = ix_key(it, score, start, end, true, w->crit);
                w->hits[n].index = it->index;
                n++;
                continue;
            }
        }
        mm_text_t text;
        if (it->text.ascii && it->text.bytes && !it->text.decoded) {
            text.runes = NULL;
            text.length = it->text.length;
            text.bytes = (const unsigned char *)it->text.bytes;
        } else {
            text = mm_item_match_text(it);
        }
        if (text.length < 0) { w->ok = false; w->n = n; return NULL; }
        int score = 0, b = -1, e = -1;
        bool matched;
        if (w->direct) {
            mm_match_t m = mm_fuzzy_match(&text, w->dpat, w->dlen, w->dcs, w->dnorm,
                                         w->forward, false);
            matched = m.result.start >= 0;
            if (matched) {
                score = m.result.score;
                b = m.result.start;
                e = m.result.end;
            }
        } else {
            matched = mm_pattern_match_span(w->pat, &text, &score, &b, &e);
        }
        if (!matched) continue;
        bool valid = b >= 0 && e > b;
        if (w->sortable)
            w->hits[n].key = ix_key(it, score, b, e, valid, w->crit);
        w->hits[n].index = it->index;
        n++;
    }
    w->n = n;
    w->ok = true;
    return NULL;
}

/* Threads stay up across keystrokes. Creating twelve of them is visible
   once the matcher itself is only a couple of milliseconds. The same pool
   sorts, so a large hit list does not run the radix on one core. */
typedef void (*ix_task_fn)(int id, void *arg);

static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pool_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_pool_done_cv = PTHREAD_COND_INITIALIZER;
static pthread_t *g_pool_th;
static ix_task_fn g_pool_fn;
static void *g_pool_arg;
static int g_pool_njobs;
static int g_pool_nthreads;
static int g_pool_ready;
static int g_pool_done;
static int g_pool_epoch;
static int g_pool_stop;

static void ix_task_match(int id, void *arg) {
    ix_worker_run(&((mm_ix_worker_t *)arg)[id]);
}

static void *ix_pool_main(void *arg) {
    int id = (int)(intptr_t)arg;
    int local = 0;
    pthread_mutex_lock(&g_pool_mu);
    g_pool_ready++;
    pthread_cond_signal(&g_pool_done_cv);
    for (;;) {
        while (g_pool_epoch == local && !g_pool_stop)
            pthread_cond_wait(&g_pool_cv, &g_pool_mu);
        if (g_pool_stop) break;
        local = g_pool_epoch;
        int njobs = g_pool_njobs;
        ix_task_fn fn = g_pool_fn;
        void *carg = g_pool_arg;
        pthread_mutex_unlock(&g_pool_mu);
        if (id < njobs && fn) fn(id, carg);
        pthread_mutex_lock(&g_pool_mu);
        g_pool_done++;
        pthread_cond_signal(&g_pool_done_cv);
    }
    pthread_mutex_unlock(&g_pool_mu);
    return NULL;
}

static int ix_pool_ensure(int want) {
    pthread_mutex_lock(&g_pool_mu);
    while (g_pool_nthreads < want) {
        int id = g_pool_nthreads;
        pthread_t *nth = realloc(g_pool_th, (size_t)(id + 1) * sizeof(pthread_t));
        if (!nth) {
            pthread_mutex_unlock(&g_pool_mu);
            return 0;
        }
        g_pool_th = nth;
        pthread_mutex_unlock(&g_pool_mu);
        int rc = pthread_create(&g_pool_th[id], NULL, ix_pool_main,
                                (void *)(intptr_t)id);
        pthread_mutex_lock(&g_pool_mu);
        if (rc != 0) {
            pthread_mutex_unlock(&g_pool_mu);
            return 0;
        }
        g_pool_nthreads++;
        while (g_pool_ready < g_pool_nthreads)
            pthread_cond_wait(&g_pool_done_cv, &g_pool_mu);
    }
    pthread_mutex_unlock(&g_pool_mu);
    return 1;
}

static void ix_pool_dispatch(int nthreads, ix_task_fn fn, void *arg) {
    if (nthreads <= 1 || !fn || !ix_pool_ensure(nthreads)) {
        if (fn) {
            for (int k = 0; k < nthreads; k++) fn(k, arg);
        }
        return;
    }
    pthread_mutex_lock(&g_pool_mu);
    g_pool_fn = fn;
    g_pool_arg = arg;
    g_pool_njobs = nthreads;
    g_pool_done = 0;
    g_pool_epoch++;
    pthread_cond_broadcast(&g_pool_cv);
    while (g_pool_done < g_pool_nthreads)
        pthread_cond_wait(&g_pool_done_cv, &g_pool_mu);
    pthread_mutex_unlock(&g_pool_mu);
}

static void ix_run_workers(mm_ix_worker_t *w, int nthreads) {
    ix_pool_dispatch(nthreads, ix_task_match, w);
}

int *mm_batch_search_interactive(mm_item_t *items, int item_count,
                                 const int *candidates, int ncand,
                                 int range_lo, int range_hi,
                                 const bool *excluded, int header_lines,
                                 mm_pattern_t *pat, const mm_sort_criteria_t *crit,
                                 bool tac, int *out_n) {
    if (out_n) *out_n = -1;
    if (!items || item_count < 0 || !pat || !crit || !out_n) return NULL;
    if (ncand < 0) ncand = 0;
    if (!candidates) ncand = 0;
    if (range_hi < range_lo) range_hi = range_lo;
    int tail = range_hi - range_lo;
    if (ncand > INT_MAX - tail) return NULL;
    int vtotal = ncand + tail;
    if (vtotal == 0) {
        *out_n = 0;
        return malloc(sizeof(int));
    }

    int nthreads = ix_thread_count(vtotal);
    mm_ix_worker_t *w = calloc((size_t)nthreads, sizeof(*w));
    ix_hit_t *slab = ix_slab_get(vtotal);
    if (!w || !slab) {
        free(w);
        return NULL;
    }

    /* One non-inverse fuzzy term is what a typed query usually is. Call the
       matcher directly; fzf does the same and skips the term-set walk. */
    const wchar_t *dpat = NULL;
    int dlen = 0;
    bool direct = false, dcs = false, dnorm = false;
    if (!pat->extended) {
        if (pat->fuzzy && pat->text && pat->textlen > 0) {
            direct = true;
            dpat = pat->text;
            dlen = pat->textlen;
            dcs = pat->case_sensitive;
            dnorm = pat->normalize;
        }
    } else if (pat->ntermsets == 1 && pat->term_sets &&
               pat->term_sets[0].count == 1 && pat->term_sets[0].terms) {
        mm_term_t *term = &pat->term_sets[0].terms[0];
        if (!term->inv && term->typ == TERM_FUZZY && term->text && term->tlen > 0) {
            direct = true;
            dpat = term->text;
            dlen = term->tlen;
            dcs = term->case_sensitive;
            dnorm = term->normalize;
        }
    }

    bool byte_pat = false;
    unsigned char b1 = 0;
    int16_t best = -1;
    if (direct && dpat && dlen > 0) {
        byte_pat = true;
        for (int i = 0; i < dlen; i++) {
            if (dpat[i] < 0 || dpat[i] >= 128) {
                byte_pat = false;
                break;
            }
        }
        if (byte_pat && dlen == 1) {
            b1 = (unsigned char)dpat[0];
            best = mm_max_first_bonus(b1, dcs);
        }
    }

    g_ix_touch_n = (pat->sortable && ix_tmp_ensure(vtotal)) ? vtotal : 0;
    int offset = 0;
    for (int k = 0; k < nthreads; k++) {
        int count = vtotal / nthreads + (k < vtotal % nthreads);
        w[k].items = items;
        w[k].cands = candidates;
        w[k].ncand = ncand;
        w[k].range_lo = range_lo;
        w[k].begin = offset;
        w[k].end = offset + count;
        w[k].excluded = excluded;
        w[k].item_count = item_count;
        w[k].header_lines = header_lines;
        w[k].pat = pat;
        w[k].crit = crit;
        w[k].hits = slab + offset;
        w[k].dpat = dpat;
        w[k].dlen = dlen;
        w[k].direct = direct;
        w[k].dcs = dcs;
        w[k].dnorm = dnorm;
        w[k].forward = pat->forward;
        w[k].sortable = pat->sortable;
        w[k].byte_pat = byte_pat;
        w[k].b1 = b1;
        w[k].best_bonus = best;
        offset += count;
    }
    ix_run_workers(w, nthreads);
    bool failed = false;
    int n = 0;
    for (int k = 0; k < nthreads; k++) {
        if (!w[k].ok) failed = true;
        if (!failed && w[k].n > 0 && n > INT_MAX - w[k].n) failed = true;
        else if (!failed) n += w[k].n;
    }
    if (!failed) {
        int at = 0;
        for (int k = 0; k < nthreads; k++) {
            if (w[k].n > 0 && at != w[k].begin)
                memmove(slab + at, slab + w[k].begin,
                        (size_t)w[k].n * sizeof(*slab));
            at += w[k].n;
        }
        if (pat->sortable) {
            int shift0 = 0;
            if (crit->count == 2 && crit->crit[0] == BY_SCORE &&
                crit->crit[1] == BY_LENGTH)
                shift0 = 32;
            else if (crit->count == 3 && crit->crit[0] == BY_SCORE &&
                     crit->crit[1] == BY_PATHNAME && crit->crit[2] == BY_LENGTH)
                shift0 = 16;
            ix_sort(slab, n, tac, shift0);
        }
        else if (tac && n > 1) {
            for (int i = 0, j = n - 1; i < j; i++, j--) {
                ix_hit_t tmp = slab[i];
                slab[i] = slab[j];
                slab[j] = tmp;
            }
        }
        int *out = malloc((size_t)(n > 0 ? n : 1) * sizeof(*out));
        if (out) {
            for (int i = 0; i < n; i++) out[i] = slab[i].index;
            *out_n = n;
            free(w);
            return out;
        }
    }
    free(w);
    return NULL;
}
