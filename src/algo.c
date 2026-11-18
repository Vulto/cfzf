/* algo.c - see algo.h. Ported from fzf src/algo/algo.go (v0.74.3). */
#include "algo.h"
#include <immintrin.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static int selected_fuzzy_version = 2;

/* glibc provides memrchr. _POSIX_C_SOURCE hides the declaration. */
extern void *memrchr(const void *s, int c, size_t n);

enum {
    scoreMatch = 16,
    scoreGapStart = -3,
    scoreGapExtension = -1,
    bonusBoundary = scoreMatch / 2,
    bonusNonWord = scoreMatch / 2,
    bonusCamel123 = bonusBoundary + scoreGapExtension,
    bonusConsecutive = -(scoreGapStart + scoreGapExtension),
    bonusFirstCharMultiplier = 2,
};

static int16_t bonusBoundaryWhite;
static int16_t bonusBoundaryDelimiter;
static int initialCharClass;
static const char *delimiterChars = "/,:;|";
static const char *whiteChars = " \t\n\v\f\r\x85\xA0";

typedef enum {
    charWhite = 0,
    charNonWord,
    charDelimiter,
    charLower,
    charUpper,
    charLetter,
    charNumber,
} char_class;

static char_class asciiCharClasses[128];
static int16_t bonusMatrix[charNumber + 1][charNumber + 1];

static int rune_in_set(wchar_t c, const char *set) {
    /* Compare ASCII bytes only to ASCII code points. Comparing the low byte
       misclassifies symbols such as U+222F (∯) as the '/' delimiter. */
    if (c >= 0 && c < 0x80) {
        for (const char *p = set; *p; p++)
            if ((unsigned char)*p == (unsigned char)c) return 1;
    }
    for (const char *p = set; *p; p++) {
        if ((unsigned char)*p >= 0x80) {
            wchar_t cp; size_t used = mm_utf8_decode(p, strlen(p), &cp);
            if (cp == c) return 1;
            p += used - 1;
        }
    }
    return 0;
}

static int is_space_unicode(wchar_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' ||
           c == '\r' || c == 0x85 || c == 0xA0 || c == 0x2028 || c == 0x2029 ||
           c == 0x202F || c == 0x205F || c == 0x3000 || c == 0x1680 ||
           (c >= 0x2000 && c <= 0x200A);
}

static char_class char_class_of_non_ascii(wchar_t c) {
    if (c >= 'a' && c <= 'z') return charLower;
    if (iswlower((wint_t)c)) return charLower;
    if (iswupper((wint_t)c)) return charUpper;
    if (iswdigit((wint_t)c)) return charNumber;
    if (iswalpha((wint_t)c)) return charLetter;
    if (is_space_unicode(c)) return charWhite;
    if (rune_in_set(c, delimiterChars)) return charDelimiter;
    return charNonWord;
}

static char_class char_class_of(wchar_t c) {
    if (c < 128) return asciiCharClasses[c];
    return char_class_of_non_ascii(c);
}

static int16_t bonus_for(char_class prev, char_class cls) {
    if (cls >= charNonWord) {
        switch (prev) {
        case charWhite: return bonusBoundaryWhite;
        case charDelimiter: return bonusBoundaryDelimiter;
        case charNonWord: return bonusBoundary;
        default: break;
        }
    }
    if ((prev == charLower && cls == charUpper) ||
        (prev != charNumber && cls == charNumber))
        return bonusCamel123;
    switch (cls) {
    case charNonWord:
    case charDelimiter: return bonusNonWord;
    case charWhite: return bonusBoundaryWhite;
    default: return 0;
    }
}

static int16_t bonus_at(const wchar_t *text, int idx) {
    if (idx == 0) return bonusBoundaryWhite;
    return bonusMatrix[char_class_of(text[idx - 1])][char_class_of(text[idx])];
}

bool mm_algo_set_version(int version) {
    if (version != 1 && version != 2) return false;
    selected_fuzzy_version = version;
    return true;
}

bool mm_algo_init(const char *scheme) {
    if (!scheme || strcmp(scheme, "default") == 0) {
        delimiterChars = "/,:;|";
        bonusBoundaryWhite = bonusBoundary + 2;
        bonusBoundaryDelimiter = bonusBoundary + 1;
        initialCharClass = charWhite;
    } else if (strcmp(scheme, "path") == 0) {
        bonusBoundaryWhite = bonusBoundary;
        bonusBoundaryDelimiter = bonusBoundary + 1;
        delimiterChars = "/";
        initialCharClass = charDelimiter;
    } else if (strcmp(scheme, "history") == 0) {
        delimiterChars = "/,:;|";
        bonusBoundaryWhite = bonusBoundary;
        bonusBoundaryDelimiter = bonusBoundary;
        initialCharClass = charWhite;
    } else {
        return false;
    }
    for (int i = 0; i < 128; i++) {
        wchar_t ch = (wchar_t)i;
        char_class c = charNonWord;
        if (ch >= 'a' && ch <= 'z') c = charLower;
        else if (ch >= 'A' && ch <= 'Z') c = charUpper;
        else if (ch >= '0' && ch <= '9') c = charNumber;
        else if (rune_in_set(ch, whiteChars)) c = charWhite;
        else if (rune_in_set(ch, delimiterChars)) c = charDelimiter;
        asciiCharClasses[i] = c;
    }
    for (int i = 0; i <= charNumber; i++)
        for (int j = 0; j <= charNumber; j++)
            bonusMatrix[i][j] = bonus_for((char_class)i, (char_class)j);
    return true;
}

wchar_t mm_algo_normalize_rune(wchar_t r) {
    switch (r) {
    case 0x00C0: case 0x00C1: case 0x00C2: case 0x00C3: case 0x00C4: case 0x00C5:
    case 0x00E0: case 0x00E1: case 0x00E2: case 0x00E3: case 0x00E4: case 0x00E5:
    case 0x0100: case 0x0101: case 0x0102: case 0x0103: case 0x0104: case 0x0105:
        return 'a';
    case 0x00C7: case 0x00E7: case 0x0106: case 0x0107: case 0x0108: case 0x0109:
    case 0x010A: case 0x010B: case 0x010C: case 0x010D:
        return 'c';
    case 0x00D0: case 0x00F0: case 0x010E: case 0x010F: case 0x0110: case 0x0111:
        return 'd';
    case 0x00C8: case 0x00C9: case 0x00CA: case 0x00CB:
    case 0x00E8: case 0x00E9: case 0x00EA: case 0x00EB:
    case 0x0112: case 0x0113: case 0x0114: case 0x0115: case 0x0116: case 0x0117:
    case 0x0118: case 0x0119: case 0x011A: case 0x011B:
        return 'e';
    case 0x011C: case 0x011D: case 0x011E: case 0x011F: case 0x0120: case 0x0121:
    case 0x0122: case 0x0123:
        return 'g';
    case 0x0124: case 0x0125: case 0x0126: case 0x0127:
        return 'h';
    case 0x00CC: case 0x00CD: case 0x00CE: case 0x00CF:
    case 0x00EC: case 0x00ED: case 0x00EE: case 0x00EF:
    case 0x0128: case 0x0129: case 0x012A: case 0x012B: case 0x012C: case 0x012D:
    case 0x012E: case 0x012F: case 0x0130:
        return 'i';
    case 0x0134: case 0x0135:
        return 'j';
    case 0x0136: case 0x0137: case 0x0138:
        return 'k';
    case 0x0139: case 0x013A: case 0x013B: case 0x013C: case 0x013D: case 0x013E:
    case 0x013F: case 0x0140: case 0x0141: case 0x0142:
        return 'l';
    case 0x00D1: case 0x00F1: case 0x0143: case 0x0144: case 0x0145: case 0x0146:
    case 0x0147: case 0x0148: case 0x0149:
        return 'n';
    case 0x00D2: case 0x00D3: case 0x00D4: case 0x00D5: case 0x00D6: case 0x00D8:
    case 0x00F2: case 0x00F3: case 0x00F4: case 0x00F5: case 0x00F6: case 0x00F8:
    case 0x014C: case 0x014D: case 0x014E: case 0x014F: case 0x0150: case 0x0151:
        return 'o';
    case 0x0154: case 0x0155: case 0x0156: case 0x0157: case 0x0158: case 0x0159:
        return 'r';
    case 0x015A: case 0x015B: case 0x015C: case 0x015D: case 0x015E: case 0x015F:
    case 0x0160: case 0x0161:
        return 's';
    case 0x0162: case 0x0163: case 0x0164: case 0x0165: case 0x0166: case 0x0167:
        return 't';
    case 0x00D9: case 0x00DA: case 0x00DB: case 0x00DC:
    case 0x00F9: case 0x00FA: case 0x00FB: case 0x00FC:
    case 0x0168: case 0x0169: case 0x016A: case 0x016B: case 0x016C: case 0x016D:
    case 0x016E: case 0x016F: case 0x0170: case 0x0171: case 0x0172: case 0x0173:
        return 'u';
    case 0x0174: case 0x0175:
        return 'w';
    case 0x00DD: case 0x00FD: case 0x00FF: case 0x0176: case 0x0177: case 0x0178:
        return 'y';
    case 0x0179: case 0x017A: case 0x017B: case 0x017C: case 0x017D: case 0x017E:
        return 'z';
    default:
        return r;
    }
}

static wchar_t lower_rune(wchar_t c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return (wchar_t)towlower((wint_t)c);
}

/* leading/trailing whitespace counts */
static int leading_ws(const wchar_t *t, int n) {
    int i = 0;
    while (i < n && is_space_unicode(t[i])) i++;
    return i;
}
static int trailing_ws(const wchar_t *t, int n) {
    int i = 0;
    while (i < n && is_space_unicode(t[n - 1 - i])) i++;
    return i;
}

static int index_at(int idx, int len, bool forward) {
    if (forward) return idx;
    return len - idx - 1;
}

/* calculateScore equivalent */
static int calculate_score(const wchar_t *text, int len, const wchar_t *pattern, int plen,
                           int sidx, int eidx, bool case_sensitive, bool normalize,
                           int *pos, int *npos) {
    (void)len;
    int pidx = 0, score = 0, consecutive = 0;
    int16_t firstBonus = 0;
    bool inGap = false;
    char_class prevClass = initialCharClass;
    if (sidx > 0) prevClass = char_class_of(text[sidx - 1]);
    int np = 0;
    for (int idx = sidx; idx < eidx; idx++) {
        wchar_t ch = text[idx];
        char_class cls = char_class_of(ch);
        if (!case_sensitive) ch = lower_rune(ch);
        if (normalize) ch = mm_algo_normalize_rune(ch);
        if (pidx < plen && ch == pattern[pidx]) {
            if (pos) pos[np] = idx;
            np++;
            score += scoreMatch;
            int16_t bonus = bonusMatrix[prevClass][cls];
            if (consecutive == 0) {
                firstBonus = bonus;
            } else {
                if (bonus >= bonusBoundary && bonus > firstBonus) firstBonus = bonus;
                int16_t m = bonus;
                if (firstBonus > m) m = firstBonus;
                if (bonusConsecutive > m) m = bonusConsecutive;
                bonus = m;
            }
            if (pidx == 0) score += (int)bonus * bonusFirstCharMultiplier;
            else score += (int)bonus;
            inGap = false;
            consecutive++;
            pidx++;
        } else {
            score += inGap ? scoreGapExtension : scoreGapStart;
            inGap = true;
            consecutive = 0;
            firstBonus = 0;
        }
        prevClass = cls;
    }
    *npos = np;
    return score;
}

static mm_match_t no_match(void) {
    mm_match_t m = {0};
    m.result.start = -1;
    m.result.end = -1;
    m.result.score = 0;
    return m;
}

/* asciiFuzzyIndex equivalent. Returns the forward start index (firstIdx) and
   sets *maxIdx (exclusive end of the search scope). Returns -1 when the
   pattern is not a subsequence. Mirrors fzf's byte-domain prefilter: for each
   pattern code point, skip forward to its occurrence; firstIdx steps back one
   from the first pattern char's first occurrence to find the right bonus point;
   maxIdx extends to the last occurrence of the final pattern char. */
/* True when the text is pure ASCII. fzf only uses its ASCII fast paths for
   such input (util.Chars.IsBytes); otherwise it falls back to the general
   path, which is also the only path that applies normalization. */
static bool text_is_ascii(const wchar_t *t, int n) {
    for (int i = 0; i < n; i++)
        if (t[i] >= 128) return false;
    return true;
}

static int ascii_fuzzy_index(const wchar_t *text, int n, const wchar_t *pattern, int plen,
                             bool case_sensitive, int *maxIdx) {
    int firstIdx = 0, idx = 0, lastIdx = 0;
    wchar_t b = 0;
    for (int pidx = 0; pidx < plen; pidx++) {
        b = pattern[pidx];
        /* trySkip: advance idx to first index >= idx where text matches b */
        int found = -1;
        for (int i = idx; i < n; i++) {
            wchar_t ch = text[i];
            bool eq = (ch == b);
            if (!eq && !case_sensitive && b >= 'a' && b <= 'z' && ch >= 'A' && ch <= 'Z')
                eq = ((wchar_t)(ch + 32) == b);
            if (eq) { found = i; break; }
        }
        if (found < 0) return -1;
        idx = found;
        if (pidx == 0 && idx > 0) firstIdx = idx - 1;
        lastIdx = idx;
        idx++;
    }
    /* Find last occurrence of the final pattern char within [lastIdx, n). */
    int end = -1;
    for (int i = lastIdx + 1; i < n; i++) {
        wchar_t ch = text[i];
        bool eq = (ch == b);
        if (!eq && !case_sensitive && b >= 'a' && b <= 'z' && ch >= 'A' && ch <= 'Z')
            eq = ((wchar_t)(ch + 32) == b);
        if (eq) end = i - (lastIdx + 1);
    }
    if (end >= 0) { *maxIdx = lastIdx + 1 + end + 1; return firstIdx; }
    *maxIdx = lastIdx + 1;
    return firstIdx;
}

/* ---------------- FuzzyMatchV2 ---------------- */

/* Reusable scratch space for the score matrices.  The matcher runs once per
   item, so allocating the matrices per item dominates the runtime on large
   inputs; fzf avoids the same cost with its slab allocator.  A single
   process-wide buffer is safe here: the matcher is not re-entrant. */
static _Thread_local int16_t *g_H0, *g_C0, *g_B, *g_H, *g_C;
static _Thread_local int     *g_F;
static _Thread_local wchar_t *g_T;
static _Thread_local size_t   g_cap_n, g_cap_m, g_cap_hc;

static bool match_ws_reserve(int n, int m) {
    if (n <= 0 || m <= 0) return false;

    size_t needN = (size_t)n;
    size_t needM = (size_t)m;
    size_t needHc;
    if (needN > SIZE_MAX / needM) return false;
    needHc = needN * needM;
    if (needN > SIZE_MAX / sizeof(*g_H0) ||
        needN > SIZE_MAX / sizeof(*g_T) ||
        needM > SIZE_MAX / sizeof(*g_F) ||
        needHc > SIZE_MAX / sizeof(*g_H))
        return false;

    /* Grow geometrically: interactive searches often increase their maximum
       line/query length gradually. Avoid reallocating the entire workspace
       for every small increase. Allocate replacement groups transactionally,
       so allocation failure leaves the old workspace valid. */
    if (needN > g_cap_n) {
        size_t capN = g_cap_n ? g_cap_n : 64;
        while (capN < needN) {
            if (capN > SIZE_MAX / 2) { capN = needN; break; }
            capN *= 2;
        }
        if (capN > SIZE_MAX / sizeof(*g_H0) ||
            capN > SIZE_MAX / sizeof(*g_T))
            return false;

        int16_t *newH0 = malloc(capN * sizeof(*newH0));
        int16_t *newC0 = malloc(capN * sizeof(*newC0));
        int16_t *newB = malloc(capN * sizeof(*newB));
        wchar_t *newT = malloc(capN * sizeof(*newT));
        if (!newH0 || !newC0 || !newB || !newT) {
            free(newH0); free(newC0); free(newB); free(newT);
            return false;
        }
        free(g_H0); free(g_C0); free(g_B); free(g_T);
        g_H0 = newH0; g_C0 = newC0; g_B = newB; g_T = newT;
        g_cap_n = capN;
    }

    if (needM > g_cap_m) {
        size_t capM = g_cap_m ? g_cap_m : 32;
        while (capM < needM) {
            if (capM > SIZE_MAX / 2) { capM = needM; break; }
            capM *= 2;
        }
        if (capM > SIZE_MAX / sizeof(*g_F)) return false;
        int *newF = malloc(capM * sizeof(*newF));
        if (!newF) return false;
        free(g_F);
        g_F = newF;
        g_cap_m = capM;
    }

    if (needHc > g_cap_hc) {
        size_t capHc = g_cap_hc ? g_cap_hc : 2048;
        while (capHc < needHc) {
            if (capHc > SIZE_MAX / 2) { capHc = needHc; break; }
            capHc *= 2;
        }
        if (capHc > SIZE_MAX / sizeof(*g_H)) return false;
        int16_t *newH = malloc(capHc * sizeof(*newH));
        int16_t *newC = malloc(capHc * sizeof(*newC));
        if (!newH || !newC) {
            free(newH); free(newC);
            return false;
        }
        free(g_H); free(g_C);
        g_H = newH; g_C = newC;
        g_cap_hc = capHc;
    }
    return true;
}

/* ASCII lines stay bytes during an interactive search. Widening every path
   to wchar_t before the scan is most of the first-keystroke cost. These
   paths score the same as FuzzyMatchV2 on ASCII: normalization does not
   change a byte below 128, which is why fzf's single- and two-character
   paths ignore that flag. */
static _Thread_local wchar_t *g_widen;
static _Thread_local int      g_widen_cap;
static wchar_t g_widen_empty;

static const wchar_t *widen_ascii(const unsigned char *s, int n) {
    if (n <= 0) return &g_widen_empty;
    if (n + 1 > g_widen_cap) {
        int cap = g_widen_cap > 0 ? g_widen_cap : 128;
        while (cap < n + 1) {
            if (cap > INT_MAX / 2) { cap = n + 1; break; }
            cap *= 2;
        }
        wchar_t *grown = malloc((size_t)cap * sizeof(wchar_t));
        if (!grown) return NULL;
        free(g_widen);
        g_widen = grown;
        g_widen_cap = cap;
    }
    for (int i = 0; i < n; i++) g_widen[i] = (wchar_t)s[i];
    g_widen[n] = 0;
    return g_widen;
}

static const wchar_t *text_as_runes(const mm_text_t *text) {
    if (!text) return NULL;
    if (text->runes || text->length <= 0 || !text->bytes) return text->runes;
    return widen_ascii(text->bytes, text->length);
}

static int cpu_has_avx2(void) {
    static atomic_int cached = -1;
    int v = atomic_load_explicit(&cached, memory_order_relaxed);
    if (v >= 0) return v;
    int got = __builtin_cpu_supports("avx2") ? 1 : 0;
    int expected = -1;
    atomic_compare_exchange_strong_explicit(&cached, &expected, got,
                                           memory_order_relaxed, memory_order_relaxed);
    return got;
}

/* One pass over both cases. fzf's IndexByteTwo does the same; two memchr
   calls read the miss path twice. */
__attribute__((target("avx2")))
static int find_either_avx2(const unsigned char *s, int from, int n,
                           unsigned char lo, unsigned char hi) {
    const unsigned char *p = s + from;
    const unsigned char *end = s + n;
    __m256i vl = _mm256_set1_epi8((char)lo);
    __m256i vh = _mm256_set1_epi8((char)hi);
    while (p + 32 <= end) {
        __m256i c = _mm256_loadu_si256((const __m256i *)p);
        __m256i eq = _mm256_or_si256(_mm256_cmpeq_epi8(c, vl), _mm256_cmpeq_epi8(c, vh));
        unsigned mask = (unsigned)_mm256_movemask_epi8(eq);
        if (mask) return (int)(p - s) + __builtin_ctz(mask);
        p += 32;
    }
    while (p < end) {
        if (*p == lo || *p == hi) return (int)(p - s);
        p++;
    }
    return -1;
}

static int find_either_mem(const unsigned char *s, int from, int n,
                          unsigned char lo, unsigned char hi) {
    const unsigned char *base = s + from;
    size_t len = (size_t)(n - from);
    const void *hit = memchr(base, lo, len);
    if (hit == base) return from;
    if (hit) {
        size_t prefix = (size_t)((const unsigned char *)hit - base);
        const void *uhit = memchr(base, hi, prefix);
        if (uhit) return (int)((const unsigned char *)uhit - s);
        return (int)((const unsigned char *)hit - s);
    }
    const void *uhit = memchr(base, hi, len);
    return uhit ? (int)((const unsigned char *)uhit - s) : -1;
}

static int find_byte(const unsigned char *s, int n, int from, unsigned char b, bool cs) {
    if (from >= n) return -1;
    if (cs || b < 'a' || b > 'z') {
        const void *hit = memchr(s + from, b, (size_t)(n - from));
        return hit ? (int)((const unsigned char *)hit - s) : -1;
    }
    unsigned char up = (unsigned char)(b - 32);
    if (cpu_has_avx2()) return find_either_avx2(s, from, n, b, up);
    return find_either_mem(s, from, n, b, up);
}

/* Rightmost of either case in one pass. A miss must not read the string twice. */
__attribute__((target("avx2")))
static int rfind_either_avx2(const unsigned char *s, int from, int last,
                            unsigned char lo, unsigned char hi) {
    const unsigned char *beg = s + from;
    const unsigned char *p = s + last;
    __m256i vl = _mm256_set1_epi8((char)lo);
    __m256i vh = _mm256_set1_epi8((char)hi);
    while (p - beg >= 32) {
        p -= 32;
        __m256i c = _mm256_loadu_si256((const __m256i *)p);
        __m256i eq = _mm256_or_si256(_mm256_cmpeq_epi8(c, vl), _mm256_cmpeq_epi8(c, vh));
        unsigned mask = (unsigned)_mm256_movemask_epi8(eq);
        if (mask) return (int)(p - s) + (31 - __builtin_clz(mask));
    }
    while (p > beg) {
        p--;
        if (*p == lo || *p == hi) return (int)(p - s);
    }
    return -1;
}

static int rfind_either_scalar(const unsigned char *s, int from, int last,
                              unsigned char lo, unsigned char hi) {
    for (int i = last - 1; i >= from; i--)
        if (s[i] == lo || s[i] == hi) return i;
    return -1;
}

static int rfind_byte(const unsigned char *s, int from, int last, unsigned char b, bool cs) {
    if (last <= from) return -1;
    const unsigned char *base = s + from;
    size_t len = (size_t)(last - from);
    if (cs || b < 'a' || b > 'z') {
        const void *hit = memrchr(base, b, len);
        return hit ? (int)((const unsigned char *)hit - s) : -1;
    }
    unsigned char up = (unsigned char)(b - 32);
    if (cpu_has_avx2()) return rfind_either_avx2(s, from, last, b, up);
    return rfind_either_scalar(s, from, last, b, up);
}

/* fzf asciiFuzzyIndex on a byte string. *maxIdx is one past the last
   occurrence of the final pattern byte. Returns -1 when it is not a
   subsequence. */
__attribute__((always_inline)) static inline int
ascii_index_bytes(const unsigned char *s, int n, const wchar_t *pattern, int plen,
                             bool case_sensitive, int *max_idx) {
    int first = 0, idx = 0, last = 0;
    unsigned char b = 0;
    for (int pidx = 0; pidx < plen; pidx++) {
        if (pattern[pidx] < 0 || pattern[pidx] >= 128) return -1;
        b = (unsigned char)pattern[pidx];
        idx = find_byte(s, n, idx, b, case_sensitive);
        if (idx < 0) return -1;
        if (pidx == 0 && idx > 0) first = idx - 1;
        last = idx;
        idx++;
    }
    if (last + 1 < n) {
        int end = rfind_byte(s, last + 1, n, b, case_sensitive);
        if (end >= 0) { *max_idx = end + 1; return first; }
    }
    *max_idx = last + 1;
    return first;
}

/* Highest first-character bonus this pattern byte can score. b must be
   an ASCII byte. Case-insensitive a-z also matches A-Z. */
static int16_t max_first_bonus(unsigned char b, bool case_sensitive) {
    int16_t best = 0;
    char_class cls = asciiCharClasses[b];
    for (int prev = 0; prev <= charNumber; prev++) {
        int16_t v = bonusMatrix[prev][cls];
        if (v > best) best = v;
    }
    if (!case_sensitive && b >= 'a' && b <= 'z') {
        cls = asciiCharClasses[(unsigned char)(b - 32)];
        for (int prev = 0; prev <= charNumber; prev++) {
            int16_t v = bonusMatrix[prev][cls];
            if (v > best) best = v;
        }
    }
    return best;
}

int16_t mm_max_first_bonus(unsigned char b, bool case_sensitive) {
    return max_first_bonus(b, case_sensitive);
}

/* Same decisions as fuzzyMatchV2Single. The best bonus is hoisted by the
   interactive scan so a million paths do not recompute it per item. */
__attribute__((always_inline)) static inline bool
ascii1_one(const unsigned char *s, int n, unsigned char b,
           bool case_sensitive, bool forward, int16_t best_bonus,
           int *start, int *score) {
    int16_t max_score = 0;
    int max_pos = -1;
    if (!forward) {
        int limit = n;
        while (limit > 0) {
            int idx = rfind_byte(s, 0, limit, b, case_sensitive);
            if (idx < 0) break;
            char_class cls = asciiCharClasses[s[idx]];
            char_class prev = (char_class)initialCharClass;
            if (idx > 0) prev = asciiCharClasses[s[idx - 1]];
            int16_t bonus = bonusMatrix[prev][cls];
            int16_t sc = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            if (sc > max_score) {
                max_score = sc;
                max_pos = idx;
                if (bonus >= best_bonus) break;
            }
            limit = idx;
        }
    } else {
        for (int idx = 0; idx < n;) {
            idx = find_byte(s, n, idx, b, case_sensitive);
            if (idx < 0) break;
            char_class cls = asciiCharClasses[s[idx]];
            char_class prev = (char_class)initialCharClass;
            if (idx > 0) prev = asciiCharClasses[s[idx - 1]];
            int16_t bonus = bonusMatrix[prev][cls];
            int16_t sc = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            if (sc > max_score) {
                max_score = sc;
                max_pos = idx;
                if (bonus >= bonusBoundary) break;
            }
            idx++;
        }
    }
    if (max_pos < 0) return false;
    *start = max_pos;
    *score = (int)max_score;
    return true;
}

bool mm_fuzzy_match_ascii1(const unsigned char *s, int n, unsigned char b,
                           bool case_sensitive, bool forward, int16_t best_bonus,
                           int *start, int *score) {
    if (!s || n <= 0 || !start || !score) return false;
    if (best_bonus < 0) best_bonus = max_first_bonus(b, case_sensitive);
    return ascii1_one(s, n, b, case_sensitive, forward, best_bonus, start, score);
}

/* Rightmost '/' or '\\'. -1 when the path has neither. */
__attribute__((always_inline)) static inline int
last_slash_of(const unsigned char *s, int n) {
    for (int i = n - 1; i >= 0; i--) {
        unsigned char c = s[i];
        if (c == '/' || c == '\\') return i;
    }
    return -1;
}

__attribute__((always_inline)) static inline uint64_t
ascii1_pack_key(int key_mode, int score, int pos, int slash, uint16_t trim) {
    uint16_t sk = (uint16_t)(0xFFFFu - (uint16_t)score);
    if (key_mode == 1)
        return ((uint64_t)sk << 48) | ((uint64_t)trim << 32);
    uint16_t path = 0xFFFF;
    if (pos >= 0 && pos <= 0xFFFF && slash <= pos)
        path = (uint16_t)(pos - slash);
    return ((uint64_t)sk << 48) | ((uint64_t)path << 32) | ((uint64_t)trim << 16);
}

__attribute__((always_inline)) static inline uint16_t
ascii1_trim(mm_item_t *it, const unsigned char *s, int len) {
    if (len > 0 && len <= 0xFFFF && s[0] > ' ' && s[len - 1] > ' ')
        return (uint16_t)len;
    return mm_item_trim_length(it);
}

int mm_ascii1_rank(mm_item_t *items, const int *cands, int ncand,
                   int range_lo, int begin, int end, int item_count,
                   int header_lines, const bool *excluded,
                   unsigned char b, bool case_sensitive, bool forward,
                   int16_t best_bonus, int key_mode, mm_hit_key_t *out,
                   int *slow) {
    if (slow) *slow = 0;
    if (!items || !out || end < begin) return 0;
    if (best_bonus < 0) best_bonus = max_first_bonus(b, case_sensitive);
    int n = 0;
    for (int v = begin; v < end; v++) {
        int idx;
        if (cands && v < ncand) idx = cands[v];
        else {
            int tail = cands ? v - ncand : v;
            idx = range_lo + tail;
        }
        if (idx < header_lines || idx >= item_count) continue;
        if (excluded && excluded[idx]) continue;
        if (v + 8 < end) {
            int ahead_v = v + 8;
            int ahead;
            if (cands && ahead_v < ncand) ahead = cands[ahead_v];
            else ahead = range_lo + (cands ? ahead_v - ncand : ahead_v);
            if (ahead >= header_lines && ahead < item_count) {
                const char *nxt = items[ahead].text.bytes;
                if (nxt) __builtin_prefetch(nxt, 0, 3);
            }
        }
        mm_item_t *it = &items[idx];
        if (!(it->text.ascii && it->text.bytes && !it->text.decoded)) {
            out[n].key = UINT64_MAX;
            out[n].index = idx;
            n++;
            if (slow) (*slow)++;
            continue;
        }
        const unsigned char *s = (const unsigned char *)it->text.bytes;
        int len = it->text.length;
        if (len <= 0) continue;
        int pos = 0, score = 0, slash = -1;
        if (!ascii1_one(s, len, b, case_sensitive, forward, best_bonus,
                        &pos, &score))
            continue;
        uint16_t trim = 0;
        if (key_mode != 0) {
            trim = ascii1_trim(it, s, len);
            if (key_mode == 2) slash = last_slash_of(s, len);
        }
        out[n].key = key_mode == 0 ? 0 :
                     ascii1_pack_key(key_mode, score, pos, slash, trim);
        out[n].index = it->index;
        n++;
    }
    return n;
}

static mm_match_t single_match_result(int max_pos, int16_t max_score, bool with_pos) {
    if (max_pos < 0) return no_match();
    mm_match_t m = no_match();
    m.result.start = max_pos;
    m.result.end = max_pos + 1;
    m.result.score = max_score;
    if (with_pos) {
        m.pos = malloc(sizeof(int));
        if (m.pos) { m.pos[0] = max_pos; m.npos = 1; }
    }
    return m;
}

/* fzf fuzzyMatchV2Single. A forward query stops at the first boundary
   bonus. A backward query keeps the rightmost best bonus. */
static mm_match_t fuzzy_v2_single_bytes(const unsigned char *s, int n, unsigned char b,
                                       bool case_sensitive, bool forward, bool with_pos) {
    int start = 0, score = 0;
    if (!mm_fuzzy_match_ascii1(s, n, b, case_sensitive, forward, -1, &start, &score))
        return no_match();
    return single_match_result(start, (int16_t)score, with_pos);
}

static int16_t i16_nonneg(int v) { return v > 0 ? (int16_t)v : 0; }

static int16_t i16_max3(int a, int b, int c) {
    int m = a > b ? a : b;
    if (c > m) m = c;
    return (int16_t)m;
}

/* fzf fuzzyMatchV2Two without positions. Row 0's consecutive count is only
   ever 0 or 1, so the previous column's bonus is the chunk-start bonus. */
__attribute__((always_inline)) static inline mm_match_t
fuzzy_v2_two_bytes(const unsigned char *s, unsigned char p0, unsigned char p1,
                                    int min_idx, int max_idx, bool case_sensitive, bool forward,
                                    bool with_pos) {
    int n = max_idx - min_idx;
    /* Positions need the score rows. Path ranking only needs the start
       offset, which the running state below reproduces without storing them. */
    bool trace = with_pos;
    int16_t *H0 = NULL, *C0 = NULL, *H1 = NULL, *C1 = NULL;
    if (trace) {
        if (!match_ws_reserve(n, 2)) return no_match();
        H0 = g_H0;
        C0 = g_C0;
        H1 = g_H;
        C1 = g_C;
    }
    int16_t max_score = 0;
    int max_pos = 0;
    char_class prev_class = (char_class)initialCharClass;
    int f0 = -1, f1 = -1;
    int16_t h0_prev = 0, c0_prev = 0, b_prev = 0;
    int16_t h1_prev = 0;
    bool in_gap0 = false, in_gap1 = false;
    /* O(1) stand-in for the two-row backtrace. stop_* is where a row-0 walk
       that starts at the previous cell ends; ans_* is the pattern start if a
       row-1 walk starts at the previous cell. hi/lo is the incoming prefer. */
    int stop_hi = 0, stop_lo = 0, older_hi = 0;
    int ans_hi = 0, ans_lo = 0, best_from = 0;

    for (int off = 0; off < n; off++) {
        unsigned char ch = s[min_idx + off];
        char_class cls = asciiCharClasses[ch];
        unsigned char low = ch;
        if (!case_sensitive && ch >= 'A' && ch <= 'Z') low = (unsigned char)(ch + 32);
        int16_t bonus = bonusMatrix[prev_class][cls];
        prev_class = cls;

        if (f0 < 0) {
            if (low == p0) f0 = off;
        } else if (low == p1 && f1 < 0) {
            f1 = off;
        }

        int16_t h0, c0;
        if (low == p0) {
            h0 = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            c0 = 1;
            in_gap0 = false;
        } else {
            h0 = i16_nonneg(h0_prev + (in_gap0 ? scoreGapExtension : scoreGapStart));
            c0 = 0;
            in_gap0 = true;
        }
        if (trace) {
            H0[off] = h0;
            C0[off] = c0;
        }

        if (f1 >= 0 && off >= f1) {
            int hleft = (off == f1) ? 0 : h1_prev;
            int s2 = hleft + (in_gap1 ? scoreGapExtension : scoreGapStart);
            int s1 = 0;
            int consecutive = 0;
            if (low == p1) {
                s1 = h0_prev + scoreMatch;
                int bb = bonus;
                consecutive = c0_prev + 1;
                if (c0_prev + 1 > 1) {
                    int fb = b_prev;
                    if (bb >= bonusBoundary && bb > fb)
                        consecutive = 1;
                    else
                        bb = i16_max3(bb, bonusConsecutive, fb);
                }
                if (s1 + bb < s2) {
                    s1 += bonus;
                    consecutive = 0;
                } else {
                    s1 += bb;
                }
            }
            in_gap1 = s1 < s2;
            int16_t score = i16_max3(s1, s2, 0);
            int16_t s1bt = h0_prev;
            int16_t s2bt = (off > f1) ? h1_prev : 0;
            bool d_hi = score > s1bt && score >= s2bt;
            bool d_lo = score > s1bt && score > s2bt;
            /* Row 0 is entered at off-1. A tie there stops only when prefer
               is true. prefer starts false for a one-long chunk and becomes
               true on the next cell when this chunk matched at all. */
            int cell = off - 1;
            bool entry_hi = consecutive > 1;
            bool dom = entry_hi ? (stop_hi == cell) : (stop_lo == cell);
            int matched;
            if (dom)
                matched = cell;
            else if (consecutive > 0 && older_hi == cell - 1)
                matched = cell - 1;
            else
                matched = entry_hi ? stop_hi : stop_lo;
            int continued = (consecutive > 1) ? ans_hi : ans_lo;
            int here_hi = d_hi ? matched : continued;
            int here_lo = d_lo ? matched : continued;
            if ((forward && score > max_score) || (!forward && score >= max_score)) {
                max_score = score;
                max_pos = off;
                best_from = here_hi;
            }
            h1_prev = score;
            ans_hi = here_hi;
            ans_lo = here_lo;
            if (trace) {
                H1[off] = score;
                C1[off] = (int16_t)consecutive;
            }
        }
        {
            int16_t left = (f0 >= 0 && off > f0) ? h0_prev : 0;
            bool dom_hi = h0 > 0 && h0 >= left;
            bool dom_lo = h0 > 0 && h0 > left;
            int next_hi = dom_hi ? off : stop_hi;
            int next_lo = dom_lo ? off : stop_lo;
            older_hi = stop_hi;
            stop_hi = next_hi;
            stop_lo = next_lo;
        }
        h0_prev = h0;
        c0_prev = c0;
        b_prev = bonus;
    }
    if (f1 < 0) return no_match();
    /* Forward queries report the first pattern byte. A backward query, which
       is what pathname ordering uses, reports the same start the backtrace
       would. The two agree on the path corpus. */
    int start_off = (!forward && !with_pos) ? best_from : f0;
    int pos_at[2];
    int npos = 0;
    if (trace) {
        /* fzf's two-row backtrace. The cell left of F[1] on row 1 was never
           written, so it is treated as zero. */
        int i = 1;
        int j = max_pos;
        bool prefer = true;
        while (j >= 0) {
            int16_t s = 0, s1 = 0, s2 = 0, ccur = 0;
            if (i == 1) {
                if (j >= f1) {
                    s = H1[j];
                    ccur = C1[j];
                }
                if (j >= f1) s1 = H0[j - 1];
                if (j > f1) s2 = H1[j - 1];
            } else {
                s = H0[j];
                ccur = C0[j];
                if (j > f0) s2 = H0[j - 1];
            }
            int row = i;
            if (s > s1 && (s > s2 || (s == s2 && prefer))) {
                if (npos < 2) pos_at[npos++] = min_idx + j;
                if (i == 0) {
                    start_off = j;
                    break;
                }
                i--;
            }
            prefer = ccur > 1 ||
                (row == 0 && j < n - 1 && (j + 1) >= f1 && C1[j + 1] > 0);
            j--;
        }
    }
    mm_match_t m = no_match();
    m.result.start = min_idx + start_off;
    m.result.end = min_idx + max_pos + 1;
    m.result.score = max_score;
    if (with_pos && npos > 0) {
        m.pos = malloc((size_t)npos * sizeof(int));
        if (m.pos) {
            for (int i = 0; i < npos; i++) m.pos[i] = pos_at[i];
            m.npos = npos;
        }
    }
    return m;
}

static _Thread_local unsigned char *g_bytes;
static _Thread_local size_t g_bytes_cap;

static unsigned char *byte_buf(int n) {
    if (n <= 0) return NULL;
    if ((size_t)n <= g_bytes_cap) return g_bytes;
    size_t cap = g_bytes_cap ? g_bytes_cap : 64;
    while (cap < (size_t)n) {
        if (cap > SIZE_MAX / 2) { cap = (size_t)n; break; }
        cap *= 2;
    }
    unsigned char *p = malloc(cap);
    if (!p) return NULL;
    free(g_bytes);
    g_bytes = p;
    g_bytes_cap = cap;
    return g_bytes;
}

/* FuzzyMatchV2 for an ASCII slice, without widening each path to wchar_t.
   The matrix and the backtrace match the rune implementation, so pathname
   tiebreak (the walker's scheme) sees the same start offset. */
static mm_match_t fuzzy_v2_ascii_n(const unsigned char *s, int min_idx, int max_idx,
                                  const wchar_t *pattern, int plen,
                                  bool case_sensitive, bool forward, bool with_pos) {
    int N = max_idx - min_idx;
    int M = plen;
    if (N <= 0 || M <= 0 || M > N) return no_match();
    if (!match_ws_reserve(N, M)) return no_match();
    unsigned char *T = byte_buf(N);
    if (!T) return no_match();

    int16_t *H0 = g_H0, *C0 = g_C0, *B = g_B;
    int *F = g_F;
    mm_match_t out = no_match();
    int16_t maxScore = 0;
    int maxScorePos = 0;
    int pidx = 0, lastIdx = 0;
    unsigned char pchar0 = (unsigned char)pattern[0];
    unsigned char pchar = pchar0;
    int16_t prevH0 = 0;
    char_class prevClass = initialCharClass;
    bool inGap = false;

    for (int off = 0; off < N; off++) {
        unsigned char raw = s[min_idx + off];
        char_class cls = raw < 128 ? asciiCharClasses[raw] : charNonWord;
        unsigned char ch = raw;
        if (raw < 128 && !case_sensitive && cls == charUpper)
            ch = (unsigned char)(raw + 32);
        T[off] = ch;
        int16_t bonus = bonusMatrix[prevClass][cls];
        B[off] = bonus;
        prevClass = cls;

        if (ch == pchar) {
            if (pidx < M) {
                F[pidx] = off;
                pidx++;
                pchar = (unsigned char)pattern[pidx < M ? pidx : M - 1];
            }
            lastIdx = off;
        }

        if (ch == pchar0) {
            int16_t score = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            H0[off] = score;
            C0[off] = 1;
            if (M == 1 && ((forward && score > maxScore) || (!forward && score >= maxScore))) {
                maxScore = score;
                maxScorePos = off;
                if (forward && bonus >= bonusBoundary) break;
            }
            inGap = false;
        } else {
            if (inGap)
                H0[off] = prevH0 + scoreGapExtension > 0 ? (int16_t)(prevH0 + scoreGapExtension) : 0;
            else
                H0[off] = prevH0 + scoreGapStart > 0 ? (int16_t)(prevH0 + scoreGapStart) : 0;
            C0[off] = 0;
            inGap = true;
        }
        prevH0 = H0[off];
    }
    if (pidx != M) return no_match();

    if (M == 1) {
        out.result.start = min_idx + maxScorePos;
        out.result.end = min_idx + maxScorePos + 1;
        out.result.score = maxScore;
        if (with_pos) {
            out.pos = malloc(sizeof(int));
            if (out.pos) { out.pos[0] = min_idx + maxScorePos; out.npos = 1; }
        }
        return out;
    }

    int f0 = F[0];
    int width = lastIdx - f0 + 1;
    int16_t *H = g_H;
    int16_t *C = g_C;
    if (!H || !C || width <= 0) return no_match();
    for (int i = 0; i <= lastIdx - f0; i++) { H[i] = H0[f0 + i]; C[i] = C0[f0 + i]; }

    maxScore = 0;
    maxScorePos = 0;
    for (int off = 1; off < M; off++) {
        int f = F[off];
        unsigned char pch = (unsigned char)pattern[off];
        int row = off * width;
        bool gap = false;
        for (int coloff = 0; coloff < lastIdx - f + 1; coloff++) {
            int col = coloff + f;
            int16_t s1 = 0, s2, consecutive = 0;
            int idxInRow = row + col - f0;
            int idxDiag = row + col - f0 - 1 - width;
            int idxLeft = row + col - f0 - 1;
            if (coloff == 0) H[idxLeft] = 0;
            s2 = (int16_t)(H[idxLeft] + (gap ? scoreGapExtension : scoreGapStart));
            if (pch == T[col]) {
                s1 = (int16_t)(H[idxDiag] + scoreMatch);
                int16_t b = B[col];
                consecutive = (int16_t)(C[idxDiag] + 1);
                if (consecutive > 1) {
                    int16_t fb = B[col - consecutive + 1];
                    if (b >= bonusBoundary && b > fb) consecutive = 1;
                    else {
                        int16_t m = b;
                        if (bonusConsecutive > m) m = bonusConsecutive;
                        if (fb > m) m = fb;
                        b = m;
                    }
                }
                if (s1 + b < s2) { s1 = (int16_t)(s1 + B[col]); consecutive = 0; }
                else s1 = (int16_t)(s1 + b);
            }
            C[idxInRow] = consecutive;
            gap = s1 < s2;
            int16_t score = s1 > s2 ? s1 : s2;
            if (score < 0) score = 0;
            if (off == M - 1 && ((forward && score > maxScore) || (!forward && score >= maxScore))) {
                maxScore = score;
                maxScorePos = col;
            }
            H[idxInRow] = score;
        }
    }

    int j = f0;
    {
        int i = M - 1;
        j = maxScorePos;
        bool preferMatch = true;
        int np = 0;
        if (with_pos) {
            out.pos = malloc((size_t)M * sizeof(int));
            if (!out.pos) return no_match();
        }
        for (;;) {
            int I = i * width;
            int j0 = j - f0;
            int16_t s = H[I + j0];
            int16_t s1 = -32768, s2 = -32768;
            if (i > 0 && j >= F[i]) s1 = H[I - width + j0 - 1];
            if (j > F[i]) s2 = H[I + j0 - 1];
            int row = i;
            if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
                if (with_pos) out.pos[np++] = j + min_idx;
                if (i == 0) break;
                i--;
            }
            preferMatch = C[I + j0] > 1 ||
                (row + 1 < M && j < lastIdx && (j + 1) >= F[row + 1] && C[I + width + j0 + 1] > 0);
            j--;
        }
        if (with_pos) out.npos = np;
    }
    out.result.start = min_idx + j;
    out.result.end = min_idx + maxScorePos + 1;
    out.result.score = maxScore;
    return out;
}

/* A run of bytes that are neither pattern character. Row scores only decay,
   and a decay never moves the backtrace stops or the best score: the gap
   penalty is negative, so the row-1 score stays strictly below the score
   that was already recorded, and a decayed row-0 cell does not dominate
   the cell before it. After one such byte both answers equal the previous
   low answer and the older stop equals the current stop. k is the length. */
static void two_apply_gap(int k, int f1, int16_t *h0, int16_t *h1,
                          bool *in_gap0, bool *in_gap1, int *ans_hi, int *ans_lo,
                          int *older_hi, int stop_hi) {
    int drop0 = *in_gap0 ? k : k + 2;
    int v0 = (int)*h0 - drop0;
    *h0 = v0 > 0 ? (int16_t)v0 : 0;
    *in_gap0 = true;
    if (f1 >= 0) {
        int drop1 = *in_gap1 ? k : k + 2;
        int v1 = (int)*h1 - drop1;
        *h1 = v1 > 0 ? (int16_t)v1 : 0;
        *in_gap1 = *h1 > 0;
        *ans_hi = *ans_lo;
    }
    *older_hi = stop_hi;
}

/* Backward two-byte match with no position list. Irrelevant bytes are
   skipped in one step; the score and the start match the matrix backtrace. */
static mm_match_t fuzzy_v2_two_fast(const unsigned char *s, int n,
                                   unsigned char p0, unsigned char p1,
                                   bool case_sensitive) {
    int16_t max_score = 0;
    int max_pos = 0;
    char_class prev_class = (char_class)initialCharClass;
    int f0 = -1, f1 = -1;
    int16_t h0_prev = 0, c0_prev = 0, b_prev = 0, h1_prev = 0;
    bool in_gap0 = false, in_gap1 = false;
    int stop_hi = 0, stop_lo = 0, older_hi = 0;
    int ans_hi = 0, ans_lo = 0, best_from = 0;

    for (int off = 0; off < n;) {
        unsigned char ch = s[off];
        unsigned char low = ch;
        if (!case_sensitive && ch >= 'A' && ch <= 'Z')
            low = (unsigned char)(ch + 32);
        if (low != p0 && low != p1) {
            int run = off + 1;
            while (run < n) {
                unsigned char c2 = s[run];
                unsigned char l2 = c2;
                if (!case_sensitive && c2 >= 'A' && c2 <= 'Z')
                    l2 = (unsigned char)(c2 + 32);
                if (l2 == p0 || l2 == p1) break;
                run++;
            }
            two_apply_gap(run - off, f1, &h0_prev, &h1_prev, &in_gap0, &in_gap1,
                          &ans_hi, &ans_lo, &older_hi, stop_hi);
            c0_prev = 0;
            prev_class = asciiCharClasses[s[run - 1]];
            off = run;
            continue;
        }

        char_class cls = asciiCharClasses[ch];
        int16_t bonus = bonusMatrix[prev_class][cls];
        prev_class = cls;
        if (f0 < 0) {
            if (low == p0) f0 = off;
        } else if (low == p1 && f1 < 0) {
            f1 = off;
        }

        int16_t h0, c0;
        if (low == p0) {
            h0 = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            c0 = 1;
            in_gap0 = false;
        } else {
            h0 = i16_nonneg(h0_prev + (in_gap0 ? scoreGapExtension : scoreGapStart));
            c0 = 0;
            in_gap0 = true;
        }

        if (f1 >= 0 && off >= f1) {
            int hleft = (off == f1) ? 0 : h1_prev;
            int s2 = hleft + (in_gap1 ? scoreGapExtension : scoreGapStart);
            int s1 = 0;
            int consecutive = 0;
            if (low == p1) {
                s1 = h0_prev + scoreMatch;
                int bb = bonus;
                consecutive = c0_prev + 1;
                if (c0_prev + 1 > 1) {
                    int fb = b_prev;
                    if (bb >= bonusBoundary && bb > fb)
                        consecutive = 1;
                    else
                        bb = i16_max3(bb, bonusConsecutive, fb);
                }
                if (s1 + bb < s2) {
                    s1 += bonus;
                    consecutive = 0;
                } else {
                    s1 += bb;
                }
            }
            in_gap1 = s1 < s2;
            int16_t score = i16_max3(s1, s2, 0);
            int16_t s1bt = h0_prev;
            int16_t s2bt = (off > f1) ? h1_prev : 0;
            bool d_hi = score > s1bt && score >= s2bt;
            bool d_lo = score > s1bt && score > s2bt;
            int cell = off - 1;
            bool entry_hi = consecutive > 1;
            bool dom = entry_hi ? (stop_hi == cell) : (stop_lo == cell);
            int matched;
            if (dom)
                matched = cell;
            else if (consecutive > 0 && older_hi == cell - 1)
                matched = cell - 1;
            else
                matched = entry_hi ? stop_hi : stop_lo;
            int continued = (consecutive > 1) ? ans_hi : ans_lo;
            int here_hi = d_hi ? matched : continued;
            int here_lo = d_lo ? matched : continued;
            if (score >= max_score) {
                max_score = score;
                max_pos = off;
                best_from = here_hi;
            }
            h1_prev = score;
            ans_hi = here_hi;
            ans_lo = here_lo;
        }
        {
            int16_t left = (f0 >= 0 && off > f0) ? h0_prev : 0;
            bool dom_hi = h0 > 0 && h0 >= left;
            bool dom_lo = h0 > 0 && h0 > left;
            int next_hi = dom_hi ? off : stop_hi;
            int next_lo = dom_lo ? off : stop_lo;
            older_hi = stop_hi;
            stop_hi = next_hi;
            stop_lo = next_lo;
        }
        h0_prev = h0;
        c0_prev = c0;
        b_prev = bonus;
        off++;
    }
    if (f1 < 0) return no_match();
    mm_match_t m = no_match();
    m.result.start = best_from;
    m.result.end = max_pos + 1;
    m.result.score = max_score;
    return m;
}

/* k gap columns. Row 0 keeps the gap flag set. Later rows clear it once the
   score hits zero, matching one non-matching step repeated k times. */
static int16_t fast3_decay(int h, bool in_gap, int k, bool *out_gap, bool row0) {
    if (k <= 0) {
        *out_gap = in_gap;
        return (int16_t)h;
    }
    int drop = in_gap ? k : k + 2;
    int v = h - drop;
    if (v < 0) v = 0;
    *out_gap = row0 ? true : v > 0;
    return (int16_t)v;
}

/* One column of a row after row 0. left_* is this row at the previous
   column. diag_* is the previous row there. bonus is this column's raw bonus. */
static void fast3_row(int16_t left_h, bool left_gap, int16_t diag_h, int16_t diag_c,
                      int16_t diag_chunk, int16_t bonus, bool match,
                      int16_t *h, int16_t *c, bool *gap, int16_t *chunk) {
    int16_t s2 = (int16_t)(left_h + (left_gap ? scoreGapExtension : scoreGapStart));
    int16_t s1 = 0;
    int16_t consecutive = 0;
    if (match) {
        s1 = (int16_t)(diag_h + scoreMatch);
        int16_t b = bonus;
        consecutive = (int16_t)(diag_c + 1);
        if (consecutive > 1) {
            int16_t fb = diag_chunk;
            if (b >= bonusBoundary && b > fb)
                consecutive = 1;
            else {
                if (bonusConsecutive > b) b = bonusConsecutive;
                if (fb > b) b = fb;
            }
        }
        if ((int)s1 + b < s2) {
            s1 = (int16_t)(s1 + bonus);
            consecutive = 0;
        } else {
            s1 = (int16_t)(s1 + b);
        }
    }
    *gap = s1 < s2;
    int16_t score = s1 > s2 ? s1 : s2;
    if (score < 0) score = 0;
    *h = score;
    *c = consecutive;
    if (consecutive == 1) *chunk = bonus;
    else if (consecutive > 1) *chunk = diag_chunk;
}

enum { FAST3_CAP = 768 };

typedef struct {
    int pos;
    int16_t h[3];
    int16_t c[3];
    uint8_t gap[3];
    uint8_t on[3];
} fast3_node;

static _Thread_local fast3_node fast3_nodes[FAST3_CAP];

/* idx is the rightmost stored column at or before j, or -1. */
static void fast3_at(const fast3_node *nodes, const int *F, int j, int row,
                     int idx, int16_t *oh, int16_t *oc) {
    if (j < 0 || idx < 0 || (row > 0 && j < F[row]) || !nodes[idx].on[row]) {
        if (oh) *oh = 0;
        if (oc) *oc = 0;
        return;
    }
    const fast3_node *nd = &nodes[idx];
    int k = j - nd->pos;
    if (k <= 0) {
        if (oh) *oh = nd->h[row];
        if (oc) *oc = nd->c[row];
        return;
    }
    bool og = false;
    int16_t hv = fast3_decay(nd->h[row], nd->gap[row], k, &og, row == 0);
    if (oh) *oh = hv;
    if (oc) *oc = 0;
}

/* Three ASCII bytes, backward, no position list. Columns that match none of
   the pattern bytes only decay, so the matrix is built on the matching
   columns and the backtrace reads the gaps back from those. */
static mm_match_t fuzzy_v2_three_fast(const unsigned char *s, int n,
                                     const wchar_t *pattern, bool case_sensitive) {
    unsigned char ps[3] = {
        (unsigned char)pattern[0], (unsigned char)pattern[1], (unsigned char)pattern[2]
    };
    int F[3] = {-1, -1, -1};
    int pidx = 0;
    unsigned char pchar = ps[0];
    int last = -1;
    int nrel = 0;
    bool overflow = false;
    for (int i = 0; i < n; i++) {
        unsigned char ch = s[i];
        if (!case_sensitive && ch >= 'A' && ch <= 'Z')
            ch = (unsigned char)(ch + 32);
        if (ch == pchar) {
            if (pidx < 3) {
                F[pidx] = i;
                pidx++;
                if (pidx < 3) pchar = ps[pidx];
            }
            last = i;
        }
        if (ch == ps[0] || ch == ps[1] || ch == ps[2]) {
            if (nrel < FAST3_CAP) fast3_nodes[nrel++].pos = i;
            else overflow = true;
        }
    }
    if (pidx < 3) return no_match();
    while (nrel > 0 && fast3_nodes[nrel - 1].pos > last) nrel--;
    if (overflow && (nrel == 0 || fast3_nodes[nrel - 1].pos < last)) {
        int max_idx = 0;
        int min_idx = ascii_index_bytes(s, n, pattern, 3, case_sensitive, &max_idx);
        if (min_idx < 0) return no_match();
        return fuzzy_v2_ascii_n(s, min_idx, max_idx, pattern, 3,
                                case_sensitive, false, false);
    }

    int16_t h[3] = {0, 0, 0};
    int16_t cv[3] = {0, 0, 0};
    int16_t chunk[3] = {0, 0, 0};
    bool gap[3] = {false, false, false};
    bool on[3] = {true, false, false};
    int16_t max_score = 0;
    int max_pos = 0;
    int prev = -1;
    for (int ni = 0; ni < nrel; ni++) {
        int c = fast3_nodes[ni].pos;
        int k = c - prev - 1;
        if (k > 0) {
            for (int r = 0; r < 3; r++) {
                if (!on[r]) continue;
                bool og = false;
                h[r] = fast3_decay(h[r], gap[r], k, &og, r == 0);
                gap[r] = og;
                cv[r] = 0;
            }
        }
        int16_t ph[3], pc[3], pb[3];
        bool pg[3];
        for (int r = 0; r < 3; r++) {
            ph[r] = h[r];
            pc[r] = cv[r];
            pb[r] = chunk[r];
            pg[r] = gap[r];
        }
        unsigned char raw = s[c];
        unsigned char low = raw;
        if (!case_sensitive && raw >= 'A' && raw <= 'Z')
            low = (unsigned char)(raw + 32);
        char_class prev_cls = (char_class)initialCharClass;
        if (c > 0) prev_cls = asciiCharClasses[s[c - 1]];
        int16_t bonus = bonusMatrix[prev_cls][asciiCharClasses[raw]];
        if (low == ps[0]) {
            h[0] = (int16_t)(scoreMatch + bonus * bonusFirstCharMultiplier);
            cv[0] = 1;
            gap[0] = false;
            chunk[0] = bonus;
        } else {
            bool og = false;
            h[0] = fast3_decay(ph[0], pg[0], 1, &og, true);
            cv[0] = 0;
            gap[0] = og;
        }
        for (int r = 1; r < 3; r++) {
            if (c < F[r]) continue;
            bool starting = !on[r];
            on[r] = true;
            fast3_row(starting ? 0 : ph[r], starting ? false : pg[r],
                      ph[r - 1], pc[r - 1], pb[r - 1], bonus, low == ps[r],
                      &h[r], &cv[r], &gap[r], &chunk[r]);
        }
        fast3_node *nd = &fast3_nodes[ni];
        for (int r = 0; r < 3; r++) {
            nd->h[r] = h[r];
            nd->c[r] = cv[r];
            nd->gap[r] = (uint8_t)gap[r];
            nd->on[r] = (uint8_t)(r == 0 || c >= F[r]);
        }
        if (c >= F[2] && h[2] >= max_score) {
            max_score = h[2];
            max_pos = c;
        }
        prev = c;
    }

    int i = 2;
    int j = max_pos;
    int start = F[0];
    bool prefer = true;
    int idx = nrel - 1;
    while (idx >= 0 && fast3_nodes[idx].pos > j) idx--;
    while (j >= 0) {
        int row = i;
        int idx_left = (idx >= 0 && fast3_nodes[idx].pos == j) ? idx - 1 : idx;
        int idx_right = idx;
        if (idx + 1 < nrel && fast3_nodes[idx + 1].pos == j + 1)
            idx_right = idx + 1;
        else if (idx < 0)
            idx_right = -1;
        int16_t s = 0, s1 = -32768, s2 = -32768;
        fast3_at(fast3_nodes, F, j, i, idx, &s, NULL);
        if (i > 0 && j >= F[i])
            fast3_at(fast3_nodes, F, j - 1, i - 1, idx_left, &s1, NULL);
        if (j > F[i])
            fast3_at(fast3_nodes, F, j - 1, i, idx_left, &s2, NULL);
        if (s > s1 && (s > s2 || (s == s2 && prefer))) {
            if (i == 0) {
                start = j;
                break;
            }
            i--;
        }
        int16_t cj = 0, cnext = 0;
        fast3_at(fast3_nodes, F, j, row, idx, NULL, &cj);
        if (row + 1 < 3 && j < last && (j + 1) >= F[row + 1])
            fast3_at(fast3_nodes, F, j + 1, row + 1, idx_right, NULL, &cnext);
        prefer = cj > 1 || cnext > 0;
        if (idx >= 0 && fast3_nodes[idx].pos == j) idx--;
        j--;
    }
    mm_match_t m = no_match();
    m.result.start = start;
    m.result.end = max_pos + 1;
    m.result.score = max_score;
    return m;
}

__attribute__((always_inline)) static inline mm_match_t
fuzzy_v2_on_bytes(const unsigned char *s, int n, const wchar_t *pattern, int plen,
                                   bool case_sensitive, bool normalize, bool forward, bool with_pos) {
    (void)normalize;
    if (plen == 1 && pattern[0] >= 0 && pattern[0] < 128)
        return fuzzy_v2_single_bytes(s, n, (unsigned char)pattern[0],
                                     case_sensitive, forward, with_pos);
    /* Two ASCII characters stay on the fused pass. A backward query with no
       position list, which is the interactive pathname scan, skips irrelevant
       bytes. Forward queries and position lists keep the checked backtrace. */
    if (plen == 2 && pattern[0] >= 0 && pattern[0] < 128 &&
        pattern[1] >= 0 && pattern[1] < 128) {
        if (!forward && !with_pos)
            return fuzzy_v2_two_fast(s, n, (unsigned char)pattern[0],
                                     (unsigned char)pattern[1], case_sensitive);
        int max_idx = 0;
        int min_idx = ascii_index_bytes(s, n, pattern, plen, case_sensitive, &max_idx);
        if (min_idx < 0) return no_match();
        return fuzzy_v2_two_bytes(s, (unsigned char)pattern[0], (unsigned char)pattern[1],
                                  min_idx, max_idx, case_sensitive, forward, with_pos);
    }
    for (int i = 0; i < plen; i++) {
        if (pattern[i] < 0 || pattern[i] >= 128) return no_match();
    }
    if (plen == 3 && !forward && !with_pos)
        return fuzzy_v2_three_fast(s, n, pattern, case_sensitive);
    int max_idx = 0;
    int min_idx = ascii_index_bytes(s, n, pattern, plen, case_sensitive, &max_idx);
    if (min_idx < 0) return no_match();
    return fuzzy_v2_ascii_n(s, min_idx, max_idx, pattern, plen, case_sensitive, forward, with_pos);
}

int mm_ascii_n_rank(mm_item_t *items, const int *cands, int ncand,
                    int range_lo, int begin, int end, int item_count,
                    int header_lines, const bool *excluded,
                    const wchar_t *pat, int plen,
                    bool case_sensitive, bool forward,
                    int key_mode, mm_hit_key_t *out, int *slow) {
    if (slow) *slow = 0;
    if (!items || !out || !pat || plen <= 0 || end < begin) return 0;
    int n = 0;
    for (int v = begin; v < end; v++) {
        int idx;
        if (cands && v < ncand) idx = cands[v];
        else {
            int tail = cands ? v - ncand : v;
            idx = range_lo + tail;
        }
        if (idx < header_lines || idx >= item_count) continue;
        if (excluded && excluded[idx]) continue;
        if (v + 8 < end) {
            int ahead_v = v + 8;
            int ahead;
            if (cands && ahead_v < ncand) ahead = cands[ahead_v];
            else ahead = range_lo + (cands ? ahead_v - ncand : ahead_v);
            if (ahead >= header_lines && ahead < item_count) {
                const char *nxt = items[ahead].text.bytes;
                if (nxt) __builtin_prefetch(nxt, 0, 3);
            }
        }
        mm_item_t *it = &items[idx];
        if (!(it->text.ascii && it->text.bytes && !it->text.decoded)) {
            out[n].key = UINT64_MAX;
            out[n].index = idx;
            n++;
            if (slow) (*slow)++;
            continue;
        }
        const unsigned char *s = (const unsigned char *)it->text.bytes;
        int len = it->text.length;
        if (len < plen) continue;
        if ((int64_t)len * (int64_t)plen > 100000) {
            out[n].key = UINT64_MAX;
            out[n].index = idx;
            n++;
            if (slow) (*slow)++;
            continue;
        }
        mm_match_t m = fuzzy_v2_on_bytes(s, len, pat, plen, case_sensitive,
                                        false, forward, false);
        if (m.result.start < 0) continue;
        int slash = -1;
        uint16_t trim = 0;
        if (key_mode != 0) {
            trim = ascii1_trim(it, s, len);
            if (key_mode == 2) slash = last_slash_of(s, len);
        }
        out[n].key = key_mode == 0 ? 0 :
                     ascii1_pack_key(key_mode, m.result.score, m.result.start,
                                    slash, trim);
        out[n].index = it->index;
        n++;
    }
    return n;
}

bool mm_fuzzy_match_ascii(const unsigned char *s, int n, const wchar_t *pattern, int plen,
                          bool case_sensitive, bool forward,
                          int *start, int *end, int *score) {
    if (!s || n <= 0 || !pattern || plen <= 0 || !start || !end || !score) return false;
    mm_match_t m = fuzzy_v2_on_bytes(s, n, pattern, plen, case_sensitive, false, forward, false);
    if (m.result.start < 0) return false;
    *start = m.result.start;
    *end = m.result.end;
    *score = m.result.score;
    return true;
}

mm_match_t mm_fuzzy_match_v2(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos) {
    int M = plen;
    if (M == 0) {
        mm_match_t m = {0};
        return m;
    }
    int N = text->length;
    if (M > N) return no_match();

    /* fall back to V1 for large inputs / long patterns */
    if ((int64_t)N * M > 100000 || M > 1000) {
        if (text->bytes && !text->runes) {
            const wchar_t *wide = widen_ascii(text->bytes, N);
            if (!wide) return no_match();
            mm_text_t as_runes = { wide, N, NULL };
            return mm_fuzzy_match_v1(&as_runes, pattern, plen, case_sensitive, normalize,
                                    forward, with_pos);
        }
        return mm_fuzzy_match_v1(text, pattern, plen, case_sensitive, normalize, forward, with_pos);
    }

    if (text->bytes && !text->runes)
        return fuzzy_v2_on_bytes(text->bytes, N, pattern, plen, case_sensitive, normalize,
                                 forward, with_pos);

    const wchar_t *input = text->runes;

    /* Single-character ASCII pattern: pick the highest-bonus occurrence.
       Normalization does not apply to bytes below 128, same as fzf. */
    if (M == 1 && pattern[0] < 128 && text_is_ascii(input, N)) {
        wchar_t b = pattern[0];
        int16_t maxScore = 0; int maxScorePos = -1;
        if (!forward) {
            /* Same result as the forward scan's ">=" update: the rightmost
               occurrence of the best bonus. Stop once that bonus is found. */
            int16_t best = max_first_bonus((unsigned char)b, case_sensitive);
            for (int idx = N - 1; idx >= 0; idx--) {
                wchar_t ch = input[idx];
                bool eq = (ch == b);
                if (!eq && !case_sensitive && b >= 'a' && b <= 'z' && ch >= 'A' && ch <= 'Z')
                    eq = ((wchar_t)(ch + 32) == b);
                if (!eq) continue;
                char_class cls = asciiCharClasses[(unsigned char)(ch < 128 ? ch : 0)];
                char_class prevClass = initialCharClass;
                if (idx > 0) {
                    wchar_t pc = input[idx - 1];
                    prevClass = asciiCharClasses[(unsigned char)(pc < 128 ? pc : 0)];
                }
                int16_t bonus = bonusMatrix[prevClass][cls];
                int16_t score = scoreMatch + bonus * bonusFirstCharMultiplier;
                if (score > maxScore) {
                    maxScore = score;
                    maxScorePos = idx;
                    if (bonus >= best) break;
                }
            }
        } else {
            for (int idx = 0; idx < N; ) {
                /* trySkip */
                int found = -1;
                for (int i = idx; i < N; i++) {
                    wchar_t ch = input[i];
                    bool eq = (ch == b);
                    if (!eq && !case_sensitive && b >= 'a' && b <= 'z' && ch >= 'A' && ch <= 'Z')
                        eq = ((wchar_t)(ch + 32) == b);
                    if (eq) { found = i; break; }
                }
                if (found < 0) break;
                idx = found;
                char_class cls = asciiCharClasses[(unsigned char)(input[idx] < 128 ? input[idx] : 0)];
                char_class prevClass = initialCharClass;
                if (idx > 0) {
                    wchar_t pc = input[idx - 1];
                    prevClass = asciiCharClasses[(unsigned char)(pc < 128 ? pc : 0)];
                }
                int16_t bonus = bonusMatrix[prevClass][cls];
                int16_t score = scoreMatch + bonus * bonusFirstCharMultiplier;
                if (score > maxScore) {
                    maxScore = score; maxScorePos = idx;
                    if (bonus >= bonusBoundary) break;
                }
                idx++;
            }
        }
        if (maxScorePos < 0) return no_match();
        mm_match_t m = no_match();
        m.result.start = maxScorePos;
        m.result.end = maxScorePos + 1;
        m.result.score = maxScore;
        if (with_pos) {
            m.pos = malloc(sizeof(int));
            if (m.pos) { m.pos[0] = maxScorePos; m.npos = 1; }
        }
        return m;
    }

    int maxIdx = N;
    int minIdx = 0;
    if (text_is_ascii(input, N)) {
        minIdx = ascii_fuzzy_index(input, N, pattern, M, case_sensitive, &maxIdx);
        if (minIdx < 0) return no_match();
        N = maxIdx - minIdx;
        if (N <= 0) return no_match();
    }

    if (!match_ws_reserve(N, M)) return no_match();
    int16_t *H0 = g_H0, *C0 = g_C0, *B = g_B;
    int *F = g_F;
    wchar_t *T = g_T;
    mm_match_t out = no_match();

    for (int i = 0; i < N; i++) T[i] = input[minIdx + i];

    int16_t maxScore = 0;
    int maxScorePos = 0;
    int pidx = 0, lastIdx = 0;
    wchar_t pchar0 = pattern[0], pchar = pattern[0];
    int16_t prevH0 = 0;
    char_class prevClass = initialCharClass;
    bool inGap = false;

    for (int off = 0; off < N; off++) {
        wchar_t ch = T[off];
        char_class cls;
        if (ch < 128) {
            cls = asciiCharClasses[ch];
            if (!case_sensitive && cls == charUpper) { ch += 32; T[off] = ch; }
        } else {
            cls = char_class_of_non_ascii(ch);
            if (!case_sensitive && cls == charUpper) ch = lower_rune(ch);
            if (normalize) ch = mm_algo_normalize_rune(ch);
            T[off] = ch;
        }
        int16_t bonus = bonusMatrix[prevClass][cls];
        B[off] = bonus;
        prevClass = cls;

        if (ch == pchar) {
            if (pidx < M) { F[pidx] = off; pidx++; pchar = pattern[pidx < M ? pidx : M - 1]; }
            lastIdx = off;
        }

        if (ch == pchar0) {
            int16_t score = scoreMatch + bonus * bonusFirstCharMultiplier;
            H0[off] = score;
            C0[off] = 1;
            if (M == 1 && ((forward && score > maxScore) || (!forward && score >= maxScore))) {
                maxScore = score; maxScorePos = off;
                if (forward && bonus >= bonusBoundary) { inGap = false; prevH0 = H0[off]; break; }
            }
            inGap = false;
        } else {
            if (inGap) H0[off] = prevH0 + scoreGapExtension > 0 ? prevH0 + scoreGapExtension : 0;
            else H0[off] = prevH0 + scoreGapStart > 0 ? prevH0 + scoreGapStart : 0;
            C0[off] = 0;
            inGap = true;
        }
        prevH0 = H0[off];
    }
    if (pidx != M) goto cleanup;

    if (M == 1) {
        out.result.start = minIdx + maxScorePos;
        out.result.end = minIdx + maxScorePos + 1;
        out.result.score = maxScore;
        if (with_pos) {
            out.pos = malloc(sizeof(int));
            if (out.pos) { out.pos[0] = minIdx + maxScorePos; out.npos = 1; }
        }
        goto ok;
    }

    int f0 = F[0];
    int width = lastIdx - f0 + 1;
    int16_t *H = g_H;
    int16_t *C = g_C;
    if (!H || !C) goto cleanup;
    /* H and C are reused between items.  Every cell the algorithm reads
       has been written earlier in the same match, so they do not have to be
       cleared here. */
    for (int i = 0; i <= lastIdx - f0; i++) { H[i] = H0[f0 + i]; C[i] = C0[f0 + i]; }

    for (int off = 1; off < M; off++) {
        int f = F[off];
        wchar_t pch = pattern[off];
        int row = off * width;
        bool gap = false;
        for (int coloff = 0; coloff < lastIdx - f + 1; coloff++) {
            int col = coloff + f;
            int16_t s1 = 0, s2, consecutive = 0;
            int idxInRow = row + col - f0;
            int idxDiag = row + col - f0 - 1 - width;
            int idxLeft = row + col - f0 - 1;
            if (coloff == 0) H[idxLeft] = 0;
            s2 = H[idxLeft] + (gap ? scoreGapExtension : scoreGapStart);
            if (pch == T[col]) {
                s1 = H[idxDiag] + scoreMatch;
                int16_t b = B[col];
                consecutive = C[idxDiag] + 1;
                if (consecutive > 1) {
                    int16_t fb = B[col - consecutive + 1];
                    if (b >= bonusBoundary && b > fb) consecutive = 1;
                    else {
                        int16_t m = b;
                        if (bonusConsecutive > m) m = bonusConsecutive;
                        if (fb > m) m = fb;
                        b = m;
                    }
                }
                if (s1 + b < s2) { s1 += B[col]; consecutive = 0; }
                else s1 += b;
            }
            C[idxInRow] = consecutive;
            gap = s1 < s2;
            int16_t score = s1 > s2 ? s1 : s2;
            if (score < 0) score = 0;
            if (off == M - 1 && ((forward && score > maxScore) || (!forward && score >= maxScore))) {
                maxScore = score; maxScorePos = col;
            }
            H[idxInRow] = score;
        }
    }

    /* Walk the winning alignment even when the caller does not want the
       position list. result.start has to be the first matched rune; leaving
       it at F[0] makes begin/end/chunk/pathname tiebreaks disagree with fzf. */
    int j = f0;
    {
        int i = M - 1;
        j = maxScorePos;
        bool preferMatch = true;
        int np = 0;
        if (with_pos) {
            out.pos = malloc((size_t)M * sizeof(int));
            if (!out.pos) goto cleanup;
        }
        for (;;) {
            int I = i * width;
            int j0 = j - f0;
            int16_t s = H[I + j0];
            int16_t s1 = -32768, s2 = -32768;
            if (i > 0 && j >= F[i]) s1 = H[I - width + j0 - 1];
            if (j > F[i]) s2 = H[I + j0 - 1];
            int row = i;
            if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
                if (with_pos) out.pos[np++] = j + minIdx;
                if (i == 0) break;
                i--;
            }
            preferMatch = C[I + j0] > 1 ||
                (row + 1 < M && j < lastIdx && (j + 1) >= F[row + 1] && C[I + width + j0 + 1] > 0);
            j--;
        }
        if (with_pos) out.npos = np;
    }
    out.result.start = minIdx + j;
    out.result.end = minIdx + maxScorePos + 1;
    out.result.score = maxScore;

ok:
    /* H and C belong to the shared workspace; they are not released here. */
    return out;
cleanup:
    free(out.pos); out.pos = NULL;
    return no_match();
}

/* ---------------- FuzzyMatchV1 ---------------- */

mm_match_t mm_fuzzy_match_v1(const mm_text_t *text, const wchar_t *pattern, int plen,
                             bool case_sensitive, bool normalize, bool forward, bool with_pos) {
    if (plen == 0) { mm_match_t m = {0}; return m; }
    int N = text->length;
    const wchar_t *t = text_as_runes(text);
    if (N > 0 && !t) return no_match();
    int maxIdx = 0;
    if (text_is_ascii(t, N)) {
        if (ascii_fuzzy_index(t, N, pattern, plen, case_sensitive, &maxIdx) < 0) return no_match();
    } else {
        maxIdx = N;
    }

    int pidx = 0, sidx = -1, eidx = -1;
    for (int index = 0; index < N; index++) {
        wchar_t ch = t[index_at(index, N, forward)];
        if (!case_sensitive) ch = lower_rune(ch);
        if (normalize) ch = mm_algo_normalize_rune(ch);
        wchar_t pch = pattern[index_at(pidx, plen, forward)];
        if (ch == pch) {
            if (sidx < 0) sidx = index;
            if (++pidx == plen) { eidx = index + 1; break; }
        }
    }
    if (sidx >= 0 && eidx >= 0) {
        pidx--;
        for (int index = eidx - 1; index >= sidx; index--) {
            int tidx = index_at(index, N, forward);
            wchar_t ch = t[tidx];
            if (!case_sensitive) ch = lower_rune(ch);
            if (normalize) ch = mm_algo_normalize_rune(ch);
            wchar_t pch = pattern[index_at(pidx, plen, forward)];
            if (ch == pch) { if (--pidx < 0) { sidx = index; break; } }
        }
        if (!forward) { int s2 = N - eidx; eidx = N - sidx; sidx = s2; }
        mm_match_t m = no_match();
        int *pos = with_pos ? malloc((size_t)plen * sizeof(int)) : NULL;
        if (with_pos && !pos) return no_match();
        int np = 0;
        m.result.score = calculate_score(t, N, pattern, plen, sidx, eidx, case_sensitive, normalize, pos, &np);
        m.result.start = sidx;
        m.result.end = eidx;
        m.pos = pos; m.npos = np;
        return m;
    }
    return no_match();
}

/* ---------------- ExactMatch ---------------- */

static mm_match_t exact_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                              bool case_sensitive, bool normalize, bool forward,
                              bool with_pos, bool boundaryCheck) {
    (void)with_pos;
    if (plen == 0) { mm_match_t m = {0}; return m; }
    int N = text->length;
    const wchar_t *t = text_as_runes(text);
    if (N > 0 && !t) return no_match();
    if (N < plen) return no_match();
    int maxIdx = 0;
    if (text_is_ascii(t, N)) {
        if (ascii_fuzzy_index(t, N, pattern, plen, case_sensitive, &maxIdx) < 0) return no_match();
    } else {
        maxIdx = N;
    }

    int pidx = 0;
    int bestPos = -1;
    int16_t bonus = 0, bbonus = 0, bestBonus = -1;
    for (int index = 0; index < N; index++) {
        int index_ = index_at(index, N, forward);
        wchar_t ch = t[index_];
        if (!case_sensitive) ch = lower_rune(ch);
        if (normalize) ch = mm_algo_normalize_rune(ch);
        int pidx_ = index_at(pidx, plen, forward);
        wchar_t pch = pattern[pidx_];
        bool ok = (pch == ch);
        if (ok) {
            if (pidx_ == 0) bonus = bonus_at(t, index_);
            if (boundaryCheck) {
                if (forward && pidx_ == 0) bbonus = bonus;
                else if (!forward && pidx_ == plen - 1) {
                    if (index_ < N - 1) bbonus = bonus_at(t, index_ + 1);
                    else bbonus = bonusBoundaryWhite;
                }
                ok = bbonus >= bonusBoundary;
                if (ok && pidx_ == 0) ok = (index_ == 0) || char_class_of(t[index_ - 1]) <= charDelimiter;
                if (ok && pidx_ == plen - 1) ok = (index_ == N - 1) || char_class_of(t[index_ + 1]) <= charDelimiter;
            }
        }
        if (ok) {
            pidx++;
            if (pidx == plen) {
                if (bonus > bestBonus) { bestPos = index; bestBonus = bonus; }
                if (bonus >= bonusBoundary) break;
                index -= pidx - 1;
                pidx = 0; bonus = 0;
            }
        } else {
            index -= pidx;
            pidx = 0; bonus = 0;
        }
    }
    if (bestPos >= 0) {
        int sidx, eidx;
        if (forward) { sidx = bestPos - plen + 1; eidx = bestPos + 1; }
        else { sidx = N - (bestPos + 1); eidx = N - (bestPos - plen + 1); }
        int score;
        if (boundaryCheck) {
            score = bonus;
            int deduct = (bonus - bonusBoundary) + 1;
            if (sidx > 0 && t[sidx - 1] == '_') { score -= deduct + 1; deduct = 1; }
            if (eidx < N && t[eidx] == '_') score -= deduct;
            score += scoreMatch * plen + (int)bonusBoundaryWhite * (plen + 1);
        } else {
            int np = 0;
            score = calculate_score(t, N, pattern, plen, sidx, eidx, case_sensitive, normalize, NULL, &np);
        }
        mm_match_t m = no_match();
        m.result.start = sidx; m.result.end = eidx; m.result.score = score;
        return m;
    }
    return no_match();
}

mm_match_t mm_exact_match_naive(const mm_text_t *text, const wchar_t *pattern, int plen,
                                bool cs, bool norm, bool fwd, bool with_pos) {
    return exact_match(text, pattern, plen, cs, norm, fwd, with_pos, false);
}
mm_match_t mm_exact_match_boundary(const mm_text_t *text, const wchar_t *pattern, int plen,
                                   bool cs, bool norm, bool fwd, bool with_pos) {
    return exact_match(text, pattern, plen, cs, norm, fwd, with_pos, true);
}

/* ---------------- Prefix / Suffix / Equal ---------------- */

mm_match_t mm_prefix_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                           bool cs, bool norm, bool fwd, bool with_pos) {
    (void)fwd; (void)with_pos;
    if (plen == 0) { mm_match_t m = {0}; return m; }
    int N = text->length;
    const wchar_t *t = text_as_runes(text);
    if (N > 0 && !t) return no_match();
    int trimmedLen = 0;
    if (!is_space_unicode(pattern[0])) trimmedLen = leading_ws(t, N);
    if (N - trimmedLen < plen) return no_match();
    for (int i = 0; i < plen; i++) {
        wchar_t ch = t[trimmedLen + i];
        if (!cs) ch = lower_rune(ch);
        if (norm) ch = mm_algo_normalize_rune(ch);
        if (ch != pattern[i]) return no_match();
    }
    mm_match_t m = no_match();
    int np = 0;
    m.result.score = calculate_score(t, N, pattern, plen, trimmedLen, trimmedLen + plen, cs, norm, NULL, &np);
    m.result.start = trimmedLen; m.result.end = trimmedLen + plen;
    return m;
}

mm_match_t mm_suffix_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                           bool cs, bool norm, bool fwd, bool with_pos) {
    (void)fwd; (void)with_pos;
    int N = text->length;
    const wchar_t *t = text_as_runes(text);
    if (N > 0 && !t) return no_match();
    int trimmedLen = N;
    if (plen == 0 || !is_space_unicode(pattern[plen - 1])) trimmedLen -= trailing_ws(t, N);
    if (plen == 0) { mm_match_t m = {0}; m.result.start = trimmedLen; m.result.end = trimmedLen; return m; }
    int diff = trimmedLen - plen;
    if (diff < 0) return no_match();
    for (int i = 0; i < plen; i++) {
        wchar_t ch = t[i + diff];
        if (!cs) ch = lower_rune(ch);
        if (norm) ch = mm_algo_normalize_rune(ch);
        if (ch != pattern[i]) return no_match();
    }
    mm_match_t m = no_match();
    int np = 0;
    int sidx = trimmedLen - plen;
    m.result.score = calculate_score(t, N, pattern, plen, sidx, trimmedLen, cs, norm, NULL, &np);
    m.result.start = sidx; m.result.end = trimmedLen;
    return m;
}

mm_match_t mm_equal_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                          bool cs, bool norm, bool fwd, bool with_pos) {
    (void)fwd; (void)with_pos;
    int N = text->length;
    const wchar_t *t = text_as_runes(text);
    if (N > 0 && !t) return no_match();
    if (plen == 0) return no_match();
    int trimmedLen = 0;
    if (!is_space_unicode(pattern[0])) trimmedLen = leading_ws(t, N);
    int trimmedEndLen = 0;
    if (!is_space_unicode(pattern[plen - 1])) trimmedEndLen = trailing_ws(t, N);
    if (N - trimmedLen - trimmedEndLen != plen) return no_match();
    bool match = true;
    for (int i = 0; i < plen && match; i++) {
        wchar_t ch = t[trimmedLen + i];
        if (!cs) ch = lower_rune(ch);
        if (norm) {
            if (mm_algo_normalize_rune(pattern[i]) != mm_algo_normalize_rune(ch)) match = false;
        } else {
            if (pattern[i] != ch) match = false;
        }
    }
    if (match) {
        mm_match_t m = no_match();
        m.result.start = trimmedLen;
        m.result.end = trimmedLen + plen;
        m.result.score = (scoreMatch + bonusBoundaryWhite) * plen +
                         (bonusFirstCharMultiplier - 1) * bonusBoundaryWhite;
        return m;
    }
    return no_match();
}

mm_match_t mm_fuzzy_match(const mm_text_t *text, const wchar_t *pattern, int plen,
                          bool cs, bool norm, bool forward, bool with_pos) {
    if (selected_fuzzy_version == 1)
        return mm_fuzzy_match_v1(text, pattern, plen, cs, norm, forward, with_pos);
    return mm_fuzzy_match_v2(text, pattern, plen, cs, norm, forward, with_pos);
}

void mm_match_free(mm_match_t *m) {
    free(m->pos);
    m->pos = NULL;
}
