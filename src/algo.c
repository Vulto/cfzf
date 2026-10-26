/* algo.c - see algo.h. Ported from fzf src/algo/algo.go (v0.74.3). */
#include "algo.h"
#include <stdlib.h>
#include <string.h>
#include <wchar.h>
#include <wctype.h>

static int selected_fuzzy_version = 2;

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
        bonusBoundaryWhite = bonusBoundary + 2;
        bonusBoundaryDelimiter = bonusBoundary + 1;
        initialCharClass = charWhite;
    } else if (strcmp(scheme, "path") == 0) {
        bonusBoundaryWhite = bonusBoundary;
        bonusBoundaryDelimiter = bonusBoundary + 1;
        delimiterChars = "/";
        initialCharClass = charDelimiter;
    } else if (strcmp(scheme, "history") == 0) {
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
    if (n > 0 && (size_t)n > g_cap_n) {
        free(g_H0); free(g_C0); free(g_B); free(g_T);
        g_H0 = calloc((size_t)n, sizeof(int16_t));
        g_C0 = calloc((size_t)n, sizeof(int16_t));
        g_B  = calloc((size_t)n, sizeof(int16_t));
        g_T  = calloc((size_t)n, sizeof(wchar_t));
        g_cap_n = (size_t)n;
        if (!g_H0 || !g_C0 || !g_B || !g_T) {
            free(g_H0); free(g_C0); free(g_B); free(g_T);
            g_H0 = g_C0 = g_B = NULL; g_T = NULL; g_cap_n = 0;
            return false;
        }
    }
    if (m > 0 && (size_t)(n * m) > g_cap_hc) {
        free(g_H); free(g_C);
        g_H = calloc((size_t)n * (size_t)m, sizeof(int16_t));
        g_C = calloc((size_t)n * (size_t)m, sizeof(int16_t));
        g_cap_hc = (size_t)n * (size_t)m;
        if (!g_H || !g_C) {
            free(g_H); free(g_C);
            g_H = g_C = NULL; g_cap_hc = 0;
            return false;
        }
    }
    if (m > 0 && (size_t)m > g_cap_m) {
        free(g_F);
        g_F = calloc((size_t)m, sizeof(int));
        g_cap_m = (size_t)m;
        if (!g_F) { g_F = NULL; g_cap_m = 0; return false; }
    }
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
    if ((int64_t)N * M > 100000 || M > 1000)
        return mm_fuzzy_match_v1(text, pattern, plen, case_sensitive, normalize, forward, with_pos);

    const wchar_t *input = text->runes;

    /* Single-character ASCII pattern: pick the highest-bonus occurrence. */
    if (M == 1 && !normalize && pattern[0] < 128 && text_is_ascii(input, N)) {
        wchar_t b = pattern[0];
        int16_t maxScore = 0; int maxScorePos = -1;
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
            if ((forward && score > maxScore) || (!forward && score >= maxScore)) {
                maxScore = score; maxScorePos = idx;
                if (forward && bonus >= bonusBoundary) break;
            }
            idx++;
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

    int j = f0;
    if (with_pos) {
        int i = M - 1;
        j = maxScorePos;
        bool preferMatch = true;
        out.pos = malloc((size_t)M * sizeof(int));
        if (!out.pos) goto cleanup;
        int np = 0;
        for (;;) {
            int I = i * width;
            int j0 = j - f0;
            int16_t s = H[I + j0];
            int16_t s1 = -32768, s2 = -32768;
            if (i > 0 && j >= F[i]) s1 = H[I - width + j0 - 1];
            if (j > F[i]) s2 = H[I + j0 - 1];
            int row = i;
            if (s > s1 && (s > s2 || (s == s2 && preferMatch))) {
                out.pos[np++] = j + minIdx;
                if (i == 0) break;
                i--;
            }
            preferMatch = C[I + j0] > 1 ||
                (row + 1 < M && j < lastIdx && (j + 1) >= F[row + 1] && C[I + width + j0 + 1] > 0);
            j--;
        }
        out.npos = np;
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
    const wchar_t *t = text->runes;
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
    const wchar_t *t = text->runes;
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
    const wchar_t *t = text->runes;
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
    const wchar_t *t = text->runes;
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
    const wchar_t *t = text->runes;
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
