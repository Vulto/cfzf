/* tokenizer.h - nth ranges, delimiters, awk/string tokenization, transform */
#ifndef MMENU_TOKENIZER_H
#define MMENU_TOKENIZER_H

#include "util.h"
#include <stdbool.h>

#define MM_RANGE_ELLIPSIS 0

typedef struct { int begin; int end; } mm_range_t;

/* Parse one nth-expression token (between commas). Returns false on error. */
bool mm_parse_range(const char *str, mm_range_t *out);

/* Parse a full --nth / --with-nth spec "1,3..5,-1" into an array. Caller frees
   *out. Returns false on error. */
bool mm_parse_ranges(const char *str, mm_range_t **out, int *outn);

typedef struct {
    const char *str; /* delimiter string (NULL => awk-style) */
    int slen;
    bool is_regex;   /* true when the delimiter is a regular expression */
    void *rx;        /* compiled regex_t* when is_regex is true */
} mm_delimiter_t;

/* Build a delimiter from the --delimiter value, following fzf's rules
   (src/options.go delimiterRegexp): a single character or a pattern without
   special characters is a literal delimiter, otherwise the value is compiled
   as a regular expression. Returns false on allocation failure. */
bool mm_delimiter_init(mm_delimiter_t *delim, const char *spec);

/* Release resources held by a delimiter. */
void mm_delimiter_free(mm_delimiter_t *delim);

/* Split text into tokens. Tokens are malloc'd. If awk (str==NULL), uses \S+\s*.
   *prefix_lengths[i] gets the rune length preceding token i. Caller frees. */
char **mm_tokenize(const char *text, mm_delimiter_t delim, int **prefix_lengths, int *outn);

/* Remove a trailing delimiter (string delimiter => TrimSuffix; awk =>
   TrimRight whitespace). Returns a newly malloc'd string. */
char *mm_strip_last_delimiter(const char *str, mm_delimiter_t delim);

/* Validate and expand fzf --accept-nth field expressions/templates. */
bool mm_accept_nth_validate(const char *spec);
char *mm_accept_nth_expand(const char *spec, const char *line,
                           mm_delimiter_t delim, int index);

/* Transform tokens according to withNth ranges, producing new token strings
   (malloc'd) and their output prefix lengths. Caller frees. */
char **mm_transform(char **tokens, int ntokens, int *tok_prefix, const mm_range_t *ranges,
                    int nranges, int **prefix_lengths, int *outn);

/* Transform a complete record through --with-nth ranges. Caller owns result. */
char *mm_transform_line(const char *line, mm_delimiter_t delim,
                        const mm_range_t *ranges, int nranges);

#endif /* MMENU_TOKENIZER_H */
