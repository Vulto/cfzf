#!/usr/bin/env bash
# Differential sweep for --nth, --with-nth and delimiters against pinned fzf.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
REF=${REF:-$("$HERE/ref.sh")}
IMPL=${IMPL:-$ROOT/cfzf}
WORK=${WORK:-/tmp/opencode/nth-diff}
mkdir -p "$WORK"

if [[ ! -x "$IMPL" ]]; then
    printf 'implementation binary not found: %s\n' "$IMPL" >&2
    exit 2
fi

CORP="$WORK/corpus.txt"
# Recreate the corpus every run; never rely on a stale /tmp file.
printf 'alpha:one:first\nbeta:two:second\nfoo:bar:baz\nthe quick brown fox\nhello world\n' > "$CORP"

pass=0
fail=0

run() {
    local desc="$1"; shift
    "$REF" --filter="$QUERY" "$@" < "$CORP" > "$WORK/ref.out" 2> "$WORK/ref.err"
    local rrc=$?
    "$IMPL" --filter="$QUERY" "$@" < "$CORP" > "$WORK/impl.out" 2> "$WORK/impl.err"
    local irc=$?
    if [[ "$rrc" -eq "$irc" ]] &&
       cmp -s "$WORK/ref.out" "$WORK/impl.out" &&
       cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
        pass=$((pass + 1))
    else
        fail=$((fail + 1))
        printf 'FAIL %s (exit ref=%d impl=%d)\n' "$desc" "$rrc" "$irc"
        diff -u "$WORK/ref.out" "$WORK/impl.out" | head -8 || true
        if ! cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
            printf 'stderr differs:\n'
            diff -u "$WORK/ref.err" "$WORK/impl.err" | head -8 || true
        fi
    fi
}

for QUERY in a oo foo the e; do
    for nth in 1 2 3 1..2 2..3 -1 -2 1,2 1,3 ..2 2..; do
        run "nth query=$QUERY nth=$nth" --nth="$nth"
    done
done

printf 'pass=%d fail=%d\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
