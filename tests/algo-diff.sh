#!/usr/bin/env bash
# Differential ordering test: compare the C23 implementation's --filter output
# (scores, ranking and tie-breaks) against the reference fzf.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
REF=${REF:-$("$ROOT/tests/ref.sh")}
CORPUS="${1:-/tmp/opencode/corpus.txt}"

if [ ! -s "$CORPUS" ]; then
    CORPUS=/tmp/opencode/algo-corpus.txt
    mkdir -p "$(dirname "$CORPUS")"
    {
        printf 'fuzzyfinder\nfuzzy-finder\nfoobar\nfoo bar\nfoobarbaz\n'
        printf 'src/main.c\nsrc/util.c\nlib/foo.c\nREADME.md\n'
        printf 'abc\nabd\nabe\nxyz\n'
        printf 'val\nvalue\nvalues\noval\n'
        printf 'a\nab\nabc\nabcd\nabcde\n'
    } > "$CORPUS"
fi

pass=0; fail=0

run() {
    local desc="$1"; shift
    local refout implout
    refout=$("$REF" "$@" < "$CORPUS" 2>&1); rrc=$?
    implout=$("$IMPL" "$@" < "$CORPUS" 2>&1); irc=$?
    if [ "$rrc" = "$irc" ] && [ "$refout" = "$implout" ]; then
        pass=$((pass + 1)); printf 'PASS  %s\n' "$desc"
    else
        fail=$((fail + 1)); printf 'FAIL  %s (exit ref=%s impl=%s)\n' "$desc" "$rrc" "$irc"
        diff <(printf '%s\n' "$refout") <(printf '%s\n' "$implout") | sed 's/^/      /' | head -10
    fi
}

for q in f ff oob foob a abc to oo val; do
    run "fuzzy q=$q"       --filter="$q"
done
for q in ff oob foob; do
    run "exact q=$q"       --filter="$q" --exact
    run "extended q=$q"    --filter="$q" --extended
    run "no-extended q=$q" --filter="$q" --no-extended
done
run "tiebreak=length"    --filter=a --tiebreak=length
run "tiebreak=begin"     --filter=a --tiebreak=begin
run "tiebreak=end"       --filter=a --tiebreak=end
run "tiebreak=pathname"  --filter=c --tiebreak=pathname
run "tiebreak=index"     --filter=a --tiebreak=index
run "scheme=path"        --filter=c --scheme=path
run "scheme=history"     --filter=a --scheme=history
run "tac"                --filter=a --tac
run "no-sort"            --filter=a +s

echo "----"
echo "pass=$pass fail=$fail"
[ "$fail" -eq 0 ]
