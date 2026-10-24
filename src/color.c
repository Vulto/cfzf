/* color.c - --color parsing and the default theme. */
#include "color.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

void mm_theme_init(mm_theme_t *t) {
    memset(t, 0, sizeof(*t));
    mm_color_attr_t def = { MM_COL_DEFAULT, MM_ATTR_UNDEFINED };
    t->fg = def; t->bg = def; t->list_fg = def; t->list_bg = def;
    t->prompt = def; t->pointer = def; t->info = def; t->header = def;
    t->footer = def; t->border = def; t->gutter = def; t->spinner = def;
    t->marker = def; t->current = def; t->current_match = def;
    t->match = def; t->selected = def; t->selected_fg = def;
    t->selected_bg = def; t->preview_fg = def; t->preview_bg = def;
    t->query = def; t->disabled = def; t->separator = def; t->scrollbar = def;
    t->header_fg = def; t->header_bg = def; t->footer_fg = def;
    t->footer_bg = def; t->input_fg = def; t->input_bg = def;

    /* fzf's Dark256 theme (src/tui/tui.go). Entries that fzf leaves
       undefined keep the terminal default. */
    t->dark_bg       = (mm_color_attr_t){ 236, MM_ATTR_UNDEFINED };
    t->prompt        = (mm_color_attr_t){ 110, MM_ATTR_UNDEFINED };
    t->match         = (mm_color_attr_t){ 108, MM_ATTR_UNDEFINED };
    t->current       = (mm_color_attr_t){ 254, MM_ATTR_UNDEFINED };
    t->current_match = (mm_color_attr_t){ 151, MM_ATTR_UNDEFINED };
    t->spinner       = (mm_color_attr_t){ 148, MM_ATTR_UNDEFINED };
    t->info          = (mm_color_attr_t){ 144, MM_ATTR_UNDEFINED };
    t->pointer       = (mm_color_attr_t){ 161, MM_ATTR_UNDEFINED };
    t->marker        = (mm_color_attr_t){ 168, MM_ATTR_UNDEFINED };
    t->header        = (mm_color_attr_t){ 109, MM_ATTR_UNDEFINED };
    t->footer        = (mm_color_attr_t){ 109, MM_ATTR_UNDEFINED };
    t->border        = (mm_color_attr_t){ 59, MM_ATTR_UNDEFINED };
    t->border_label  = (mm_color_attr_t){ MM_COL_UNDEFINED, MM_ATTR_UNDEFINED };
}

static const struct { const char *name; mm_color_t color; } named_colors[] = {
    { "black", MM_COL_BLACK }, { "red", MM_COL_RED }, { "green", MM_COL_GREEN },
    { "yellow", MM_COL_YELLOW }, { "blue", MM_COL_BLUE },
    { "magenta", MM_COL_MAGENTA }, { "cyan", MM_COL_CYAN },
    { "white", MM_COL_WHITE }, { "grey", MM_COL_GREY },
    { "gray", MM_COL_GREY },
    { "bright-red", MM_COL_BRIGHT_RED }, { "bright-green", MM_COL_BRIGHT_GREEN },
    { "bright-yellow", MM_COL_BRIGHT_YELLOW },
    { "bright-blue", MM_COL_BRIGHT_BLUE },
    { "bright-magenta", MM_COL_BRIGHT_MAGENTA },
    { "bright-cyan", MM_COL_BRIGHT_CYAN },
    { "bright-white", MM_COL_BRIGHT_WHITE },
};

bool mm_color_parse(const char *spec, mm_color_t *out) {
    if (!spec || !*spec) return false;
    if (strcmp(spec, "default") == 0) { *out = MM_COL_DEFAULT; return true; }
    if (strcmp(spec, "none") == 0) { *out = MM_COL_DEFAULT; return true; }
    if (spec[0] == '#') {
        unsigned r, g, b;
        if (sscanf(spec + 1, "%2x%2x%2x", &r, &g, &b) != 3) return false;
        if (r > 255 || g > 255 || b > 255) return false;
        *out = (mm_color_t)((1 << 24) | (r << 16) | (g << 8) | b);
        return true;
    }
    for (size_t i = 0; i < sizeof(named_colors) / sizeof(named_colors[0]); i++) {
        if (strcmp(named_colors[i].name, spec) == 0) {
            *out = named_colors[i].color;
            return true;
        }
    }
    /* 0-255 */
    if (isdigit((unsigned char)spec[0])) {
        char *end = NULL;
        long v = strtol(spec, &end, 10);
        if (!end || *end || v < 0 || v > 255) return false;
        *out = (mm_color_t)v;
        return true;
    }
    return false;
}

static mm_color_attr_t *theme_entry(mm_theme_t *t, const char *name,
                                    mm_attr_t extra_attr) {
    struct { const char *name; size_t off; } table[] = {
        { "fg", offsetof(mm_theme_t, fg) },
        { "bg", offsetof(mm_theme_t, bg) },
        { "list-fg", offsetof(mm_theme_t, list_fg) },
        { "list-bg", offsetof(mm_theme_t, list_bg) },
        { "prompt", offsetof(mm_theme_t, prompt) },
        { "pointer", offsetof(mm_theme_t, pointer) },
        { "info", offsetof(mm_theme_t, info) },
        { "header", offsetof(mm_theme_t, header) },
        { "footer", offsetof(mm_theme_t, footer) },
        { "border", offsetof(mm_theme_t, border) },
        { "label", offsetof(mm_theme_t, border_label) },
        { "gutter", offsetof(mm_theme_t, gutter) },
        { "spinner", offsetof(mm_theme_t, spinner) },
        { "marker", offsetof(mm_theme_t, marker) },
        { "current", offsetof(mm_theme_t, current) },
        { "current-match", offsetof(mm_theme_t, current_match) },
        { "match", offsetof(mm_theme_t, match) },
        { "selected", offsetof(mm_theme_t, selected) },
        { "selected-fg", offsetof(mm_theme_t, selected_fg) },
        { "selected-bg", offsetof(mm_theme_t, selected_bg) },
        { "preview-fg", offsetof(mm_theme_t, preview_fg) },
        { "preview-bg", offsetof(mm_theme_t, preview_bg) },
        { "query", offsetof(mm_theme_t, query) },
        { "ghost", offsetof(mm_theme_t, ghost) },
        { "disabled", offsetof(mm_theme_t, disabled) },
        { "separator", offsetof(mm_theme_t, separator) },
        { "scrollbar", offsetof(mm_theme_t, scrollbar) },
        { "header-fg", offsetof(mm_theme_t, header_fg) },
        { "header-bg", offsetof(mm_theme_t, header_bg) },
        { "footer-fg", offsetof(mm_theme_t, footer_fg) },
        { "footer-bg", offsetof(mm_theme_t, footer_bg) },
        { "input-fg", offsetof(mm_theme_t, input_fg) },
        { "input-bg", offsetof(mm_theme_t, input_bg) },
    };
    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (strcmp(table[i].name, name) == 0) {
            mm_color_attr_t *e = (mm_color_attr_t *)((char *)t + table[i].off);
            e->attr |= extra_attr;
            return e;
        }
    }
    return NULL;
}

bool mm_theme_parse(mm_theme_t *t, const char *spec, const char **err) {
    if (!spec) return true;
    const char *p = spec;
    while (*p) {
        /* One entry: "name:color" or "name". */
        const char *colon = strchr(p, ':');
        const char *comma = strchr(p, ',');
        size_t namelen;
        if (!colon) {
            /* fzf requires a value for every entry. */
            *err = "invalid color specification";
            return false;
        }
        if (comma && colon > comma) { *err = "invalid color specification"; return false; }
        namelen = (size_t)(colon - p);
        char name[64];
        if (namelen >= sizeof(name)) namelen = sizeof(name) - 1;
        memcpy(name, p, namelen);
        name[namelen] = 0;

        const char *vstart = colon + 1;
        size_t vlen = comma ? (size_t)(comma - vstart) : strlen(vstart);
        char value[64];
        if (vlen >= sizeof(value)) vlen = sizeof(value) - 1;
        memcpy(value, vstart, vlen);
        value[vlen] = 0;

        mm_attr_t extra = 0;
        char *plus = strchr(value, '+');
        if (plus) {
            *plus = 0;
            if (strcmp(plus + 1, "bold") == 0) extra = MM_BOLD;
            else if (strcmp(plus + 1, "italic") == 0) extra = MM_ITALIC;
            else if (strcmp(plus + 1, "underline") == 0) extra = MM_UNDERLINE;
            else if (strcmp(plus + 1, "dim") == 0) extra = MM_DIM;
            else if (strcmp(plus + 1, "reverse") == 0) extra = MM_REVERSE;
            else if (strcmp(plus + 1, "strikethrough") == 0) extra = MM_STRIKETHROUGH;
            else { *err = "invalid color specification"; return false; }
        }
        mm_color_attr_t *e = theme_entry(t, name, extra);
        if (!e) { *err = "invalid color specification"; return false; }
        mm_color_t c;
        if (!mm_color_parse(value, &c)) { *err = "invalid color specification"; return false; }
        e->color = c;
        p = comma ? comma + 1 : vstart + vlen;
    }
    return true;
}
