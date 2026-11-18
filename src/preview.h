/* preview.h - preview window support (fzf's --preview / --preview-window).
   Port of the behaviour of fzf's previewOpts and its previewer. */
#ifndef MM_PREVIEW_H
#define MM_PREVIEW_H

#include <stdbool.h>
#include <stddef.h>
#include <sys/types.h>

typedef enum {
    MM_PREVIEW_RIGHT,
    MM_PREVIEW_LEFT,
    MM_PREVIEW_TOP,
    MM_PREVIEW_BOTTOM,
} mm_preview_pos_t;

typedef struct {
    mm_preview_pos_t pos;
    int  size;        /* percent, or an absolute cell count when size_percent is false */
    bool size_percent;
    /* -1 keeps --preview-border. Any other value is a border shape from
       --preview-window, including MM_BORDER_NONE. */
    int  border_shape;
    bool hidden;      /* --preview-window=hidden */
    bool info;        /* show the info line in the preview */
    bool wrap;        /* wrap the preview text instead of clipping it */
    bool wrap_word;   /* break wrapped preview lines at word boundaries */
    bool cycle;       /* cycle preview scrolling */
} mm_preview_win_t;

/* Factory defaults: right, 50%, full border, visible. */
void mm_preview_window_reset(mm_preview_win_t *out);

/* Apply one fzf preview-window spec on top of the current options.
   An empty spec changes nothing. Returns false on an unknown token. */
bool mm_preview_window_apply(const char *spec, mm_preview_win_t *out);

/* Reset, then apply. Colon-separated size syntax is accepted too. */
bool mm_preview_window_parse(const char *spec, mm_preview_win_t *out);

typedef struct mm_preview_t mm_preview_t;

/* Create a preview runner.  `command` is the command template and `shell`
   the shell to run it with (NULL means /bin/sh). */
mm_preview_t *mm_preview_new(const char *command, const char *shell);
void mm_preview_free(mm_preview_t *p);

/* Start the command for the given item and query. The placeholders {} (item),
   {q} (query) and {n} (index) are expanded. */
void mm_preview_start(mm_preview_t *p, const char *item, const char *query, int index);

/* Read whatever the command produced. Returns true when new output arrived
   (and the caller should repaint). */
bool mm_preview_poll(mm_preview_t *p);

/* Wait up to timeout_ms for the command to produce output, then read it.
   A fast command returns as soon as the bytes are ready. */
bool mm_preview_drain(mm_preview_t *p, int timeout_ms);

/* The output collected so far (NUL terminated, may be empty). */
const char *mm_preview_output(mm_preview_t *p);

/* Set selected items in two forms: raw newline-separated records for {f},
   and shell-quoted arguments for {+}. Both strings are copied. */
void mm_preview_set_selected(mm_preview_t *p, const char *selected,
                             const char *selected_shell);

/* Change the preview command template. The string is copied. */
void mm_preview_set_command(mm_preview_t *p, const char *command);

#endif /* MM_PREVIEW_H */
