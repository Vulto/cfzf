#!/usr/bin/env bash
# ref.sh - resolve the reference fzf binary used by the differential tests.
#
# Resolution order:
#   1. $FZF_REF, if set
#   2. /usr/bin/fzf, if present
#   3. $FZF_SRC (default /tmp/opencode/fzf-src) built from the pinned commit
#   4. $WORK/fzf-ref built from the pinned commit
#
# The reference is junegunn/fzf v0.74.3 (15f64c492a08f0840b81540c7d1de35737448086).
set -u

FZF_REF=${FZF_REF:-}
FZF_SRC=${FZF_SRC:-/tmp/opencode/fzf-src}
REF_COMMIT=${REF_COMMIT:-15f64c492a08f0840b81540c7d1de35737448086}
REF_BUILD=${REF_BUILD:-/tmp/opencode/fzf-ref}

build_ref() {
    [ -d "$FZF_SRC/.git" ] || return 1
    command -v go >/dev/null 2>&1 || return 1
    ( cd "$FZF_SRC" &&
      git cat-file -e "$REF_COMMIT^{commit}" >/dev/null 2>&1 || exit 1
      git worktree add --detach -f "$REF_BUILD.wt" "$REF_COMMIT" >/dev/null 2>&1 || exit 1
      cd "$REF_BUILD.wt" && go build -o "$REF_BUILD" . ) >/dev/null 2>&1 || return 1
    rm -rf "$FZF_SRC/$REF_BUILD.wt"
    [ -x "$REF_BUILD" ]
}

if [ -n "$FZF_REF" ] && [ -x "$FZF_REF" ]; then
    printf '%s\n' "$FZF_REF"
    exit 0
fi
if [ -x /usr/bin/fzf ]; then
    printf '%s\n' /usr/bin/fzf
    exit 0
fi
if [ -x "$REF_BUILD" ]; then
    printf '%s\n' "$REF_BUILD"
    exit 0
fi
if build_ref; then
    printf '%s\n' "$REF_BUILD"
    exit 0
fi
printf 'no reference fzf binary found (set FZF_REF or provide %s)\n' "$FZF_SRC" >&2
exit 1
