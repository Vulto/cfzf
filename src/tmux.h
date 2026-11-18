/* tmux.h - --tmux / --popup launcher.
   fzf starts a tmux popup or a Zellij floating pane when that environment
   is set and the option is not overridden by a later --height. */
#ifndef MM_TMUX_H
#define MM_TMUX_H

#include <stdbool.h>

typedef enum {
    MM_POPUP_CENTER = 0,
    MM_POPUP_UP,
    MM_POPUP_DOWN,
    MM_POPUP_LEFT,
    MM_POPUP_RIGHT,
} mm_popup_pos_t;

typedef struct {
    bool set;
    int index;                 /* position in the merged option list */
    mm_popup_pos_t position;
    double width;
    bool width_percent;
    double height;
    bool height_percent;
    bool border;               /* border-native */
} mm_popup_opts_t;

/* Parse a --tmux/--popup value. NULL means the defaults (center, 50%).
   On failure the fzf error is written to stderr and the function returns
   false. */
bool mm_popup_parse(const char *arg, int index, mm_popup_opts_t *out);

/* True when every margin component is zero, the way fzf compares a margin
   with defaultMargin before injecting --margin=0,1. An unparsable value
   is not the default. */
bool mm_margin_is_default(const char *spec);

/* Run the popup when TMUX or ZELLIJ says so. Returns false when this
   process should continue as a normal finder. On true, *code is the
   exit status. argc/argv are the original arguments, without options
   taken from FZF_DEFAULT_OPTS. border_shape -1 means the user did not
   set --border. */
bool mm_popup_launch(const mm_popup_opts_t *opt, int height_index,
                     bool margin_default, int border_shape,
                     const char *border_label, const char *with_shell,
                     bool force_tty_in, int argc, char **argv, int *code);

#endif /* MM_TMUX_H */
