/* item.c - see item.h */
#include "item.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

int mm_item_init(mm_item_t *it, char *line, int32_t index) {
    memset(it, 0, sizeof(*it));
    it->orig = line;
    it->index = index;
    if (!mchars_init(&it->text, line)) return 0;
    return 1;
}

void mm_item_free(mm_item_t *it) {
    mchars_free(&it->text);
    free(it->colors);
    it->colors = NULL;
}

uint16_t mm_item_trim_length(mm_item_t *it) {
    if (it->trim_known) return it->trim_length;
    it->trim_known = true;
    if (!mchars_decode(&it->text)) {
        it->trim_length = 0;
        return 0;
    }
    int len = it->text.length;
    int i;
    for (i = len - 1; i >= 0; i--) {
        wchar_t c = it->text.runes[i];
        if (!iswspace((wint_t)c)) break;
    }
    if (i < 0) { it->trim_length = 0; return 0; }
    int j;
    for (j = 0; j < len; j++) {
        wchar_t c = it->text.runes[j];
        if (!iswspace((wint_t)c)) break;
    }
    it->trim_length = (uint16_t)(i - j + 1);
    return it->trim_length;
}

const char *mm_item_as_string(mm_item_t *it, bool strip_ansi) {
    (void)strip_ansi;
    return it->orig;
}
