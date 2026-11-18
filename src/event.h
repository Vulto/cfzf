/* event.h - input event types and decoding, mirroring fzf's tui.EventType
   (src/tui/tui.go) and the escape-sequence decoder in src/tui/light.go. */
#ifndef MM_EVENT_H
#define MM_EVENT_H

#include <stdbool.h>
#include <stddef.h>
#include <wchar.h>

typedef enum {
    MM_EV_RUNE = 0,
    MM_EV_CTRL_A, MM_EV_CTRL_B, MM_EV_CTRL_C, MM_EV_CTRL_D, MM_EV_CTRL_E,
    MM_EV_CTRL_F, MM_EV_CTRL_G, MM_EV_CTRL_H, MM_EV_TAB, MM_EV_CTRL_J,
    MM_EV_CTRL_K, MM_EV_CTRL_L, MM_EV_ENTER, MM_EV_CTRL_N, MM_EV_CTRL_O,
    MM_EV_CTRL_P, MM_EV_CTRL_Q, MM_EV_CTRL_R, MM_EV_CTRL_S, MM_EV_CTRL_T,
    MM_EV_CTRL_U, MM_EV_CTRL_V, MM_EV_CTRL_W, MM_EV_CTRL_X, MM_EV_CTRL_Y,
    MM_EV_CTRL_Z, MM_EV_ESC, MM_EV_CTRL_SPACE,
    MM_EV_CTRL_BACKSLASH, MM_EV_CTRL_RIGHT_BRACKET, MM_EV_CTRL_CARET,
    MM_EV_CTRL_SLASH,
    MM_EV_SHIFT_TAB,
    MM_EV_BACKSPACE,
    MM_EV_DELETE,
    MM_EV_PAGE_UP, MM_EV_PAGE_DOWN,
    MM_EV_UP, MM_EV_DOWN, MM_EV_LEFT, MM_EV_RIGHT, MM_EV_HOME, MM_EV_END,
    MM_EV_INSERT,
    MM_EV_SHIFT_UP, MM_EV_SHIFT_DOWN, MM_EV_SHIFT_LEFT, MM_EV_SHIFT_RIGHT,
    MM_EV_SHIFT_DELETE, MM_EV_SHIFT_HOME, MM_EV_SHIFT_END,
    MM_EV_SHIFT_PAGE_UP, MM_EV_SHIFT_PAGE_DOWN,
    MM_EV_F1, MM_EV_F2, MM_EV_F3, MM_EV_F4, MM_EV_F5, MM_EV_F6,
    MM_EV_F7, MM_EV_F8, MM_EV_F9, MM_EV_F10, MM_EV_F11, MM_EV_F12,
    MM_EV_ALT_BACKSPACE,
    MM_EV_ALT_UP, MM_EV_ALT_DOWN, MM_EV_ALT_LEFT, MM_EV_ALT_RIGHT,
    MM_EV_ALT_DELETE, MM_EV_ALT_HOME, MM_EV_ALT_END,
    MM_EV_ALT_PAGE_UP, MM_EV_ALT_PAGE_DOWN,
    MM_EV_ALT_SHIFT_UP, MM_EV_ALT_SHIFT_DOWN, MM_EV_ALT_SHIFT_LEFT,
    MM_EV_ALT_SHIFT_RIGHT, MM_EV_ALT_SHIFT_DELETE, MM_EV_ALT_SHIFT_HOME,
    MM_EV_ALT_SHIFT_END, MM_EV_ALT_SHIFT_PAGE_UP, MM_EV_ALT_SHIFT_PAGE_DOWN,
    MM_EV_CTRL_UP, MM_EV_CTRL_DOWN, MM_EV_CTRL_LEFT, MM_EV_CTRL_RIGHT,
    MM_EV_CTRL_HOME, MM_EV_CTRL_END, MM_EV_CTRL_BACKSPACE, MM_EV_CTRL_DELETE,
    MM_EV_CTRL_PAGE_UP, MM_EV_CTRL_PAGE_DOWN,
    MM_EV_ALT, MM_EV_CTRL_ALT,
    MM_EV_CTRL_ALT_UP, MM_EV_CTRL_ALT_DOWN, MM_EV_CTRL_ALT_LEFT,
    MM_EV_CTRL_ALT_RIGHT, MM_EV_CTRL_ALT_HOME, MM_EV_CTRL_ALT_END,
    MM_EV_CTRL_ALT_BACKSPACE, MM_EV_CTRL_ALT_DELETE,
    MM_EV_CTRL_ALT_PAGE_UP, MM_EV_CTRL_ALT_PAGE_DOWN,
    MM_EV_CTRL_SHIFT_UP, MM_EV_CTRL_SHIFT_DOWN, MM_EV_CTRL_SHIFT_LEFT,
    MM_EV_CTRL_SHIFT_RIGHT, MM_EV_CTRL_SHIFT_HOME, MM_EV_CTRL_SHIFT_END,
    MM_EV_CTRL_SHIFT_DELETE, MM_EV_CTRL_SHIFT_PAGE_UP, MM_EV_CTRL_SHIFT_PAGE_DOWN,
    MM_EV_CTRL_ALT_SHIFT_UP, MM_EV_CTRL_ALT_SHIFT_DOWN,
    MM_EV_CTRL_ALT_SHIFT_LEFT, MM_EV_CTRL_ALT_SHIFT_RIGHT,
    MM_EV_CTRL_ALT_SHIFT_HOME, MM_EV_CTRL_ALT_SHIFT_END,
    MM_EV_CTRL_ALT_SHIFT_DELETE, MM_EV_CTRL_ALT_SHIFT_PAGE_UP,
    MM_EV_CTRL_ALT_SHIFT_PAGE_DOWN,
    MM_EV_MOUSE, MM_EV_DOUBLE_CLICK, MM_EV_LEFT_CLICK, MM_EV_RIGHT_CLICK,
    MM_EV_SLEFT_CLICK, MM_EV_SRIGHT_CLICK,
    MM_EV_SCROLL_UP, MM_EV_SCROLL_DOWN, MM_EV_SSCROLL_UP, MM_EV_SSCROLL_DOWN,
    MM_EV_PREVIEW_SCROLL_UP, MM_EV_PREVIEW_SCROLL_DOWN,

    /* Synthetic / non-user events. */
    MM_EV_INVALID, MM_EV_FATAL,
    MM_EV_BRACKETED_PASTE_BEGIN, MM_EV_BRACKETED_PASTE_END,
    MM_EV_RESIZE, MM_EV_CHANGE, MM_EV_BACKWARD_EOF,
    MM_EV_START, MM_EV_LOAD, MM_EV_FOCUS, MM_EV_ONE, MM_EV_ZERO,
    MM_EV_RESULT, MM_EV_JUMP, MM_EV_JUMP_CANCEL,
    MM_EV_CLICK_HEADER, MM_EV_CLICK_FOOTER,
    MM_EV_MULTI, MM_EV_EVERY, MM_EV_RESULT_FINAL,
} mm_event_type_t;

typedef struct {
    mm_event_type_t type;
    wchar_t         rune;   /* for MM_EV_RUNE */
    int             x, y;   /* mouse coordinates, 0-based */
    bool            mouse_down;
    int             mouse_scroll; /* 1 up, -1 down, 0 otherwise */
} mm_event_t;

/* Human readable name of an event type, as used by --bind. Returns a
   static string. */
const char *mm_event_name(mm_event_type_t t);

/* Parse a key name such as "ctrl-a", "alt-b", "f1", "enter", "up" into an
   event type. Returns true on success. */
bool mm_event_from_name(const char *name, mm_event_type_t *out);

/* ---- input buffer & decoding ---- */

typedef struct {
    char  *buf;
    size_t len;
    size_t cap;
} mm_keybuf_t;

void mm_keybuf_init(mm_keybuf_t *in);
void mm_keybuf_free(mm_keybuf_t *in);

/* Read the next event from fd. Returns false on a fatal read error. */
bool mm_read_event(int fd, mm_keybuf_t *in, mm_event_t *ev, bool cancellable);

#endif /* MM_EVENT_H */
