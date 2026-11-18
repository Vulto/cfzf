/* ui.h - interactive fuzzy finder loop. */
#ifndef MM_UI_H
#define MM_UI_H

#include "filter.h"
#include "item.h"
#include "pattern.h"
#include "result.h"
#include "tui.h"
#include "terminal.h"
#include "bind.h"
#include "color.h"
#include "preview.h"
#include "history.h"

#include <stdbool.h>
#include <sys/types.h>

#define MM_UI_QCACHE 4
typedef struct {
    char *query;
    int   item_count;
    int   n;
    int  *idx;
} mm_qcache_slot_t;

typedef struct {
    /* Input */
    char      **lines;
    int        count;

    /* Search configuration */
    mm_sort_criteria_t crit;
    mm_delimiter_t    delim;
    const char       *accept_nth_spec; /* --accept-nth output expression */
    const char       *id_nth_spec; /* --id-nth identity expression */
    mm_range_t       *nth;
    int              nth_n;
    mm_range_t       *with_nth;
    int              with_nth_n;
    bool             extended;
    bool             fuzzy;
    mm_case_t        case_mode;
    bool             tac;
    bool             no_sort;
    bool             multi;
    int              multi_max;
    bool             reverse;
    bool             cycle;
    const char      *prompt;
    const char      *query;
    const char      *header;
    const char      *saved_header; /* last configured header while hidden */
    bool              header_hidden;
    const char      *header_lines; /* --header-lines: the first N input lines */
    char            *header_lines_buf; /* owned text of header_lines */
    int              header_line_count; /* items hidden as header lines */
    const char      *footer;     /* --footer: the text at the top of the list */
    const char      *footer_label; /* label over the footer divider */
    int              footer_label_pos; /* 0 = centered */
    bool             footer_label_bottom;
    int              footer_border; /* --footer-border; line when unset */
    bool             footer_border_set;
    int              header_lines_border;
    bool             header_lines_border_set;
    bool             separator_default; /* true until --separator / --no-separator */
    bool             info;
    bool             info_inline;       /* --info=inline: count shares the prompt line */
    bool             info_inline_right; /* --info=inline-right */
    const char      *info_prefix;      /* --info=inline:PREFIX; NULL is " < " */
    bool             print_query;
    bool             expect;
    bool             read0;
    bool             print0;
    bool             select_1;
    bool             exit_0;
    bool             ansi;
    bool             literal;
    /* --expect: keys that accept the current selection and are printed
       before it. */
    char           **expect_keys;
    int              expect_n;

    /* Runtime */
    mm_item_t   *items;
    int         *results;   /* indices into items, in display order */
    int          nresults;
    int          results_cap; /* allocated entries in results */
    /* results[i] == i for every i, so a live empty query can append. */
    bool         plain_results;
    bool         has_excluded; /* an exclude action hid at least one item */
    int          cursor;
    char        *pending_track_key; /* identity captured before reload */
    int          offset;    /* first visible row */
    char        *query_buf; /* current query text */
    char        *search_override; /* search action: match text, display stays query_buf */
    char        *yank_buf;   /* last killed text, inserted by yank */
    bool         stop_chain; /* hide-input/show-input: drop the rest of this chain */
    int          trigger_depth;
    int          defer_search; /* >0 while an action chain is running */
    bool         search_dirty; /* a deferred action asked for a new search */
    char        *last_search_query; /* owned snapshot used by change events */
    size_t       query_len, query_cap;
    bool        *selected;
    bool        *excluded; /* items hidden by exclude actions until reload */
    int          nselected;
    /* The items can be read while the program is running, so that a live
       stream shows up immediately (fzf reads the input in the background). */
    int          input_fd;      /* the fd the items are read from (-1: none) */
    char         input_sep;     /* the separator of the input (" or \0) */
    bool         input_done;    /* the input reached its end */
    char        *pending;       /* the incomplete line of the stream */
    size_t       pending_len;
    size_t       pending_cap;
    int          lines_cap;     /* the capacity of the lines array */
    /* Streamed lines are packed into slabs. NULL means each lines[i] was
       malloc'd on its own and must be freed on its own. */
    void        *line_slabs;
    bool         lines_pooled;
    /* The selected items in the order they were selected (fzf prints them
       in that order). */
    int         *sel_order;
    int          sel_order_n;
    int          sel_order_cap;
    int          exit_code;
    const char  *expected;   /* --expect key that accepted the selection */
    mm_keymap_t  keymap;     /* --bind specifications */
    mm_keymap_t  disabled_keymap; /* keys temporarily removed by unbind */
    mm_event_map_t events;   /* --bind event bindings */
    bool result_events_initialized; /* final result-count events were emitted */
    int jump_mode; /* 0 disabled, 1 jump, 2 jump-and-accept */
    int jump_label_count; /* visible jump labels in the current layout */
    mm_preview_t *preview;   /* preview runner (NULL when disabled) */
    pid_t        bg_transform_pid;
    int          bg_transform_fd;
    mm_action_t  bg_transform_target; /* field to apply, or BG_TRANSFORM for a chain */
    bool         bg_transform_first_line;
    char        *bg_transform_output;
    size_t       bg_transform_len, bg_transform_cap;
    size_t       bg_transform_placeholder_start;
    bool         preview_hidden; /* visibility toggled without destroying command */
    int          preview_offset; /* first preview output line to display */
    int          preview_page_size; /* visible preview content rows */
    int          preview_item; /* result index the running preview was started for */
    char        *preview_query; /* query that preview was started with */
    bool         preview_fresh; /* the command was just started and not drained */
    int          preview_layout; /* last preview geometry painted, for erasing */
    mm_match_pos_t *match_pos;  /* the matched offsets of every result */
    int             match_pos_n;
    /* Lazy highlights cover the visible window only. Index 0 is the result
       at window_pos_from. A full match_pos array is the non-lazy path. */
    mm_match_pos_t *window_pos;
    int             window_pos_n;
    int             window_pos_from;
    mm_preview_win_t preview_win;
    mm_preview_win_t preview_win_initial; /* --preview-window, before rotations */
    mm_theme_t   theme;      /* --color theme */
    int          border;     /* --border shape id */
    int          preview_border; /* --preview-border shape id */
    const char  *preview_label; /* --preview-label */
    const char  *preview_wrap_sign; /* --preview-wrap-sign */
    int          preview_label_pos; /* 0 = center, otherwise 1-based/negative offset */
    bool         preview_label_bottom;
    int          list_border; /* --list-border shape id; -1 inherits style */
    int          header_border; /* --header-border shape id */
    int          input_border; /* --input-border shape id */
    bool         input_border_set;
    const char  *input_label; /* --input-label */
    int          input_label_pos;
    bool         input_label_bottom;
    bool         header_border_set;
    bool         list_border_set;
    bool         style_full; /* --style=full component layout */
    const char  *pointer;    /* --pointer: the current line marker */
    const char  *gutter;
    const char  *gutter_raw;     /* --gutter: the marker of the other lines */
    const char  *marker;     /* --marker: the multi-select marker */
    const char  *shell;      /* --with-shell: the shell used by execute */
    const char  *proxy_script; /* --proxy-script: become writes here and exits 126 */
    char         *become_command; /* expanded command that replaces this process */
    char        **runtime_strings; /* owned strings installed by dynamic actions */
    size_t        runtime_strings_n, runtime_strings_cap;
    char        **placeholder_files; /* temporary files used by shell placeholders */
    size_t        placeholder_files_n, placeholder_files_cap;
    mm_history_t *history;   /* --history: the query history file */
    const char  *ghost;       /* --ghost: the prompt placeholder */
    int          height;      /* --height: the inline window height (0 = full screen) */
    const char  *height_spec; /* --height: the raw specification */
    int          layout;     /* 0 = default, 1 = reverse-list (top down) */
    bool         no_bold;    /* --no-bold */
    bool         no_color;   /* --no-color or NO_COLOR with no later --color */
    bool         mouse;      /* mouse reporting, off with --no-mouse */
    bool         plain_theme; /* NO_COLOR / --no-color base, then a --color spec */
    int          min_height; /* --min-height: the minimum inline window height */
    const char  *ellipsis;   /* --ellipsis: the truncation marker */
    int          gap;        /* --gap: blank lines between the list items */
    const char  *gap_line;   /* --gap-line: the line drawn in the gap */
    int          padding;    /* --padding: columns of padding around the list */
    int          margin;     /* --margin: columns of margin around the list */
    int          scroll_off; /* --scroll-off: lines of context above/below the cursor */
    int          hscroll_off;/* --hscroll-off: columns of context left/right of the cursor */
    int          freeze_left; /* --freeze-left: fields kept visible on the left */
    int          freeze_right; /* --freeze-right: fields kept visible on the right */
    const char  *separator;  /* --separator: the line drawn between list items */
    bool         no_scrollbar;/* --no-scrollbar: hide the scrollbar column */
    bool         no_clear;    /* --no-clear: do not clear the screen on exit */
    int          tabstop;     /* --tabstop: tab stop width in columns */
    bool         no_input;    /* --no-input: do not read any input */
    bool         no_hscroll;  /* --no-hscroll: disable horizontal scrolling */
    bool         no_multi_line;/* --no-multi-line: disable multi-line display */
    bool         no_tty_default;/* --no-tty-default: don't default to TTY */
    bool         raw;         /* --raw: display non-matching items in place */
    bool         no_unicode; /* --no-unicode: use the ASCII glyphs */
    bool         phony;      /* --phony: do not filter with the query */
    bool         search_enabled; /* --disabled / enable-search action state */
    bool         wrap;       /* --wrap: wrap the long items */
    bool         wrap_word;  /* --wrap=word: break the lines at the spaces */
    const char  *wrap_sign;  /* --wrap-sign: the marker of a wrapped line */
    const char  *list_label; /* --list-label */
    int          list_label_pos;
    bool         list_label_bottom;
    const char  *header_label; /* --header-label */
    int          header_label_pos;
    bool         header_label_bottom;
    const char  *border_label;
    int          border_label_pos;     /* --border-label-pos: 0 = center */
    bool         border_label_bottom;  /* --border-label-pos=...:bottom */
    bool         highlight_line; /* --highlight-line */
    const char  *info_command;   /* --info-command */
    const char  *jump_labels;    /* --jump-labels */
    const char  *marker_multi_line; /* --marker-multi-line */
    int          threads;        /* --threads */
    bool         track;          /* --track: global cursor tracking */
    bool         track_current;  /* one-shot tracking of the current item */
    bool         ambidouble;     /* --ambidouble */
    bool         filepath_word;  /* --filepath-word */
    bool         header_first;   /* --header-first */
    const char  *listen_spec;    /* --listen */
    const char  *listen_unsafe_spec; /* --listen-unsafe */
    int          query_pos;  /* cursor position inside the query */
    int          page_size;  /* number of visible rows, for page actions */
    mm_renderer_t *renderer; /* set while the loop runs */
    bool         aborted;    /* an abort action ended the loop */
    int          search_count; /* number of items already searched in streaming mode */
    int         *search_results; /* unused; the query cache replaced this */
    /* ui->count at the time results was built. A longer query may rescan
       only that list and the items appended after it. */
    int          results_item_count;
    /* Highlights are filled for the visible rows, not for every match. */
    bool         lazy_highlight;
    /* A streamed line needed a rune slab. ASCII lists skip that pass. */
    bool         has_non_ascii;
    void        *rune_slabs; /* decoded non-ASCII runes, borrowed by items */
    int          runes_ready; /* items [0, runes_ready) have been considered */
    mm_qcache_slot_t qcache[MM_UI_QCACHE];
    int          qcache_next;
} mm_ui_t;

/* Evaluate a --height specification; 0 means the full screen. */
int mm_ui_height(const char *spec, int rows, int min_height);

/* Initialize the UI from a list of input lines. */
bool mm_ui_init(mm_ui_t *ui, char **lines, int count, int cap);

/* Read the items that are available on the input and append them to the
   list.  Returns true when new items were added. */
bool mm_ui_read_items(mm_ui_t *ui, int ttyin);

/* Run the interactive loop. Returns the process exit code. */
int mm_ui_run(mm_ui_t *ui);

/* Free all resources owned by the UI. */
void mm_ui_free(mm_ui_t *ui);

#endif /* MM_UI_H */
