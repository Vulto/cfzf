/* item.h - input record: original bytes, decoded text, index, colors. */
#ifndef MMENU_ITEM_H
#define MMENU_ITEM_H

#include "util.h"
#include <stdint.h>
#include <stdbool.h>

typedef struct {
    int32_t start; /* rune offset */
    int32_t end;   /* rune offset (exclusive) */
    int32_t color; /* encoded color pair */
} mm_ansi_offset_t;

typedef struct {
    char *orig;          /* original bytes (may be NULL if == text) */
    mchars_t text;       /* decoded text */
    char *bytes;         /* owned byte buffer backing text */
    int32_t index;       /* ordinal index */
    mm_ansi_offset_t *colors;
    int ncolors;
    uint16_t trim_length;
    bool trim_known;
} mm_item_t;

/* Initialize an item from a line (borrowed bytes pointer; the item does not
   copy). Returns 0 on failure. */
int mm_item_init(mm_item_t *it, char *line, int32_t index);
void mm_item_free(mm_item_t *it);

uint16_t mm_item_trim_length(mm_item_t *it);
const char *mm_item_as_string(mm_item_t *it, bool strip_ansi);

#endif /* MMENU_ITEM_H */
