#!/usr/bin/env bash
# Differential ordering test against the pinned upstream reference.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
REF=${REF:-$("$HERE/ref.sh")}
WORK=${WORK:-/tmp/opencode/algo-diff}
mkdir -p "$WORK"

if [[ ! -x "$IMPL" ]]; then
    printf 'implementation binary not found: %s\n' "$IMPL" >&2
    exit 2
fi

CORPUS="$WORK/corpus.txt"
# Deterministic fixture: always overwrite any previous run's data.
{
    printf 'fuzzyfinder\nfuzzy-finder\nfoobar\nfoo bar\nfoobarbaz\n'
    printf 'src/main.c\nsrc/util.c\nlib/foo.c\nREADME.md\n'
    printf 'abc\nabd\nabe\nxyz\n'
    printf 'val\nvalue\nvalues\noval\n'
    printf 'ax\nįx\nfoo/ix\nfooįx\nfoo/x\nx\n'
    printf 'a\nab\nabc\nabcd\nabcde\n'
} > "$CORPUS"

pass=0
fail=0

run() {
    local desc="$1"; shift
    "$REF" "$@" < "$CORPUS" > "$WORK/ref.out" 2> "$WORK/ref.err"
    local rrc=$?
    "$IMPL" "$@" < "$CORPUS" > "$WORK/impl.out" 2> "$WORK/impl.err"
    local irc=$?
    if [[ "$rrc" -eq "$irc" ]] &&
       cmp -s "$WORK/ref.out" "$WORK/impl.out" &&
       cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
        pass=$((pass + 1))
        printf 'PASS %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf 'FAIL %s (exit ref=%d impl=%d)\n' "$desc" "$rrc" "$irc"
        diff -u "$WORK/ref.out" "$WORK/impl.out" | head -10 || true
        if ! cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
            printf 'stderr differs:\n'
            diff -u "$WORK/ref.err" "$WORK/impl.err" | head -10 || true
        fi
    fi
}

for q in f ff oob foob a abc to oo val x; do
    run "fuzzy q=$q" --filter="$q"
done
for q in f ff oob foob a abc to oo val x; do
    run "algo=v1 q=$q" --algo=v1 --filter="$q"
    run "algo=v2 q=$q" --algo=v2 --filter="$q"
done
run "algo=v1 separate argument" --algo v1 --filter=foob
run "algo=v1 extended query" --algo=v1 --filter="foo bar" --extended
run "algo=v1 nth token" --algo=v1 --delimiter=/ --nth=2 --filter=main
for q in ff oob foob; do
    run "exact q=$q" --filter="$q" --exact
    run "extended q=$q" --filter="$q" --extended
    run "no-extended q=$q" --filter="$q" --no-extended
done
run "tiebreak=length" --filter=a --tiebreak=length
run "tiebreak=begin" --filter=a --tiebreak=begin
run "tiebreak=end" --filter=a --tiebreak=end
run "tiebreak=pathname" --filter=c --tiebreak=pathname
run "tiebreak=index" --filter=a --tiebreak=index
run "scheme=path" --filter=c --scheme=path
run "scheme=history" --filter=a --scheme=history
run "tac" --filter=a --tac
run "no-sort" --filter=a +s

printf 'pass=%d fail=%d\n' "$pass" "$fail"
[[ "$fail" -eq 0 ]]
