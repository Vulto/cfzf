/* bind.c - parse --bind specifications and look up actions. */
#define _POSIX_C_SOURCE 200809L
#include "bind.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

static const struct { const char *name; mm_action_t action; } action_table[] = {
    { "accept", MM_ACT_ACCEPT },
    { "accept-non-empty", MM_ACT_ACCEPT_NON_EMPTY },
    { "accept-or-print-query", MM_ACT_ACCEPT_OR_PRINT_QUERY },
    { "abort", MM_ACT_ABORT },
    { "cancel", MM_ACT_CANCEL },
    { "clear-query", MM_ACT_CLEAR_QUERY },
    { "clear-selection", MM_ACT_CLEAR_SELECTION },
    { "clear-screen", MM_ACT_CLEAR_SCREEN },
    { "close", MM_ACT_CLOSE },
    { "bell", MM_ACT_BELL },
    { "up", MM_ACT_UP },
    { "down", MM_ACT_DOWN },
    { "up-match", MM_ACT_UP_MATCH },
    { "down-match", MM_ACT_DOWN_MATCH },
    { "up-selected", MM_ACT_UP_SELECTED },
    { "down-selected", MM_ACT_DOWN_SELECTED },
    { "first", MM_ACT_FIRST },
    { "last", MM_ACT_LAST },
    { "page-up", MM_ACT_PAGE_UP },
    { "page-down", MM_ACT_PAGE_DOWN },
    { "half-page-up", MM_ACT_HALF_PAGE_UP },
    { "half-page-down", MM_ACT_HALF_PAGE_DOWN },
    { "backward-delete-char", MM_ACT_BACKWARD_DELETE_CHAR },
    { "delete-char", MM_ACT_DELETE_CHAR },
    { "kill-line", MM_ACT_KILL_LINE },
    { "kill-word", MM_ACT_KILL_WORD },
    { "backward-kill-word", MM_ACT_BACKWARD_KILL_WORD },
    { "unix-word-rubout", MM_ACT_UNIX_WORD_RUBOUT },
    { "toggle", MM_ACT_TOGGLE },
    { "toggle-all", MM_ACT_TOGGLE_ALL },
    { "toggle-down", MM_ACT_TOGGLE_DOWN },
    { "toggle-up", MM_ACT_TOGGLE_UP },
    { "select-all", MM_ACT_SELECT_ALL },
    { "deselect-all", MM_ACT_DESELECT_ALL },
    { "deselect", MM_ACT_DESELECT },
    { "deselect-down", MM_ACT_DESELECT_DOWN },
    { "refresh", MM_ACT_REFRESH },
    { "refresh-preview", MM_ACT_REFRESH_PREVIEW },
    { "print-query", MM_ACT_PRINT_QUERY },
    { "execute", MM_ACT_EXECUTE },
    { "execute-silent", MM_ACT_EXECUTE_SILENT },
    { "execute-multi", MM_ACT_EXECUTE_MULTI },
    { "change-query", MM_ACT_CHANGE_QUERY },
    { "change-prompt", MM_ACT_CHANGE_PROMPT },
    { "change-header", MM_ACT_CHANGE_HEADER },
    { "change-multi", MM_ACT_CHANGE_MULTI },
    { "change-pointer", MM_ACT_CHANGE_POINTER },
    { "ignore", MM_ACT_IGNORE },
    { "print", MM_ACT_PRINT },
    { "become", MM_ACT_BECOME },
    { "reload", MM_ACT_RELOAD },
    { "prev-history", MM_ACT_PREV_HISTORY },
    { "next-history", MM_ACT_NEXT_HISTORY },
    { "clear-multi", MM_ACT_CLEAR_MULTI },
    { "deselect-down", MM_ACT_DESELECT_DOWN },
    { "hide-header", MM_ACT_HIDE_HEADER },
    { "hide-preview", MM_ACT_HIDE_PREVIEW },
    { "offset-down", MM_ACT_OFFSET_DOWN },
    { "offset-middle", MM_ACT_OFFSET_MIDDLE },
    { "offset-up", MM_ACT_OFFSET_UP },
    { "pos", MM_ACT_POS },
    { "replace-query", MM_ACT_REPLACE_QUERY },
    { "toggle-header", MM_ACT_TOGGLE_HEADER },
    { "toggle-hscroll", MM_ACT_TOGGLE_HSCROLL },
    { "toggle-preview", MM_ACT_TOGGLE_PREVIEW },
    { "toggle-preview-wrap", MM_ACT_TOGGLE_PREVIEW_WRAP },
    { "toggle-preview-wrap-word", MM_ACT_TOGGLE_PREVIEW_WRAP_WORD },
    { "toggle-raw", MM_ACT_TOGGLE_RAW },
    { "disable-search", MM_ACT_DISABLE_SEARCH },
    { "enable-search", MM_ACT_ENABLE_SEARCH },
    { "toggle-search", MM_ACT_TOGGLE_SEARCH },
    { "toggle-sort", MM_ACT_TOGGLE_SORT },
    { "toggle-track", MM_ACT_TOGGLE_TRACK },
    { "toggle-track-current", MM_ACT_TOGGLE_TRACK_CURRENT },
    { "toggle-wrap", MM_ACT_TOGGLE_WRAP },
    { "toggle-wrap-word", MM_ACT_TOGGLE_WRAP_WORD },
    { "track", MM_ACT_TRACK },
    { "unbind", MM_ACT_UNBIND },
    { "unix-line-discard", MM_ACT_UNIX_LINE_DISCARD },
    { "untrack-current", MM_ACT_UNTRACK_CURRENT },
    { "wait", MM_ACT_WAIT },
    { "yank", MM_ACT_YANK },
    { "change-preview", MM_ACT_CHANGE_PREVIEW },
    { "change-preview-window", MM_ACT_CHANGE_PREVIEW_WINDOW },
    { "preview", MM_ACT_PREVIEW },
    { "transform", MM_ACT_TRANSFORM },
    { "bg-transform", MM_ACT_BG_TRANSFORM },
    { "best", MM_ACT_BEST },
    { "put", MM_ACT_PUT },
    { "select", MM_ACT_SELECT },
    { "exclude", MM_ACT_EXCLUDE },
    { "exclude-multi", MM_ACT_EXCLUDE_MULTI },
    { "jump", MM_ACT_JUMP },
    { "jump-accept", MM_ACT_JUMP_ACCEPT },
    { "backward-char", MM_ACT_BACKWARD_CHAR },
    { "forward-char", MM_ACT_FORWARD_CHAR },
    { "beginning-of-line", MM_ACT_BEGINNING_OF_LINE },
    { "end-of-line", MM_ACT_END_OF_LINE },
    { "forward-word", MM_ACT_FORWARD_WORD },
    { "backward-word", MM_ACT_BACKWARD_WORD },
    { "backward-kill-subword", MM_ACT_BACKWARD_KILL_SUBWORD },
    { "forward-subword", MM_ACT_FORWARD_SUBWORD },
    { "reload-sync", MM_ACT_RELOAD_SYNC },
    { "rebind", MM_ACT_REBIND },
    { "show-header", MM_ACT_SHOW_HEADER },
    { "show-input", MM_ACT_SHOW_INPUT },
    { "show-preview", MM_ACT_SHOW_PREVIEW },
    { "toggle-multi-line", MM_ACT_TOGGLE_MULTI_LINE },
    { "toggle-input", MM_ACT_TOGGLE_INPUT },
    { "toggle-in", MM_ACT_TOGGLE_IN },
    { "toggle-out", MM_ACT_TOGGLE_OUT },
    { "select-down", MM_ACT_SELECT_DOWN },
    { "deselect-up", MM_ACT_DESELECT_UP },
    { "bg-cancel", MM_ACT_BG_CANCEL },
    { NULL, MM_ACT_IGNORE }
};

const char *mm_action_name(mm_action_t a) {
    for (size_t i = 0; i < sizeof(action_table) / sizeof(action_table[0]); i++)
        if (action_table[i].action == a) return action_table[i].name;
    return "none";
}

static bool keymap_push(mm_keymap_t *km, mm_binding_t b) {
    if (km->n == km->cap) {
        int ncap = km->cap ? km->cap * 2 : 16;
        mm_binding_t *items = realloc(km->items, (size_t)ncap * sizeof(mm_binding_t));
        if (!items) return false;
        km->items = items;
        km->cap = ncap;
    }
    km->items[km->n++] = b;
    return true;
}

/* Parse a key name into an event type and rune. */
static bool parse_key(const char *key, mm_event_type_t *type, wchar_t *rune) {
    *rune = 0;
    if (strcmp(key, "space") == 0) { *type = MM_EV_RUNE; *rune = ' '; return true; }
    if (strcmp(key, "enter") == 0 || strcmp(key, "return") == 0) { *type = MM_EV_ENTER; return true; }
    if (mm_event_from_name(key, type)) {
        if (*type == MM_EV_RUNE && key[0] && !key[1]) *rune = (wchar_t)key[0];
        return true;
    }
    /* A single character is a rune binding. */
    if (key[0] && !key[1]) { *type = MM_EV_RUNE; *rune = (wchar_t)key[0]; return true; }
    return false;
}

/* Parse an action name with an optional "(argument)" suffix. */
static bool parse_action(const char *spec, mm_action_t *action, char **arg) {
    *arg = NULL;
    size_t len = strlen(spec);
    char name[64];
    size_t nlen = len;
    const char *paren = strchr(spec, '(');
    if (paren) {
        nlen = (size_t)(paren - spec);
        if (nlen >= sizeof(name)) nlen = sizeof(name) - 1;
        if (spec[len - 1] != ')') return false;
        *arg = malloc(len - nlen);
        if (!*arg) return false;
        memcpy(*arg, spec + nlen + 1, len - nlen - 2);
        (*arg)[len - nlen - 2] = 0;
    } else {
        if (len >= sizeof(name)) return false;
    }
    memcpy(name, spec, nlen);
    name[nlen] = 0;
    for (size_t i = 0; i < sizeof(action_table) / sizeof(action_table[0]); i++) {
        if (strcmp(action_table[i].name, name) == 0) {
            *action = action_table[i].action;
            return true;
        }
    }
    free(*arg);
    *arg = NULL;
    return false;
}

/* Remove the bindings of a key, so that a user binding replaces the
   built-in one (fzf stores the bindings in a map). */
static void keymap_remove_key(mm_keymap_t *km, mm_event_type_t type, wchar_t rune) {
    int w = 0;
    for (int i = 0; i < km->n; i++) {
        if (km->items[i].type == type &&
            (type != MM_EV_RUNE || km->items[i].rune == rune)) {
            free(km->items[i].arg);
            continue;
        }
        km->items[w++] = km->items[i];
    }
    km->n = w;
}

bool mm_bind_unbind(mm_keymap_t *km, const char *keys) {
    if (!km || !keys || !*keys) return false;
    if (strcmp(keys, "*") == 0) {
        for (int i = 0; i < km->n; i++) free(km->items[i].arg);
        km->n = 0;
        return true;
    }
    const char *p = keys;
    while (*p) {
        const char *end = strchr(p, ',');
        size_t len = end ? (size_t)(end - p) : strlen(p);
        while (len && (*p == ' ' || *p == '\t')) { p++; len--; }
        while (len && (p[len - 1] == ' ' || p[len - 1] == '\t')) len--;
        if (len == 0) return false;
        char *name = malloc(len + 1);
        if (!name) return false;
        memcpy(name, p, len);
        name[len] = '\0';
        mm_event_type_t type;
        wchar_t rune;
        bool valid = parse_key(name, &type, &rune);
        free(name);
        if (!valid) return false;
        keymap_remove_key(km, type, rune);
        if (!end) break;
        p = end + 1;
        if (!*p) return false;
    }
    return true;
}

bool mm_bind_parse(mm_keymap_t *km, const char *spec, bool replace, const char **err) {
    if (!spec || !*spec) { *err = "bind action required"; return false; }

    const char *p = spec;
    while (*p) {
        /* Key part: up to the first ':' that is not inside parentheses. */
        const char *colon = NULL;
        int depth = 0;
        for (const char *q = p; *q; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ':' && depth == 0) { colon = q; break; }
        }
        if (!colon) { *err = "invalid key specification"; return false; }

        char *key = malloc((size_t)(colon - p) + 1);
        if (!key) { *err = "out of memory"; return false; }
        memcpy(key, p, (size_t)(colon - p));
        key[colon - p] = 0;

        mm_event_type_t type;
        wchar_t rune;
        if (!parse_key(key, &type, &rune)) {
            free(key);
            *err = "unsupported key";
            return false;
        }
        free(key);

        /* A user binding replaces the built-in binding of the same key. */
        if (replace) keymap_remove_key(km, type, rune);

        /* Action part: up to the next ',' at depth 0. */
        const char *q = colon + 1;
        depth = 0;
        const char *end = NULL;
        for (const char *r = q; *r; r++) {
            if (*r == '(') depth++;
            else if (*r == ')') depth--;
            else if (*r == ',' && depth == 0) { end = r; break; }
        }
        if (!end) end = q + strlen(q);
        size_t alen = (size_t)(end - q);
        char *actions = malloc(alen + 1);
        if (!actions) { *err = "out of memory"; return false; }
        memcpy(actions, q, alen);
        actions[alen] = 0;

        /* Actions may be chained with '+'. */
        char *save = NULL;
        for (char *tok = strtok_r(actions, "+", &save); tok;
             tok = strtok_r(NULL, "+", &save)) {
            mm_action_t action;
            char *arg = NULL;
            if (!parse_action(tok, &action, &arg)) {
                free(actions);
                *err = "unsupported action";
                return false;
            }
            mm_binding_t b = { type, rune, action, arg };
            if (!keymap_push(km, b)) {
                free(arg);
                free(actions);
                *err = "out of memory";
                return false;
            }
        }
        free(actions);

        p = (end < q + strlen(q) && *end == ',') ? end + 1 : end;
        if (*p == 0) break;
    }
    return true;
}

int mm_bind_lookup(const mm_keymap_t *km, const mm_event_t *ev,
                   const mm_binding_t **out, int max) {
    int found = 0;
    for (int i = 0; i < km->n && found < max; i++) {
        const mm_binding_t *b = &km->items[i];
        if (b->type != ev->type) continue;
        if (b->type == MM_EV_RUNE && b->rune != ev->rune) continue;
        out[found++] = b;
    }
    return found;
}

void mm_keymap_free(mm_keymap_t *km) {
    for (int i = 0; i < km->n; i++) free(km->items[i].arg);
    free(km->items);
    km->items = NULL;
    km->n = km->cap = 0;
}

/* Valid fzf event names. */
static const char *event_names[] = {
    "start", "load", "change", "focus", "result", "result-final",
    "resize", "one", "zero", "jump", "jump-cancel", "click-header",
    "click-footer", NULL
};

static bool is_event_name(const char *name) {
    for (int i = 0; event_names[i]; i++)
        if (strcmp(name, event_names[i]) == 0) return true;
    return false;
}

/* Push an event binding onto the event map. */
static bool event_map_push(mm_event_map_t *em, mm_event_binding_t b) {
    if (em->n == em->cap) {
        int ncap = em->cap ? em->cap * 2 : 16;
        mm_event_binding_t *items = realloc(em->items, (size_t)ncap * sizeof(mm_event_binding_t));
        if (!items) return false;
        em->items = items;
        em->cap = ncap;
    }
    em->items[em->n++] = b;
    return true;
}

/* Parse an event binding specification ("event:action(+action)...") and append
   the bindings to the event map. Returns false on a parse error. */
bool mm_event_bind_parse(mm_event_map_t *em, const char *spec, const char **err) {
    if (!spec || !*spec) { *err = "event binding required"; return false; }

    const char *p = spec;
    while (*p) {
        /* Event name part: up to the first ':' that is not inside parentheses. */
        const char *colon = NULL;
        int depth = 0;
        for (const char *q = p; *q; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ':' && depth == 0) { colon = q; break; }
        }
        if (!colon) { *err = "invalid event specification"; return false; }

        size_t elen = (size_t)(colon - p);
        char *name = malloc(elen + 1);
        if (!name) { *err = "out of memory"; return false; }
        memcpy(name, p, elen);
        name[elen] = 0;

        if (!is_event_name(name)) {
            free(name);
            *err = "unsupported event";
            return false;
        }

        /* Action part: up to the next ',' at depth 0. */
        const char *q = colon + 1;
        depth = 0;
        const char *end = NULL;
        for (const char *r = q; *r; r++) {
            if (*r == '(') depth++;
            else if (*r == ')') depth--;
            else if (*r == ',' && depth == 0) { end = r; break; }
        }
        if (!end) end = q + strlen(q);
        size_t alen = (size_t)(end - q);
        char *actions = malloc(alen + 1);
        if (!actions) { free(name); *err = "out of memory"; return false; }
        memcpy(actions, q, alen);
        actions[alen] = 0;

        char *save = NULL;
        for (char *tok = strtok_r(actions, "+", &save);
             tok; tok = strtok_r(NULL, "+", &save)) {
            mm_action_t action;
            char *arg = NULL;
            if (!parse_action(tok, &action, &arg)) {
                free(name); free(actions); free(arg);
                *err = "unsupported action";
                return false;
            }
                mm_event_binding_t b = { strdup(name), action, arg };
            if (!b.name) { free(name); free(actions); free(arg);
                *err = "out of memory"; return false; }
            if (!event_map_push(em, b)) {
                free(name); free(actions); free((void *)b.name); free(arg);
                *err = "out of memory";
                return false;
            }
        }
        free(actions);
        free(name);

        p = (end < q + strlen(q) && *end == ',') ? end + 1 : end;
        if (*p == 0) break;
    }
    return true;
}

/* Look up the actions bound to an event name. */
int mm_event_lookup(const mm_event_map_t *em, const char *name,
                    const mm_event_binding_t **out, int max) {
    int found = 0;
    for (int i = 0; i < em->n && found < max; i++) {
        if (strcmp(em->items[i].name, name) == 0)
            out[found++] = &em->items[i];
    }
    return found;
}

void mm_event_map_free(mm_event_map_t *em) {
    if (!em) return;
    for (int i = 0; i < em->n; i++) {
        free((char *)em->items[i].name);
        free(em->items[i].arg);
    }
    free(em->items);
    em->items = NULL;
    em->n = em->cap = 0;
}

/* Parse a --bind specification, routing key bindings to *km and event
   bindings to *em. Returns false on a parse error. */
bool mm_bind_parse_full(mm_keymap_t *km, mm_event_map_t *em, const char *spec,
                        const char **err) {
    if (!spec || !*spec) { *err = "bind specification required"; return false; }

    const char *p = spec;
    while (*p) {
        /* Split each binding at the top-level ','. */
        const char *comma = NULL;
        int depth = 0;
        for (const char *q = p; *q; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ',' && depth == 0) { comma = q; break; }
        }
        const char *bind_end = comma ? comma : p + strlen(p);

        /* Find the ':' within this binding. */
        const char *colon = NULL;
        depth = 0;
        for (const char *q = p; q < bind_end; q++) {
            if (*q == '(') depth++;
            else if (*q == ')') depth--;
            else if (*q == ':' && depth == 0) { colon = q; break; }
        }
        if (!colon) { *err = "invalid key specification"; return false; }

        size_t klen = (size_t)(colon - p);
        char *key = malloc(klen + 1);
        if (!key) { *err = "out of memory"; return false; }
        memcpy(key, p, klen);
        key[klen] = 0;

        bool is_event = is_event_name(key);

        /* Parse the action part. */
        const char *q = colon + 1;
        depth = 0;
        const char *aend = NULL;
        for (const char *r = q; r < bind_end; r++) {
            if (*r == '(') depth++;
            else if (*r == ')') depth--;
            else if (*r == ',' && depth == 0) { aend = r; break; }
        }
        if (!aend) aend = bind_end;
        size_t alen = (size_t)(aend - q);
        char *actions = malloc(alen + 1);
        if (!actions) { free(key); *err = "out of memory"; return false; }
        memcpy(actions, q, alen);
        actions[alen] = 0;

        char *save = NULL;
        for (char *tok = strtok_r(actions, "+", &save);
             tok; tok = strtok_r(NULL, "+", &save)) {
            mm_action_t action;
            char *arg = NULL;
            if (!parse_action(tok, &action, &arg)) {
                free(key); free(actions); free(arg);
                *err = "unsupported action";
                return false;
            }

        if (is_event) {
            /* fzf allows binding to events */
                mm_event_binding_t b = { strdup(key), action, arg };
                if (!b.name) { free(key); free(actions); free(arg);
                    *err = "out of memory"; return false; }
                if (!event_map_push(em, b)) {
                    free(key); free(actions); free((void *)b.name); free(arg);
                    *err = "out of memory";
                    return false;
                }
            } else {
                mm_event_type_t type;
                wchar_t rune;
                if (!parse_key(key, &type, &rune)) {
                    free(key); free(actions); free(arg);
                    *err = "unsupported key";
                    return false;
                }
                mm_binding_t b = { type, rune, action, arg };
                if (!keymap_push(km, b)) {
                    free(key); free(actions); free(arg);
                    *err = "out of memory";
                    return false;
                }
            }
        }
        free(actions);
        free(key);

        p = (comma) ? comma + 1 : bind_end;
        if (*p == 0) break;
    }
    return true;
}
