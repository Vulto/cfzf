/* color.h - --color parsing and the theme it produces.
   Port of fzf's color option handling (src/options.go parseColor) and the
   default dark theme (src/tui/tui.go Dark256). */
#ifndef MM_COLOR_H
#define MM_COLOR_H

#include "tui.h"

#include <stdbool.h>
#include <stddef.h>

/* The theme entries that --color can override. */
typedef struct {
    mm_color_attr_t fg, bg, list_fg, list_bg, prompt, pointer, info, header,
        footer, border, gutter, spinner, marker, current, current_match,
        match, selected, selected_fg, selected_bg, preview_fg, preview_bg,
        query, ghost, disabled, separator, scrollbar, header_fg, header_bg,
        footer_fg, footer_bg, input_fg, input_bg, dark_bg, border_label;
} mm_theme_t;

/* Initialize the theme with fzf's default 256-color dark theme. */
void mm_theme_init(mm_theme_t *t);

/* Apply a --color specification such as "fg:red,bg:default,pointer:cyan".
   Returns false on a parse error and fills *err with a static message. */
bool mm_theme_parse(mm_theme_t *t, const char *spec, const char **err);

/* Parse a single color value: a name ("red", "bright-blue", "default"), a
   number (0-255) or "#rrggbb". Returns false on error. */
bool mm_color_parse(const char *spec, mm_color_t *out);

#endif /* MM_COLOR_H */
