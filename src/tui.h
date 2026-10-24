/* tui.h - terminal UI types shared by the C23 fzf port.
   Mirrors the parts of fzf's src/tui/tui.go that the core needs (Color, Attr,
   ColorAttr). Rendering/layout live in terminal.c. */
#ifndef MM_TUI_H
#define MM_TUI_H

#include <stdint.h>
#include <stdbool.h>

typedef int32_t mm_color_t;

enum {
    MM_COL_UNDEFINED = -2,
    MM_COL_DEFAULT   = -1,
    MM_COL_BLACK = 0, MM_COL_RED, MM_COL_GREEN, MM_COL_YELLOW,
    MM_COL_BLUE, MM_COL_MAGENTA, MM_COL_CYAN, MM_COL_WHITE,
    MM_COL_GREY,
    MM_COL_BRIGHT_RED, MM_COL_BRIGHT_GREEN, MM_COL_BRIGHT_YELLOW,
    MM_COL_BRIGHT_BLUE, MM_COL_BRIGHT_MAGENTA, MM_COL_BRIGHT_CYAN,
    MM_COL_BRIGHT_WHITE,
};

typedef int32_t mm_attr_t;

enum {
    MM_ATTR_UNDEFINED = 0,
    MM_ATTR_REGULAR   = 1 << 8,
    MM_ATTR_CLEAR     = 1 << 9,
    MM_BOLDFORCE      = 1 << 10,
    MM_FULLBG         = 1 << 11,
    MM_STRIP          = 1 << 12,

    /* Underline style in bits 13-15. */
    MM_ULSTYLE_SHIFT = 13,
    MM_ULSTYLE_MASK  = 0b111 << MM_ULSTYLE_SHIFT,
    MM_ULSTYLE_DOUBLE = 0b001 << MM_ULSTYLE_SHIFT,
    MM_ULSTYLE_CURLY  = 0b010 << MM_ULSTYLE_SHIFT,
    MM_ULSTYLE_DOTTED = 0b011 << MM_ULSTYLE_SHIFT,
    MM_ULSTYLE_DASHED = 0b100 << MM_ULSTYLE_SHIFT,
};

/* Basic attribute bits. These match the non-tcell (dummy) build of fzf's
   src/tui/dummy.go, which is the default pure-Go renderer. */
enum {
    MM_BOLD          = 1,
    MM_DIM           = 1 << 1,
    MM_ITALIC        = 1 << 2,
    MM_UNDERLINE     = 1 << 3,
    MM_BLINK         = 1 << 4,
    MM_BLINK2        = 1 << 5,
    MM_REVERSE       = 1 << 6,
    MM_STRIKETHROUGH = 1 << 7,
};

typedef struct {
    mm_color_t color;
    mm_attr_t  attr;
} mm_color_attr_t;

static inline mm_attr_t mm_attr_underline_style(mm_attr_t a) {
    return a & MM_ULSTYLE_MASK;
}

static inline mm_attr_t mm_attr_merge(mm_attr_t a, mm_attr_t b) {
    if (b & MM_ATTR_REGULAR) {
        return (b & ~MM_ATTR_REGULAR) | (a & MM_BOLDFORCE);
    }
    mm_attr_t merged = (a & ~MM_ATTR_REGULAR) | b;
    if (b & MM_UNDERLINE) {
        merged = (merged & ~MM_ULSTYLE_MASK) | (b & MM_ULSTYLE_MASK);
    }
    return merged;
}

static inline bool mm_color_attr_is_undefined(mm_color_attr_t a) {
    return a.color == MM_COL_UNDEFINED &&
           (a.attr & ~MM_BOLDFORCE) == MM_ATTR_UNDEFINED;
}

#endif /* MM_TUI_H */
