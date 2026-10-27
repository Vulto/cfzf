# FEATURES.md — fzf C23 port feature inventory

Reference: junegunn/fzf **v0.74.3** @ `15f64c492a08f0840b81540c7d1de35737448086`

Status legend: `verified` (implemented **and** covered by a differential test against
the reference), `implemented` (implemented, not yet covered by an automated
differential test), `unstarted` (not implemented).

No feature is marked `verified` on the strength of its name appearing in the code
(PLAN §2).

## Summary

Run `python3 tests/status.py` to audit the inventory. CI runs the same audit.
It reports row counts by declared status, duplicate/unexpected/missing IDs, and
whether each `verified` row names a test file. These are inventory diagnostics,
not a feature-completion percentage.

**No reliable feature-level completion percentage is available yet.** Existing
rows reference broad test suites rather than named test cases and observed
results, so a passing suite cannot establish that every linked feature was
tested. Do not treat the legacy `verified` labels below as independently
revalidated. The next measurement milestone is per-feature, per-case evidence
with results tied to a specific implementation commit and pinned reference.

## CLI options

| ID | Option | C location | Verification | Status |
|----|--------|-----------|--------------|--------|
| OPT-001 | `--accept-nth` | src/main.c, src/tokenizer.c, src/ui.c | tests/batch-diff.sh:accept-nth-regex-delimiter, tests/batch-diff.sh:accept-nth-literal-trailing-delimiter, tests/batch-diff.sh:accept-nth-ranges, tests/batch-diff.sh:accept-nth-range-expression, tests/batch-diff.sh:accept-nth-range-template, tests/batch-diff.sh:accept-nth-template, tests/batch-diff.sh:accept-nth-missing, tests/batch-diff.sh:accept-nth-invalid-range, tests/batch-diff.sh:accept-nth-template-no-placeholder, tests/pty-diff.sh:accept-nth-interactive-template | implemented but unverified |
| OPT-002 | `--algo` | src/algo.c, src/pattern.c, src/main.c | tests/algo-diff.sh:algo=v1 q=$q, tests/algo-diff.sh:algo=v1 separate argument, tests/algo-diff.sh:algo=v2 q=$q, tests/batch-diff.sh:algo-invalid, tests/batch-diff.sh:algo-missing, tests/algo-diff.sh:algo=v1 extended query, tests/algo-diff.sh:algo=v1 nth token, tests/algo-diff.sh:unicode delimiter scheme=path, tests/algo-diff.sh:unicode delimiter scheme=history | implemented but unverified |
| OPT-003 | `--ambidouble` | src/main.c | - | implemented |
| OPT-004 | `--ansi` | src/ansi.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-005 | `--bash` | src/main.c | - | implemented |
| OPT-006 | `--bench` | - | - | unstarted |
| OPT-007 | `--bind` | src/bind.c, src/ui.c | tests/pty-diff.sh | verified |
| OPT-008 | `--black` | src/main.c | - | implemented |
| OPT-009 | `\-\-border` | src/terminal.c, src/ui.c | tests/pty-diff.sh, tests/walker-diff.sh | verified |
| OPT-010 | `--border-label` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-011 | `--border-label-pos` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh:border-label-pos-left, tests/pty-diff.sh:border-label-pos-right, tests/pty-diff.sh:border-label-pos-bottom | implemented but unverified |
| OPT-012 | `--color` | src/color.c, src/ui.c | tests/pty-diff.sh | verified |
| OPT-013 | `--cycle` | src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-014 | `--delimiter` | src/tokenizer.c, src/main.c | tests/batch-diff.sh:delimiter-missing, tests/batch-diff.sh:delim-empty-rune-split, tests/batch-diff.sh:delim-regex-zero-width | implemented but unverified |
| OPT-015 | `--disabled` | src/ui.c, src/main.c | tests/batch-diff.sh:disabled-filter, tests/batch-diff.sh:disabled-nomatch, tests/pty-diff.sh:disabled-toggle-search | implemented but unverified |
| OPT-016 | `\-\-ellipsis` | src/ui.c | tests/pty-diff.sh | verified |
| OPT-017 | `--exact` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-018 | `--exit-0` | - | tests/batch-diff.sh | verified |
| OPT-019 | `--expect` | src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-020 | `--extended` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-021 | `--filepath-word` | src/main.c | - | implemented |
| OPT-022 | `--filter` | src/main.c, src/filter.c | tests/batch-diff.sh, tests/batch-diff.sh:filter-missing | implemented but unverified |
| OPT-023 | `--fish` | src/main.c | - | implemented |
| OPT-024 | `--footer` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-025 | `--footer-border` | - | - | unstarted |
| OPT-026 | `--footer-label` | - | - | unstarted |
| OPT-027 | `--footer-label-pos` | - | - | unstarted |
| OPT-028 | `--freeze-left` | - | - | unstarted |
| OPT-029 | `--freeze-right` | - | - | unstarted |
| OPT-030 | `\-\-gap` | src/ui.c, src/main.c | tests/pty-diff.sh:gap-default-layout, tests/pty-diff.sh:gap-disabled-layout, tests/batch-diff.sh:gap-default, tests/batch-diff.sh:gap-explicit-value, tests/batch-diff.sh:no-gap | implemented but unverified |
| OPT-031 | `\-\-gap\-line` | src/ui.c, src/main.c | tests/pty-diff.sh:gap-line-layout, tests/pty-diff.sh:gap-line-disabled-layout, tests/batch-diff.sh:no-gap-line, tests/batch-diff.sh:gap-line-optional | implemented but unverified |
| OPT-032 | `\-\-ghost` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh, tests/batch-diff.sh:ghost-missing | implemented but unverified |
| OPT-033 | `--graph` (not in pinned upstream) | - | tests/batch-diff.sh | n/a |
| OPT-034 | `--gutter` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-035 | `--gutter-raw` | src/main.c | tests/pty-diff.sh | verified |
| OPT-036 | `--header` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/batch-diff.sh:no-header, tests/batch-diff.sh:header-missing | implemented but unverified |
| OPT-037 | `--header-border` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-038 | `--header-first` | src/ui.c | tests/pty-diff.sh | verified |
| OPT-039 | `--header-label` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-040 | `--header-label-pos` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-041 | `--header-lines` | src/ui.c, src/main.c | tests/batch-diff.sh:header-lines-one, tests/batch-diff.sh:header-lines-two, tests/batch-diff.sh:no-header-lines | implemented but unverified |
| OPT-042 | `--header-lines-border` | - | - | unstarted |
| OPT-043 | `--height` | src/ui.c, src/terminal.c | tests/pty-diff.sh | verified |
| OPT-044 | `--highlight-line` | src/ui.c | tests/pty-diff.sh | verified |
| OPT-045 | `\-\-history` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh, tests/batch-diff.sh:history-missing, tests/batch-diff.sh:no-history | implemented but unverified |
| OPT-046 | `\-\-history\-size` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh, tests/batch-diff.sh:history-size-invalid, tests/batch-diff.sh:history-size-zero | implemented but unverified |
| OPT-047 | `--hscroll-off` | src/ui.c, src/main.c | tests/batch-diff.sh:hscroll-off-negative | implemented but unverified |
| OPT-048 | `--id-nth` | src/main.c, src/ui.c, src/ui.h, src/tokenizer.c | tests/batch-diff.sh:id-nth-missing, tests/batch-diff.sh:id-nth-invalid, tests/pty-diff.sh:id-nth-reload-track, tests/pty-diff.sh:id-nth-reload-selection | implemented but unverified |
| OPT-049 | `--ignore-case` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-050 | `--info[=default/inline-right/hidden]` | src/main.c, src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-051 | `--info-command` | src/ui.c | tests/pty-diff.sh | verified |
| OPT-052 | `--input-border` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-053 | `--input-label` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-054 | `--input-label-pos` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-055 | `--jump-labels` | src/main.c | - | implemented |
| OPT-056 | `\-\-keep\-right` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-057 | `\-\-layout` | src/ui.c | tests/pty-diff.sh | verified |
| OPT-058 | `--line-number` (not in pinned upstream) | - | tests/batch-diff.sh | n/a |
| OPT-059 | `--list-border` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-060 | `--listen` | src/main.c | - | implemented |
| OPT-061 | `--listen-unsafe` | src/main.c | - | implemented |
| OPT-062 | `--list-label` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-063 | `--list-label-pos` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-064 | `--literal` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-065 | `--margin` | src/ui.c, src/main.c | tests/pty-diff.sh | implemented but unverified |
| OPT-066 | `\-\-marker` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-067 | `--marker-multi-line` | src/main.c | tests/batch-diff.sh:marker-multi-line-missing | implemented but unverified |
| OPT-068 | `\-\-min\-height` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-069 | `--multi` | src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-070 | `--no` (not in pinned upstream) | - | tests/batch-diff.sh | n/a |
| OPT-071 | `\-\-no\-bold` | src/ui.c, src/color.c | tests/pty-diff.sh | verified |
| OPT-072 | `--no-clear` | src/ui.c, src/main.c | - | implemented |
| OPT-073 | `\-\-no\-color` | src/ui.c, src/color.c | tests/pty-diff.sh | verified |
| OPT-074 | `--no-expect` | - | tests/batch-diff.sh | verified |
| OPT-075 | `--no-extended` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-076 | `--no-header-lines-border` | - | - | unstarted |
| OPT-077 | `--no-hscroll` | src/main.c, src/ui.c | tests/pty-diff.sh:no-hscroll-match-visible | implemented but unverified |
| OPT-078 | `--no-ignore-case` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-079 | `--no-info` | - | tests/batch-diff.sh | verified |
| OPT-080 | `--no-input` | src/main.c | - | implemented |
| OPT-081 | `\-\-no\-mouse` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-082 | `--no-multi` | - | tests/batch-diff.sh | verified |
| OPT-083 | `--no-multi-line` | src/main.c | tests/pty-diff.sh | verified |
| OPT-084 | `--no-scrollbar` | src/ui.c, src/main.c | - | implemented |
| OPT-085 | `--no-separator` | src/main.c | - | implemented |
| OPT-086 | `--no-sort` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-087 | `--no-tty-default` | src/main.c | tests/pty-diff.sh | verified |
| OPT-088 | `--no-unicode` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-089 | `--nth` | src/tokenizer.c, src/main.c | tests/batch-diff.sh:nth-missing, tests/batch-diff.sh:nth-empty-range, tests/batch-diff.sh:nth-empty-component, tests/batch-diff.sh:nth-trailing-comma, tests/batch-diff.sh:nth-overflow, tests/batch-diff.sh:nth-range-overflow | implemented but unverified |
| OPT-090 | `--nushell` | - | - | unstarted |
| OPT-091 | `--oneline` / `--no-oneline` (not in pinned upstream) | - | tests/batch-diff.sh | n/a |
| OPT-092 | `--padding` | src/ui.c, src/main.c | tests/pty-diff.sh | implemented but unverified |
| OPT-093 | `\-\-pointer` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-094 | `--popup` | - | - | unstarted |
| OPT-095 | `\-\-preview` | src/preview.c, src/ui.c, src/main.c | tests/pty-diff.sh, tests/batch-diff.sh:preview-missing, tests/batch-diff.sh:no-preview | implemented but unverified |
| OPT-096 | `--preview-border` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-097 | `--preview-label` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-098 | `--preview-label-pos` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-099 | `\-\-preview\-window` | src/preview.c, src/ui.c, src/main.c | tests/pty-diff.sh, tests/batch-diff.sh:preview-window-missing | implemented but unverified |
| OPT-100 | `--preview-wrap-sign` | src/main.c, src/ui.c, src/ui.h | tests/pty-diff.sh | implemented but unverified |
| OPT-101 | `--print0` | src/main.c | tests/batch-diff.sh | verified |
| OPT-102 | `--print-query` | src/main.c | tests/batch-diff.sh | verified |
| OPT-103 | `--prompt` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/batch-diff.sh:prompt-missing | implemented but unverified |
| OPT-104 | `--query` | src/main.c | tests/batch-diff.sh, tests/batch-diff.sh:query-missing | implemented but unverified |
| OPT-105 | `--raw` | src/main.c | - | implemented |
| OPT-106 | `--read0` | src/main.c | tests/batch-diff.sh | verified |
| OPT-107 | `--reverse` | src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | verified |
| OPT-108 | `--scheme` | src/result.c, src/main.c | tests/batch-diff.sh, tests/batch-diff.sh:scheme-missing | implemented but unverified |
| OPT-109 | `--scrollbar` | src/ui.c, src/main.c | - | implemented |
| OPT-110 | `--scroll-off` | src/ui.c, src/main.c | tests/batch-diff.sh:scroll-off-invalid | implemented but unverified |
| OPT-111 | `--select-1` | - | tests/batch-diff.sh | verified |
| OPT-112 | `--separator` | src/ui.c, src/main.c | - | implemented |
| OPT-113 | `--smart-case` | src/pattern.c | tests/batch-diff.sh | verified |
| OPT-114 | `--style` | src/main.c, src/ui.c | tests/batch-diff.sh, tests/pty-diff.sh | implemented but unverified |
| OPT-115 | `\-\-sync` | src/ui.c, src/main.c | tests/pty-diff.sh | verified |
| OPT-116 | `--tabstop` | src/ui.c, src/main.c | tests/batch-diff.sh:tabstop-zero | implemented but unverified |
| OPT-117 | `--tac` | src/result.c | tests/batch-diff.sh | verified |
| OPT-118 | `--tail` | src/main.c | tests/batch-diff.sh, tests/batch-diff.sh:tail-missing, tests/batch-diff.sh:tail-one, tests/batch-diff.sh:no-tail | implemented but unverified |
| OPT-119 | `--threads` | src/main.c, src/filter.c | tests/thread-diff.sh:--threads=2, tests/thread-diff.sh:--threads=4, tests/batch-diff.sh:threads-zero-default | implemented but unverified |
| OPT-120 | `--tiebreak` | src/result.c, src/main.c | tests/batch-diff.sh, tests/batch-diff.sh:tiebreak-missing | implemented but unverified |
| OPT-121 | `--tmux` | - | - | unstarted |
| OPT-122 | `--track` | src/ui.c, src/main.c | tests/pty-diff.sh:track-current-item, tests/pty-diff.sh:action-reload-track, tests/pty-diff.sh:id-nth-reload-track | implemented but unverified |
| OPT-123 | `\-\-walker` | src/walker.c, src/main.c | tests/pty-diff.sh, tests/walker-diff.sh, tests/batch-diff.sh:walker-missing | implemented but unverified |
| OPT-124 | `\-\-walker\-root` | src/walker.c | tests/pty-diff.sh, tests/walker-diff.sh | verified |
| OPT-125 | `\-\-walker\-skip` | src/walker.c, src/main.c | tests/pty-diff.sh, tests/walker-diff.sh, tests/batch-diff.sh:walker-skip-missing | implemented but unverified |
| OPT-126 | `--with-nth` | src/tokenizer.c, src/main.c | tests/batch-diff.sh:with-nth-missing, tests/batch-diff.sh:with-nth-empty-component | implemented but unverified |
| OPT-127 | `\-\-with\-shell` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-128 | `--wrap` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-129 | `--wrap-sign` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-130 | `--wrap-word` | src/ui.c, src/main.c | tests/pty-diff.sh, tests/manpage.sh | verified |
| OPT-131 | `--zsh` | - | - | unstarted |

## Actions

| ID | Action | C location | Verification | Status |
|----|--------|-----------|--------------|--------|
| ACT-001 | `accept` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-002 | `accept-non-empty` | src/bind.c, src/ui.c | - | implemented |
| ACT-003 | `accept-or-print-query` | src/bind.c, src/ui.c | - | implemented |
| ACT-004 | `backward-char` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-backward-char | implemented but unverified |
| ACT-005 | `backward-delete-char` | src/bind.c, src/ui.c | tests/pty-diff.sh:query-cursor-backspace | implemented but unverified |
| ACT-006 | `backward-kill-subword` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-backward-kill-subword | implemented but unverified |
| ACT-007 | `backward-kill-word` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-backward-kill-word | implemented but unverified |
| ACT-008 | `backward-subword` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-backward-subword | implemented but unverified |
| ACT-009 | `backward-word` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-backward-word | implemented but unverified |
| ACT-010 | `become` | src/bind.c, src/ui.c, src/ui.h | tests/pty-diff.sh:bind-become-command | implemented but unverified |
| ACT-011 | `bell` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-012 | `best` | - | - | unstarted |
| ACT-013 | `bg-cancel` | src/bind.c, src/ui.c, src/ui.h | tests/pty-diff.sh:bind-bg-cancel | implemented but unverified |
| ACT-014 | `cancel` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-015 | `change-border-label` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-border-label | implemented but unverified |
| ACT-016 | `change-footer` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-footer | implemented but unverified |
| ACT-017 | `change-footer-label` | - | - | unstarted |
| ACT-018 | `change-ghost` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-ghost | implemented but unverified |
| ACT-019 | `change-header` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-hide-show-header | implemented but unverified |
| ACT-020 | `change-header-label` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-header-label | implemented but unverified |
| ACT-021 | `change-header-lines` | - | - | unstarted |
| ACT-022 | `change-input-label` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-input-label | implemented but unverified |
| ACT-023 | `change-list-label` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-list-label | implemented but unverified |
| ACT-024 | `change-multi` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-025 | `change-nth` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-nth | implemented but unverified |
| ACT-026 | `change-pointer` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-027 | `change-preview` | src/bind.c, src/ui.c | - | implemented |
| ACT-028 | `change-preview-label` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-change-preview-label | implemented but unverified |
| ACT-029 | `change-preview-window` | src/bind.c, src/ui.c | - | implemented |
| ACT-030 | `change-prompt` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-031 | `change-query` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-transform-change-query | implemented but unverified |
| ACT-032 | `change-with-nth` | - | - | unstarted |
| ACT-033 | `clear-multi` | src/bind.c, src/ui.c | - | implemented |
| ACT-034 | `clear-query` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-035 | `clear-screen` | src/bind.c, src/ui.c | - | implemented |
| ACT-036 | `close` | src/bind.c, src/ui.c | - | implemented |
| ACT-037 | `delete-char` | src/bind.c, src/ui.c | tests/pty-diff.sh | implemented but unverified |
| ACT-038 | `deselect` | src/bind.c, src/ui.c | - | implemented |
| ACT-039 | `deselect-all` | src/bind.c, src/ui.c | - | implemented |
| ACT-040 | `disable-raw` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-disable-raw | implemented but unverified |
| ACT-041 | `disable-search` | src/bind.c, src/ui.c | tests/pty-diff.sh:disable-search-action | implemented but unverified |
| ACT-042 | `down` | src/bind.c, src/ui.c | - | implemented |
| ACT-043 | `down-match` | src/bind.c, src/ui.c | - | implemented |
| ACT-044 | `down-selected` | src/bind.c, src/ui.c | - | implemented |
| ACT-045 | `enable-raw` | src/bind.h, src/bind.c, src/ui.c | tests/pty-diff.sh:bind-enable-raw | implemented but unverified |
| ACT-046 | `enable-search` | src/bind.c, src/ui.c | tests/pty-diff.sh:disabled-enable-search-action | implemented but unverified |
| ACT-047 | `end-of-line` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-end-of-line | implemented but unverified |
| ACT-048 | `exclude` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-exclude-current | implemented but unverified |
| ACT-049 | `exclude-multi` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-exclude-multi-selected | implemented but unverified |
| ACT-050 | `execute` | src/bind.c, src/ui.c | - | implemented |
| ACT-051 | `execute-multi` | src/bind.c, src/ui.c | - | implemented |
| ACT-052 | `execute-silent` | src/bind.c, src/ui.c | - | implemented |
| ACT-053 | `first` | src/bind.c, src/ui.c | - | implemented |
| ACT-054 | `forward-char` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-forward-char | implemented but unverified |
| ACT-055 | `forward-subword` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-forward-subword | implemented but unverified |
| ACT-056 | `forward-word` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-forward-word | implemented but unverified |
| ACT-057 | `half-page-down` | src/bind.c, src/ui.c | - | implemented |
| ACT-058 | `half-page-up` | src/bind.c, src/ui.c | - | implemented |
| ACT-059 | `hide-header` | src/bind.c, src/ui.c | - | implemented |
| ACT-061 | `hide-preview` | src/bind.c, src/ui.c | - | implemented |
| ACT-062 | `jump` | - | - | unstarted |
| ACT-063 | `jump-accept` | - | - | unstarted |
| ACT-064 | `kill-line` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-kill-line | implemented but unverified |
| ACT-066 | `kill-word` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-kill-word | implemented but unverified |
| ACT-067 | `last` | src/bind.c, src/ui.c | - | implemented |
| ACT-068 | `next-history` | src/bind.c, src/ui.c | - | implemented |
| ACT-069 | `offset-down` | src/bind.c, src/ui.c | - | implemented |
| ACT-070 | `offset-middle` | src/bind.c, src/ui.c | - | implemented |
| ACT-071 | `offset-up` | src/bind.c, src/ui.c | - | implemented |
| ACT-072 | `page-down` | src/bind.c, src/ui.c | - | implemented |
| ACT-073 | `page-up` | src/bind.c, src/ui.c | - | implemented |
| ACT-074 | `pos` | src/bind.c, src/ui.c | - | implemented |
| ACT-075 | `prev-history` | src/bind.c, src/ui.c | - | implemented |
| ACT-076 | `preview` | src/bind.c, src/ui.c | - | implemented |
| ACT-077 | `preview-bottom` | src/bind.c, src/ui.c | tests/pty-diff.sh:preview-scroll-bottom | implemented but unverified |
| ACT-078 | `preview-down` | src/bind.c, src/ui.c | tests/pty-diff.sh:preview-scroll-down | implemented but unverified |
| ACT-079 | `preview-half-page-down` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-080 | `preview-half-page-up` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-081 | `preview-page-down` | src/bind.c, src/ui.c | tests/pty-diff.sh:preview-scroll-page-down | implemented but unverified |
| ACT-082 | `preview-page-up` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-083 | `preview-top` | src/bind.c, src/ui.c | tests/pty-diff.sh:preview-scroll-top | implemented but unverified |
| ACT-084 | `preview-up` | src/bind.c, src/ui.c | - | implemented but unverified |
| ACT-085 | `print` | src/bind.c, src/ui.c | - | implemented |
| ACT-086 | `print-query` | src/bind.c, src/ui.c | - | implemented |
| ACT-087 | `put` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-put-query | implemented but unverified |
| ACT-088 | `rebind` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-unbind-rebind-key | implemented but unverified |
| ACT-089 | `refresh-preview` | src/bind.c, src/ui.c | - | implemented |
| ACT-090 | `reload` | src/bind.c, src/ui.c | tests/pty-diff.sh:action-reload, tests/pty-diff.sh:action-reload-empty, tests/pty-diff.sh:action-reload-track | implemented but unverified |
| ACT-091 | `reload-sync` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-reload-sync | implemented but unverified |
| ACT-092 | `replace-query` | src/bind.c, src/ui.c | - | implemented |
| ACT-093 | `select` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-select-current | implemented but unverified |
| ACT-094 | `select-all` | src/bind.c, src/ui.c | - | implemented |
| ACT-095 | `show-header` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-hide-show-header | implemented but unverified |
| ACT-096 | `show-input` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-show-input | implemented but unverified |
| ACT-097 | `show-preview` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-hide-show-preview | implemented but unverified |
| ACT-098 | `toggle` | src/bind.c, src/ui.c | - | implemented |
| ACT-099 | `toggle-all` | src/bind.c, src/ui.c | - | implemented |
| ACT-101 | `toggle-down` | src/bind.c, src/ui.c | - | implemented |
| ACT-102 | `toggle-header` | src/bind.c, src/ui.c | - | implemented |
| ACT-103 | `toggle-hscroll` | src/bind.c, src/ui.c | tests/pty-diff.sh:toggle-hscroll-action | implemented but unverified |
| ACT-104 | `toggle-in` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-toggle-in | implemented but unverified |
| ACT-105 | `toggle-input` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-toggle-input | implemented but unverified |
| ACT-106 | `toggle-multi-line` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-toggle-multi-line | implemented but unverified |
| ACT-107 | `toggle-out` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-toggle-out | implemented but unverified |
| ACT-108 | `toggle-preview` | src/bind.c, src/ui.c | - | implemented |
| ACT-109 | `toggle-preview-wrap` | src/bind.c, src/ui.c | - | implemented |
| ACT-110 | `toggle-preview-wrap-word` | src/bind.c, src/ui.c | - | implemented |
| ACT-111 | `toggle-raw` | src/bind.c, src/ui.c | - | implemented |
| ACT-112 | `toggle-search` | src/bind.c, src/ui.c | tests/pty-diff.sh:disabled-toggle-search | implemented but unverified |
| ACT-113 | `toggle-sort` | src/bind.c, src/ui.c | - | implemented |
| ACT-114 | `toggle-track` | src/bind.c, src/ui.c | tests/pty-diff.sh:track-toggle-action, tests/pty-diff.sh:track-toggle-disable-action | implemented but unverified |
| ACT-115 | `toggle-track-current` | src/bind.c, src/ui.c | tests/pty-diff.sh:toggle-track-current-action | implemented but unverified |
| ACT-116 | `toggle-up` | src/bind.c, src/ui.c | - | implemented |
| ACT-117 | `toggle-wrap` | src/bind.c, src/ui.c | - | implemented |
| ACT-118 | `toggle-wrap-word` | src/bind.c, src/ui.c | - | implemented |
| ACT-119 | `track` | src/bind.c, src/ui.c | tests/pty-diff.sh:track-current-action | implemented but unverified |
| ACT-120 | `transform` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-transform-change-query | implemented but unverified |
| ACT-121 | `bg-transform` | src/bind.c, src/ui.c, src/ui.h | tests/pty-diff.sh:bind-bg-transform-change-query | implemented but unverified |
| ACT-124 | `unbind` | src/bind.c, src/ui.c | - | implemented |
| ACT-125 | `unix-line-discard` | src/bind.c, src/ui.c | - | implemented |
| ACT-126 | `unix-word-rubout` | src/bind.c, src/ui.c | tests/pty-diff.sh:bind-unix-word-rubout | implemented but unverified |
| ACT-127 | `untrack-current` | src/bind.c, src/ui.c | tests/pty-diff.sh:untrack-current-action | implemented but unverified |
| ACT-128 | `up` | src/bind.c, src/ui.c | - | implemented |
| ACT-129 | `up-match` | src/bind.c, src/ui.c | - | implemented |
| ACT-130 | `up-selected` | src/bind.c, src/ui.c | - | implemented |
| ACT-131 | `wait` | src/bind.c, src/ui.c | - | implemented |
| ACT-132 | `yank` | src/bind.c, src/ui.c | - | implemented |

## Events

| ID | Event | C location | Verification | Status |
|----|-------|-----------|--------------|--------|
| EVT-001 | `start` | - | - | unstarted |
| EVT-002 | `load` | - | - | unstarted |
| EVT-003 | `change` | - | - | unstarted |
| EVT-004 | `focus` | - | - | unstarted |
| EVT-005 | `result` | - | - | unstarted |
| EVT-006 | `result-final` | - | - | unstarted |
| EVT-007 | `resize` | - | - | unstarted |
| EVT-008 | `one` | - | - | unstarted |
| EVT-009 | `zero` | - | - | unstarted |
| EVT-010 | `jump` | - | - | unstarted |
| EVT-011 | `jump-cancel` | - | - | unstarted |
| EVT-012 | `click-header` | - | - | unstarted |
| EVT-013 | `click-footer` | - | - | unstarted |
