#!/usr/bin/env bash
# Resolve the exact fzf reference used by differential tests.
#
# Default: build the pinned upstream commit from FZF_SRC. Do not silently use
# a system fzf or a stale REF_BUILD binary: either can produce false results.
#
# FZF_REF is an explicit override for a reference binary the caller has already
# verified to come from REF_COMMIT.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FZF_SRC="${FZF_SRC:-/tmp/opencode/fzf-src}"
REF_COMMIT="${REF_COMMIT:-15f64c492a08f0840b81540c7d1de35737448086}"
REF_BUILD="${REF_BUILD:-/tmp/opencode/fzf-ref}"
REF_WORKTREE="${REF_WORKTREE:-${REF_BUILD}.wt}"

if [[ -n "${FZF_REF:-}" ]]; then
    if [[ ! -x "$FZF_REF" ]]; then
        printf 'FZF_REF is set but is not executable: %s\n' "$FZF_REF" >&2
        exit 1
    fi
    printf '%s\n' "$FZF_REF"
    exit 0
fi

if [[ ! -e "$FZF_SRC/.git" ]]; then
    printf 'pinned fzf source repository not found: %s\n' "$FZF_SRC" >&2
    printf 'Provide that checkout at commit %s, or set FZF_REF to a verified binary.\n' "$REF_COMMIT" >&2
    exit 1
fi

if ! command -v go >/dev/null 2>&1; then
    printf 'Go is required to build the pinned fzf reference.\n' >&2
    exit 1
fi

if ! git -C "$FZF_SRC" cat-file -e "${REF_COMMIT}^{commit}" 2>/dev/null; then
    printf 'pinned commit %s is unavailable in %s\n' "$REF_COMMIT" "$FZF_SRC" >&2
    exit 1
fi

mkdir -p "$(dirname "$REF_BUILD")"

# Never trust an existing binary at REF_BUILD: rebuild it from the pinned
# commit for this invocation.
if [[ -e "$REF_WORKTREE" ]]; then
    git -C "$FZF_SRC" worktree remove --force "$REF_WORKTREE" >/dev/null 2>&1 || {
        printf 'cannot remove stale reference worktree: %s\n' "$REF_WORKTREE" >&2
        exit 1
    }
fi

cleanup() {
    git -C "$FZF_SRC" worktree remove --force "$REF_WORKTREE" >/dev/null 2>&1 || true
}
trap cleanup EXIT HUP INT TERM

git -C "$FZF_SRC" worktree add --detach --force "$REF_WORKTREE" "$REF_COMMIT" >/dev/null

actual_commit="$(git -C "$REF_WORKTREE" rev-parse HEAD)"
if [[ "$actual_commit" != "$REF_COMMIT" ]]; then
    printf 'reference worktree commit mismatch: expected %s, got %s\n' "$REF_COMMIT" "$actual_commit" >&2
    exit 1
fi

if ! (cd "$REF_WORKTREE" && go build -o "$REF_BUILD" .); then
    printf 'failed to build fzf reference at %s\n' "$REF_COMMIT" >&2
    exit 1
fi

cleanup
trap - EXIT HUP INT TERM
if [[ ! -x "$REF_BUILD" ]]; then
    printf 'reference build did not produce an executable: %s\n' "$REF_BUILD" >&2
    exit 1
fi

printf '%s\n' "$REF_BUILD"
