/* event.c - event type names and key-name parsing.
   Mirrors fzf's parseKeyChords (src/options.go) and the tui.Event model. */
#include "event.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>

static const char *const event_names[] = {
    "rune", "ctrl-a", "ctrl-b", "ctrl-c", "ctrl-d", "ctrl-e", "ctrl-f",
    "ctrl-g", "ctrl-h", "tab", "ctrl-j", "ctrl-k", "ctrl-l", "enter",
    "ctrl-n", "ctrl-o", "ctrl-p", "ctrl-q", "ctrl-r", "ctrl-s", "ctrl-t",
    "ctrl-u", "ctrl-v", "ctrl-w", "ctrl-x", "ctrl-y", "ctrl-z", "esc",
    "ctrl-space", "ctrl-\\", "ctrl-]", "ctrl-^", "ctrl-/",
    "shift-tab", "backspace", "delete", "page-up", "page-down",
    "up", "down", "left", "right", "home", "end", "insert",
    "shift-up", "shift-down", "shift-left", "shift-right",
    "shift-delete", "shift-home", "shift-end",
    "shift-page-up", "shift-page-down",
    "f1", "f2", "f3", "f4", "f5", "f6", "f7", "f8", "f9", "f10", "f11", "f12",
    "alt-backspace",
    "alt-up", "alt-down", "alt-left", "alt-right", "alt-delete",
    "alt-home", "alt-end", "alt-page-up", "alt-page-down",
    "alt-shift-up", "alt-shift-down", "alt-shift-left", "alt-shift-right",
    "alt-shift-delete", "alt-shift-home", "alt-shift-end",
    "alt-shift-page-up", "alt-shift-page-down",
    "ctrl-up", "ctrl-down", "ctrl-left", "ctrl-right", "ctrl-home",
    "ctrl-end", "ctrl-backspace", "ctrl-delete", "ctrl-page-up",
    "ctrl-page-down",
    "alt", "ctrl-alt",
    "ctrl-alt-up", "ctrl-alt-down", "ctrl-alt-left", "ctrl-alt-right",
    "ctrl-alt-home", "ctrl-alt-end", "ctrl-alt-backspace", "ctrl-alt-delete",
    "ctrl-alt-page-up", "ctrl-alt-page-down",
    "ctrl-shift-up", "ctrl-shift-down", "ctrl-shift-left", "ctrl-shift-right",
    "ctrl-shift-home", "ctrl-shift-end", "ctrl-shift-delete",
    "ctrl-shift-page-up", "ctrl-shift-page-down",
    "ctrl-alt-shift-up", "ctrl-alt-shift-down", "ctrl-alt-shift-left",
    "ctrl-alt-shift-right", "ctrl-alt-shift-home", "ctrl-alt-shift-end",
    "ctrl-alt-shift-delete", "ctrl-alt-shift-page-up",
    "ctrl-alt-shift-page-down",
    "mouse", "double-click", "left-click", "right-click", "shift-left-click",
    "shift-right-click", "scroll-up", "scroll-down", "shift-scroll-up",
    "shift-scroll-down", "preview-scroll-up", "preview-scroll-down",
    "invalid", "fatal", "bracketed-paste-begin", "bracketed-paste-end",
    "resize", "change", "backward-eof",
    "start", "load", "focus", "one", "zero", "result", "jump",
    "jump-cancel", "click-header", "click-footer", "multi", "every",
    "result-final",
};

const char *mm_event_name(mm_event_type_t t) {
    size_t n = sizeof(event_names) / sizeof(event_names[0]);
    if ((size_t)t < n) return event_names[t];
    return "invalid";
}

static bool ieq(const char *a, const char *b) {
    while (*a && *b) {
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        a++; b++;
    }
    return *a == *b;
}

static bool starts_with(const char *s, const char *prefix) {
    return strncmp(s, prefix, strlen(prefix)) == 0;
}

static bool is_alpha(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

bool mm_event_from_name(const char *name, mm_event_type_t *out) {
    if (!name || !*name) return false;

    static const struct { const char *name; mm_event_type_t ev; } table[] = {
        { "up", MM_EV_UP }, { "down", MM_EV_DOWN }, { "left", MM_EV_LEFT },
        { "right", MM_EV_RIGHT },
        { "enter", MM_EV_ENTER }, { "return", MM_EV_ENTER },
        { "backspace", MM_EV_BACKSPACE }, { "bspace", MM_EV_BACKSPACE },
        { "bs", MM_EV_BACKSPACE },
        { "ctrl-space", MM_EV_CTRL_SPACE },
        { "ctrl-^", MM_EV_CTRL_CARET }, { "ctrl-6", MM_EV_CTRL_CARET },
        { "ctrl-/", MM_EV_CTRL_SLASH }, { "ctrl-_", MM_EV_CTRL_SLASH },
        { "ctrl-\\", MM_EV_CTRL_BACKSLASH },
        { "ctrl-]", MM_EV_CTRL_RIGHT_BRACKET },
        { "change", MM_EV_CHANGE },
        { "backward-eof", MM_EV_BACKWARD_EOF },
        { "start", MM_EV_START }, { "load", MM_EV_LOAD },
        { "focus", MM_EV_FOCUS }, { "result", MM_EV_RESULT },
        { "result-final", MM_EV_RESULT_FINAL },
        { "resize", MM_EV_RESIZE }, { "one", MM_EV_ONE }, { "zero", MM_EV_ZERO },
        { "jump", MM_EV_JUMP }, { "jump-cancel", MM_EV_JUMP_CANCEL },
        { "click-header", MM_EV_CLICK_HEADER },
        { "click-footer", MM_EV_CLICK_FOOTER },
        { "multi", MM_EV_MULTI },
        { "alt-bs", MM_EV_ALT_BACKSPACE },
        { "alt-bspace", MM_EV_ALT_BACKSPACE },
        { "alt-backspace", MM_EV_ALT_BACKSPACE },
        { "ctrl-bs", MM_EV_CTRL_BACKSPACE },
        { "ctrl-bspace", MM_EV_CTRL_BACKSPACE },
        { "ctrl-backspace", MM_EV_CTRL_BACKSPACE },
        { "ctrl-alt-bs", MM_EV_CTRL_ALT_BACKSPACE },
        { "ctrl-alt-bspace", MM_EV_CTRL_ALT_BACKSPACE },
        { "ctrl-alt-backspace", MM_EV_CTRL_ALT_BACKSPACE },
        { "alt-up", MM_EV_ALT_UP }, { "alt-down", MM_EV_ALT_DOWN },
        { "alt-left", MM_EV_ALT_LEFT }, { "alt-right", MM_EV_ALT_RIGHT },
        { "alt-home", MM_EV_ALT_HOME }, { "alt-end", MM_EV_ALT_END },
        { "alt-delete", MM_EV_ALT_DELETE },
        { "alt-page-up", MM_EV_ALT_PAGE_UP },
        { "alt-page-down", MM_EV_ALT_PAGE_DOWN },
        { "tab", MM_EV_TAB },
        { "btab", MM_EV_SHIFT_TAB }, { "shift-tab", MM_EV_SHIFT_TAB },
        { "esc", MM_EV_ESC },
        { "delete", MM_EV_DELETE }, { "del", MM_EV_DELETE },
        { "home", MM_EV_HOME }, { "end", MM_EV_END },
        { "insert", MM_EV_INSERT },
        { "pgup", MM_EV_PAGE_UP }, { "page-up", MM_EV_PAGE_UP },
        { "pgdn", MM_EV_PAGE_DOWN }, { "page-down", MM_EV_PAGE_DOWN },
        { "alt-shift-up", MM_EV_ALT_SHIFT_UP },
        { "shift-alt-up", MM_EV_ALT_SHIFT_UP },
        { "alt-shift-down", MM_EV_ALT_SHIFT_DOWN },
        { "shift-alt-down", MM_EV_ALT_SHIFT_DOWN },
        { "alt-shift-left", MM_EV_ALT_SHIFT_LEFT },
        { "shift-alt-left", MM_EV_ALT_SHIFT_LEFT },
        { "alt-shift-right", MM_EV_ALT_SHIFT_RIGHT },
        { "shift-alt-right", MM_EV_ALT_SHIFT_RIGHT },
        { "alt-shift-home", MM_EV_ALT_SHIFT_HOME },
        { "shift-alt-home", MM_EV_ALT_SHIFT_HOME },
        { "alt-shift-end", MM_EV_ALT_SHIFT_END },
        { "shift-alt-end", MM_EV_ALT_SHIFT_END },
        { "alt-shift-delete", MM_EV_ALT_SHIFT_DELETE },
        { "shift-alt-delete", MM_EV_ALT_SHIFT_DELETE },
        { "alt-shift-page-up", MM_EV_ALT_SHIFT_PAGE_UP },
        { "shift-alt-page-up", MM_EV_ALT_SHIFT_PAGE_UP },
        { "alt-shift-page-down", MM_EV_ALT_SHIFT_PAGE_DOWN },
        { "shift-alt-page-down", MM_EV_ALT_SHIFT_PAGE_DOWN },
        { "ctrl-up", MM_EV_CTRL_UP }, { "ctrl-down", MM_EV_CTRL_DOWN },
        { "ctrl-right", MM_EV_CTRL_RIGHT }, { "ctrl-left", MM_EV_CTRL_LEFT },
        { "ctrl-home", MM_EV_CTRL_HOME }, { "ctrl-end", MM_EV_CTRL_END },
        { "ctrl-delete", MM_EV_CTRL_DELETE },
        { "ctrl-page-up", MM_EV_CTRL_PAGE_UP },
        { "ctrl-page-down", MM_EV_CTRL_PAGE_DOWN },
        { "ctrl-alt-up", MM_EV_CTRL_ALT_UP },
        { "alt-ctrl-up", MM_EV_CTRL_ALT_UP },
        { "ctrl-alt-down", MM_EV_CTRL_ALT_DOWN },
        { "alt-ctrl-down", MM_EV_CTRL_ALT_DOWN },
        { "ctrl-alt-right", MM_EV_CTRL_ALT_RIGHT },
        { "alt-ctrl-right", MM_EV_CTRL_ALT_RIGHT },
        { "ctrl-alt-left", MM_EV_CTRL_ALT_LEFT },
        { "alt-ctrl-left", MM_EV_CTRL_ALT_LEFT },
        { "ctrl-alt-home", MM_EV_CTRL_ALT_HOME },
        { "alt-ctrl-home", MM_EV_CTRL_ALT_HOME },
        { "ctrl-alt-end", MM_EV_CTRL_ALT_END },
        { "alt-ctrl-end", MM_EV_CTRL_ALT_END },
        { "ctrl-alt-delete", MM_EV_CTRL_ALT_DELETE },
        { "alt-ctrl-delete", MM_EV_CTRL_ALT_DELETE },
        { "ctrl-alt-page-up", MM_EV_CTRL_ALT_PAGE_UP },
        { "alt-ctrl-page-up", MM_EV_CTRL_ALT_PAGE_UP },
        { "ctrl-alt-page-down", MM_EV_CTRL_ALT_PAGE_DOWN },
        { "alt-ctrl-page-down", MM_EV_CTRL_ALT_PAGE_DOWN },
        { "ctrl-shift-up", MM_EV_CTRL_SHIFT_UP },
        { "shift-ctrl-up", MM_EV_CTRL_SHIFT_UP },
        { "ctrl-shift-down", MM_EV_CTRL_SHIFT_DOWN },
        { "shift-ctrl-down", MM_EV_CTRL_SHIFT_DOWN },
        { "ctrl-shift-right", MM_EV_CTRL_SHIFT_RIGHT },
        { "shift-ctrl-right", MM_EV_CTRL_SHIFT_RIGHT },
        { "ctrl-shift-left", MM_EV_CTRL_SHIFT_LEFT },
        { "shift-ctrl-left", MM_EV_CTRL_SHIFT_LEFT },
        { "ctrl-shift-home", MM_EV_CTRL_SHIFT_HOME },
        { "shift-ctrl-home", MM_EV_CTRL_SHIFT_HOME },
        { "ctrl-shift-end", MM_EV_CTRL_SHIFT_END },
        { "shift-ctrl-end", MM_EV_CTRL_SHIFT_END },
        { "ctrl-shift-delete", MM_EV_CTRL_SHIFT_DELETE },
        { "shift-ctrl-delete", MM_EV_CTRL_SHIFT_DELETE },
        { "ctrl-shift-page-up", MM_EV_CTRL_SHIFT_PAGE_UP },
        { "shift-ctrl-page-up", MM_EV_CTRL_SHIFT_PAGE_UP },
        { "ctrl-shift-page-down", MM_EV_CTRL_SHIFT_PAGE_DOWN },
        { "shift-ctrl-page-down", MM_EV_CTRL_SHIFT_PAGE_DOWN },
        { "ctrl-alt-shift-up", MM_EV_CTRL_ALT_SHIFT_UP },
        { "ctrl-alt-shift-down", MM_EV_CTRL_ALT_SHIFT_DOWN },
        { "ctrl-alt-shift-right", MM_EV_CTRL_ALT_SHIFT_RIGHT },
        { "ctrl-alt-shift-left", MM_EV_CTRL_ALT_SHIFT_LEFT },
        { "ctrl-alt-shift-home", MM_EV_CTRL_ALT_SHIFT_HOME },
        { "ctrl-alt-shift-end", MM_EV_CTRL_ALT_SHIFT_END },
        { "ctrl-alt-shift-delete", MM_EV_CTRL_ALT_SHIFT_DELETE },
        { "ctrl-alt-shift-page-up", MM_EV_CTRL_ALT_SHIFT_PAGE_UP },
        { "ctrl-alt-shift-page-down", MM_EV_CTRL_ALT_SHIFT_PAGE_DOWN },
        { "shift-up", MM_EV_SHIFT_UP }, { "shift-down", MM_EV_SHIFT_DOWN },
        { "shift-left", MM_EV_SHIFT_LEFT }, { "shift-right", MM_EV_SHIFT_RIGHT },
        { "shift-home", MM_EV_SHIFT_HOME }, { "shift-end", MM_EV_SHIFT_END },
        { "shift-delete", MM_EV_SHIFT_DELETE },
        { "shift-page-up", MM_EV_SHIFT_PAGE_UP },
        { "shift-page-down", MM_EV_SHIFT_PAGE_DOWN },
        { "left-click", MM_EV_LEFT_CLICK },
        { "right-click", MM_EV_RIGHT_CLICK },
        { "shift-left-click", MM_EV_SLEFT_CLICK },
        { "shift-right-click", MM_EV_SRIGHT_CLICK },
        { "double-click", MM_EV_DOUBLE_CLICK },
        { "scroll-up", MM_EV_SCROLL_UP },
        { "scroll-down", MM_EV_SCROLL_DOWN },
        { "shift-scroll-up", MM_EV_SSCROLL_UP },
        { "shift-scroll-down", MM_EV_SSCROLL_DOWN },
        { "preview-scroll-up", MM_EV_PREVIEW_SCROLL_UP },
        { "preview-scroll-down", MM_EV_PREVIEW_SCROLL_DOWN },
        { "f10", MM_EV_F10 }, { "f11", MM_EV_F11 }, { "f12", MM_EV_F12 },
    };

    for (size_t i = 0; i < sizeof(table) / sizeof(table[0]); i++) {
        if (ieq(name, table[i].name)) { *out = table[i].ev; return true; }
    }

    /* every(...) is handled by the caller; it carries a millisecond payload. */
    if (ieq(name, "every")) { *out = MM_EV_EVERY; return true; }

    /* ctrl-<letter> */
    if (strlen(name) == 6 && starts_with(name, "ctrl-") && is_alpha(name[5])) {
        char c = (char)tolower((unsigned char)name[5]);
        if (c == 'h') { *out = MM_EV_CTRL_BACKSPACE; return true; }
        *out = (mm_event_type_t)(MM_EV_CTRL_A + (c - 'a'));
        return true;
    }

    /* ctrl-alt-<letter> */
    if (strlen(name) == 10 && starts_with(name, "ctrl-alt-") && is_alpha(name[9])) {
        char c = (char)tolower((unsigned char)name[9]);
        if (c == 'h') { *out = MM_EV_CTRL_ALT_BACKSPACE; return true; }
        *out = (mm_event_type_t)(MM_EV_CTRL_ALT_UP + 0); /* resolved by caller */
        /* Map ctrl-alt-<letter> to the Alt event carrying the rune. */
        *out = MM_EV_CTRL_ALT;
        return true;
    }

    /* f1..f9 */
    if (strlen(name) == 2 && name[0] == 'f' && name[1] >= '1' && name[1] <= '9') {
        *out = (mm_event_type_t)(MM_EV_F1 + (name[1] - '1'));
        return true;
    }

    /* A single character is a rune event. */
    if (strlen(name) == 1) {
        *out = MM_EV_RUNE;
        return true;
    }

    /* alt-<rune> and ctrl-alt-<rune> variants that carry a rune: the caller
       distinguishes them by inspecting the name. */
    if (starts_with(name, "alt-") && strlen(name) > 4) {
        *out = MM_EV_ALT;
        return true;
    }

    return false;
}
