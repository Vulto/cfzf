/* bind.h - key/event bindings and actions (port of fzf's --bind handling in
   src/options.go and the action execution in src/terminal.go). */
#ifndef MM_BIND_H
#define MM_BIND_H

#include "event.h"

#include <stdbool.h>
#include <wchar.h>

typedef enum {
    MM_ACT_NONE = 0,
    MM_ACT_ACCEPT,
    MM_ACT_ACCEPT_NON_EMPTY,
    MM_ACT_ACCEPT_OR_PRINT_QUERY,
    MM_ACT_ABORT,
    MM_ACT_CANCEL,
    MM_ACT_CLEAR_QUERY,
    MM_ACT_CLEAR_SELECTION,
    MM_ACT_CLEAR_SCREEN,
    MM_ACT_CLOSE,
    MM_ACT_BELL,
    MM_ACT_UP,
    MM_ACT_DOWN,
    MM_ACT_UP_MATCH,
    MM_ACT_DOWN_MATCH,
    MM_ACT_UP_SELECTED,
    MM_ACT_DOWN_SELECTED,
    MM_ACT_FIRST,
    MM_ACT_LAST,
    MM_ACT_PAGE_UP,
    MM_ACT_PAGE_DOWN,
    MM_ACT_HALF_PAGE_UP,
    MM_ACT_HALF_PAGE_DOWN,
    MM_ACT_BACKWARD_DELETE_CHAR,
    MM_ACT_DELETE_CHAR,
    MM_ACT_KILL_LINE,
    MM_ACT_KILL_WORD,
    MM_ACT_BACKWARD_KILL_WORD,
    MM_ACT_UNIX_WORD_RUBOUT,
    MM_ACT_TOGGLE,
    MM_ACT_TOGGLE_ALL,
    MM_ACT_TOGGLE_DOWN,
    MM_ACT_TOGGLE_UP,
    MM_ACT_SELECT_ALL,
    MM_ACT_DESELECT_ALL,
    MM_ACT_DESELECT,
    MM_ACT_DESELECT_DOWN,
    MM_ACT_REFRESH,
    MM_ACT_REFRESH_PREVIEW,
    MM_ACT_PRINT_QUERY,
    MM_ACT_EXECUTE,
    MM_ACT_EXECUTE_SILENT,
    MM_ACT_EXECUTE_MULTI,
    MM_ACT_CHANGE_BORDER_LABEL,
    MM_ACT_CHANGE_FOOTER,
    MM_ACT_CHANGE_GHOST,
    MM_ACT_CHANGE_HEADER_LABEL,
    MM_ACT_CHANGE_INPUT_LABEL,
    MM_ACT_CHANGE_LIST_LABEL,
    MM_ACT_CHANGE_PREVIEW_LABEL,
    MM_ACT_CHANGE_QUERY,
    MM_ACT_CHANGE_PROMPT,
    MM_ACT_CHANGE_HEADER,
    MM_ACT_CHANGE_MULTI,
    MM_ACT_CHANGE_NTH,
    MM_ACT_CHANGE_POINTER,
    MM_ACT_IGNORE,
    MM_ACT_PREV_HISTORY,
    MM_ACT_NEXT_HISTORY,
    MM_ACT_PRINT,
    MM_ACT_BECOME,
    MM_ACT_RELOAD,
    MM_ACT_CLEAR_MULTI,
    MM_ACT_HIDE_HEADER,
    MM_ACT_HIDE_PREVIEW,
    MM_ACT_OFFSET_DOWN,
    MM_ACT_OFFSET_MIDDLE,
    MM_ACT_OFFSET_UP,
    MM_ACT_POS,
    MM_ACT_REPLACE_QUERY,
    MM_ACT_TOGGLE_HEADER,
    MM_ACT_TOGGLE_HSCROLL,
    MM_ACT_TOGGLE_PREVIEW,
    MM_ACT_TOGGLE_PREVIEW_WRAP,
    MM_ACT_TOGGLE_PREVIEW_WRAP_WORD,
    MM_ACT_TOGGLE_RAW,
    MM_ACT_DISABLE_RAW,
    MM_ACT_ENABLE_RAW,
    MM_ACT_DISABLE_SEARCH,
    MM_ACT_ENABLE_SEARCH,
    MM_ACT_TOGGLE_SEARCH,
    MM_ACT_TOGGLE_SORT,
    MM_ACT_TOGGLE_TRACK,
    MM_ACT_TOGGLE_TRACK_CURRENT,
    MM_ACT_TOGGLE_WRAP,
    MM_ACT_TOGGLE_WRAP_WORD,
    MM_ACT_TRACK,
    MM_ACT_UNBIND,
    MM_ACT_UNIX_LINE_DISCARD,
    MM_ACT_UNTRACK_CURRENT,
    MM_ACT_WAIT,
    MM_ACT_YANK,
    MM_ACT_CHANGE_PREVIEW,
    MM_ACT_CHANGE_PREVIEW_WINDOW,
    MM_ACT_PREVIEW,
    MM_ACT_PREVIEW_TOP,
    MM_ACT_PREVIEW_BOTTOM,
    MM_ACT_PREVIEW_UP,
    MM_ACT_PREVIEW_DOWN,
    MM_ACT_PREVIEW_PAGE_UP,
    MM_ACT_PREVIEW_PAGE_DOWN,
    MM_ACT_PREVIEW_HALF_PAGE_UP,
    MM_ACT_PREVIEW_HALF_PAGE_DOWN,
    MM_ACT_TRANSFORM,
    MM_ACT_BG_TRANSFORM,
    MM_ACT_BEST,
    MM_ACT_PUT,
    MM_ACT_SELECT,
    MM_ACT_EXCLUDE,
    MM_ACT_EXCLUDE_MULTI,
    MM_ACT_JUMP,
    MM_ACT_JUMP_ACCEPT,
    MM_ACT_BACKWARD_CHAR,
    MM_ACT_FORWARD_CHAR,
    MM_ACT_BEGINNING_OF_LINE,
    MM_ACT_END_OF_LINE,
    MM_ACT_FORWARD_WORD,
    MM_ACT_BACKWARD_WORD,
    MM_ACT_BACKWARD_SUBWORD,
    MM_ACT_BACKWARD_KILL_SUBWORD,
    MM_ACT_FORWARD_SUBWORD,
    MM_ACT_RELOAD_SYNC,
    MM_ACT_REBIND,
    MM_ACT_SHOW_HEADER,
    MM_ACT_SHOW_INPUT,
    MM_ACT_SHOW_PREVIEW,
    MM_ACT_TOGGLE_MULTI_LINE,
    MM_ACT_TOGGLE_INPUT,
    MM_ACT_TOGGLE_IN,
    MM_ACT_TOGGLE_OUT,
    MM_ACT_SELECT_DOWN,
    MM_ACT_DESELECT_UP,
    MM_ACT_BG_CANCEL,
} mm_action_t;

typedef struct {
    mm_event_type_t type;
    wchar_t         rune;    /* only for MM_EV_RUNE bindings */
    mm_action_t     action;
    char           *arg;     /* action argument, malloc'd (may be NULL) */
} mm_binding_t;

typedef struct {
    mm_binding_t *items;
    int           n;
    int           cap;
} mm_keymap_t;

/* Forward declaration for event map (defined below). */
typedef struct mm_event_map_t mm_event_map_t;

/* Look up the actions bound to an event. Returns the number of matches and
   fills *out (up to max) with pointers into the keymap. */
int mm_bind_lookup(const mm_keymap_t *km, const mm_event_t *ev,
                   const mm_binding_t **out, int max);

/* Free all memory owned by a keymap. */
void mm_keymap_free(mm_keymap_t *km);

/* Human readable action name (for diagnostics). */
const char *mm_action_name(mm_action_t a);

/* ---- event bindings (fzf events: start, load, change, focus, result, ...) ---- */

typedef struct {
    const char *name;   /* event name, e.g. "start", "change", "result" */
    mm_action_t action;
    char       *arg;    /* action argument, malloc'd (may be NULL) */
} mm_event_binding_t;

typedef struct mm_event_map_t {
    mm_event_binding_t *items;
    int                 n;
    int                 cap;
} mm_event_map_t;

/* Parse an event binding specification ("event:action(+action)...") and
   append the bindings to the event map. Returns false on a parse error and
   fills *err with a static message. */
bool mm_event_bind_parse(mm_event_map_t *em, const char *spec, const char **err);

/* Look up the actions bound to an event name. Returns the number of matches
   and fills *out (up to max) with pointers into the event map. */
int mm_event_lookup(const mm_event_map_t *em, const char *name,
                    const mm_event_binding_t **out, int max);

/* Free all memory owned by an event map. */
void mm_event_map_free(mm_event_map_t *em);

/* Parse a --bind specification ("key:action(+action)...") and append the
   bindings to the keymap. Returns false on a parse error and fills *err with
   a static message. */
bool mm_bind_parse(mm_keymap_t *km, const char *spec, bool replace,
                   const char **err);

/* Parse a --bind specification, routing key bindings to *km and event
   bindings to *em. Returns false on a parse error. */
bool mm_bind_parse_full(mm_keymap_t *km, mm_event_map_t *em, const char *spec,
                        const char **err);
/* Remove runtime key bindings named by a comma-separated key list. */
bool mm_bind_unbind(mm_keymap_t *km, mm_keymap_t *disabled, const char *keys);
bool mm_bind_rebind(mm_keymap_t *km, mm_keymap_t *disabled, const char *keys);

/* Human readable action name (for diagnostics). */
const char *mm_action_name(mm_action_t a);

#endif /* MM_BIND_H */
