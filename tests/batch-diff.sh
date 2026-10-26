#!/usr/bin/env bash
# batch-diff.sh - differential test of the batch (--filter) mode against the
# reference fzf. Compares stdout, stderr and exit status for a matrix of
# options over a generated corpus.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REF=${REF:-$("$HERE/ref.sh")}
IMPL=${IMPL:-$HERE/../cfzf}
WORK=${WORK:-/tmp/opencode/batch-diff}
mkdir -p "$WORK"

if [ ! -x "$IMPL" ]; then
    printf 'implementation binary not found: %s\n' "$IMPL" >&2
    exit 2
fi

CORPUS="$WORK/corpus.txt"
# Always regenerate the fixture: stale files in WORK must never change a test run.
{
        printf 'apple\nbanana\nCherry\ncherry\ndate\nelderberry\nfig\ngrape\n'
        printf 'src/main.c\nsrc/util.c\nlib/foo.c\nREADME.md\ndocs/PLAN.md\n'
        printf 'a:b:c\nx:y:z\nfoo:bar:baz\n'
        printf 'hello world\nhello there\nworld hello\n'
        printf 'naïve\nrésumé\nüber\n日本語テキスト\n'
        printf 'a b  c\n  leading\ntrailing  \ntab\there\n'
        printf 'ABC\nabc\nAbC\naBc\n'
        printf 'foo bar baz\nfoo_bar_baz\nfoo-bar-baz\n'
        printf '123\n456\n789\n'
        printf 'x\ny\nz\n'
} > "$CORPUS"

pass=0
fail=0

check() {
    desc=$1; shift
    "$REF" "$@" < "$CORPUS" > "$WORK/ref.out" 2> "$WORK/ref.err"
    rrc=$?
    "$IMPL" "$@" < "$CORPUS" > "$WORK/impl.out" 2> "$WORK/impl.err"
    irc=$?
    if [ "$rrc" = "$irc" ] && cmp -s "$WORK/ref.out" "$WORK/impl.out" && cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
        pass=$((pass + 1))
        [ -n "${VERBOSE:-}" ] && printf 'PASS %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf 'FAIL %s (exit ref=%s impl=%s)\n' "$desc" "$rrc" "$irc"
        diff -u "$WORK/ref.out" "$WORK/impl.out" | head -12 || true
        if ! cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
            printf 'stderr differs:\n'
            diff -u "$WORK/ref.err" "$WORK/impl.err" | head -12 || true
        fi
    fi
}

check_file() {
    desc=$1; input=$2; shift 2
    "$REF" "$@" < "$input" > "$WORK/ref.out" 2> "$WORK/ref.err"
    rrc=$?
    "$IMPL" "$@" < "$input" > "$WORK/impl.out" 2> "$WORK/impl.err"
    irc=$?
    if [ "$rrc" = "$irc" ] && cmp -s "$WORK/ref.out" "$WORK/impl.out" && cmp -s "$WORK/ref.err" "$WORK/impl.err"; then
        pass=$((pass + 1))
        [ -n "${VERBOSE:-}" ] && printf 'PASS %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf 'FAIL %s (exit ref=%s impl=%s)\n' "$desc" "$rrc" "$irc"
        cmp -s "$WORK/ref.out" "$WORK/impl.out" || printf 'stdout sizes: ref=%s impl=%s\n' "$(wc -c < "$WORK/ref.out")" "$(wc -c < "$WORK/impl.out")"
        cmp -s "$WORK/ref.err" "$WORK/impl.err" || diff -u "$WORK/ref.err" "$WORK/impl.err" | head -12 || true
    fi
}

# Basic filtering.
for q in apple a e '' AN 'ch' 'a b' 'z' '1' '2'; do
    check "filter=$q" --filter="$q"
done

# Match modes.
check "exact"          --filter=ch --exact
check "extended-off"   --filter=ch --no-extended
check "extended-on"    --filter=ch --extended
check "smart-case"     --filter=CH
check "ignore-case"    --filter=CH --ignore-case
check "case-sensitive" --filter=CH --case-sensitive

# Ordering.
check "tac"            --filter=a --tac
check "no-sort"        --filter=a +s
check "sort"           --filter=a --sort
check "tiebreak=length"  --filter=a --tiebreak=length
check "tiebreak=begin"   --filter=a --tiebreak=begin
check "tiebreak=end"     --filter=a --tiebreak=end
check "tiebreak=pathname" --filter=a --tiebreak=pathname
check "tiebreak=index"   --filter=a --tiebreak=index
check "tiebreak=length,begin" --filter=a --tiebreak=length,begin

# Schemes.
check "scheme=default" --filter=a --scheme=default
check "scheme=path"    --filter=c --scheme=path
check "scheme=history" --filter=a --scheme=history

# Field selection.
check "nth=1"          --filter=a --nth=1
check "nth=2"          --filter=b --nth=2
check "nth=1..2"       --filter=a --nth=1..2
check "nth=..2"        --filter=a --nth=..2
check "nth=2.."        --filter=b --nth=2..
check "with-nth=2"     --filter=b --with-nth=2
check "with-nth=2.."   --filter=b --with-nth=2..
check "delim-colon"    --filter=b --delimiter=: --nth=2
check "delim-attach"   --filter=b -d: --nth=2
check "delim-regex"    --filter=b --delimiter='[:_]' --nth=2

# Output control.
check "print-query"    --filter=a --print-query
check "tail=3"         --filter=a --tail=3
check "multi"          --filter=a --multi
check "select-1"       --filter=a --select-1
check "exit-0"         --filter=a --exit-0

# Extended query syntax.
check "ext-prefix"     --filter='^a'
check "ext-suffix"     --filter='a$'
check "ext-exact"      --filter="'a"
check "ext-inverse"    --filter='!a'
check "ext-or"         --filter='a | b'
check "ext-boundary"   --filter="'a'"

# Invalid options.
check "bad-scheme"     --filter=a --scheme=zzz
check "bad-tiebreak"   --filter=a --tiebreak=zzz
check "bad-style"      --filter=a --style=unsupported
check "unsupported-line-number" --filter=a --line-number
check "unsupported-no-line-number" --filter=a --no-line-number
check "unsupported-graph" --filter=a --graph
check "unsupported-no" --filter=a --no
check "unsupported-oneline" --filter=a --oneline
check "unsupported-no-oneline" --filter=a --no-oneline
check "style-full"     --filter=a --style=full
check "header-lines-one" --filter=a --header-lines=1
check "header-lines-two" --filter=a --header-lines=2
check "header-lines-zero" --filter=a --header-lines=0
check "threads-zero-default" --filter=alpha --threads=0
check "gap-default" --gap --filter=a
check "gap-explicit-value" --gap 2 --filter=a
check "no-gap" --no-gap --filter=a
check "no-gap-line" --no-gap-line --filter=a
check "gap-line-optional" --gap-line --filter=a
check "gap-line-value" --gap-line '|' --filter=a

check_invalid_option() {
    desc=$1
    expected=$2
    shift 2
    "$IMPL" "$@" < "$CORPUS" > "$WORK/impl.out" 2> "$WORK/impl.err"
    rc=$?
    if [ "$rc" -eq 2 ] && grep -q "$expected" "$WORK/impl.err"; then
        pass=$((pass + 1))
        [ -n "${VERBOSE:-}" ] && printf 'PASS %s\n' "$desc"
    else
        fail=$((fail + 1))
        printf 'FAIL %s (exit=%s)\n' "$desc" "$rc"
        cat "$WORK/impl.err" >&2
    fi
}
check_invalid_option "threads-invalid" 'invalid --threads value' --filter=a --threads=bad
check_invalid_option "threads-negative" 'invalid --threads value' --filter=a --threads=-1
check_invalid_option "threads-missing-value" 'invalid --threads value' --filter=a --threads
check_invalid_option "header-lines-invalid" 'invalid --header-lines value' --filter=a --header-lines=bad
check_invalid_option "header-lines-negative" 'invalid --header-lines value' --filter=a --header-lines=-1
check_invalid_option "header-lines-missing-value" 'invalid --header-lines value' --filter=a --header-lines
check_invalid_option "multi-max-invalid" 'invalid --multi-max value' --filter=a --multi-max=bad
check_invalid_option "multi-max-negative" 'invalid --multi-max value' --filter=a --multi-max=-1
check_invalid_option "gap-invalid" 'invalid --gap value' --filter=a --gap=bad
check_invalid_option "gap-negative" 'invalid --gap value' --filter=a --gap=-1
check_invalid_option "scroll-off-invalid" 'invalid --scroll-off value' --filter=a --scroll-off=bad
check_invalid_option "hscroll-off-negative" 'invalid --hscroll-off value' --filter=a --hscroll-off=-1
check_invalid_option "tabstop-zero" 'invalid --tabstop value' --filter=a --tabstop=0
check_invalid_option "history-size-invalid" 'invalid --history-size value' --filter=a --history-size=bad
check_invalid_option "history-size-zero" 'invalid --history-size value' --filter=a --history-size=0
check_invalid_option "multi-max-missing" 'invalid --multi-max value' --filter=a --multi-max
check_invalid_option "scroll-off-missing" 'invalid --scroll-off value' --filter=a --scroll-off
check_invalid_option "hscroll-off-missing" 'invalid --hscroll-off value' --filter=a --hscroll-off
check_invalid_option "tabstop-missing" 'invalid --tabstop value' --filter=a --tabstop
check_invalid_option "history-size-missing" 'invalid --history-size value' --filter=a --history-size

# Exercise stdin reading and output at an exact 4 MiB boundary.
BOUNDARY="$WORK/exact-4mib.txt"
head -c 4194303 /dev/zero | tr '\\000' 'x' > "$BOUNDARY"
printf '\n' >> "$BOUNDARY"
[ "$(wc -c < "$BOUNDARY")" -eq 4194304 ]
check_file "stdin-exact-4mib-boundary" "$BOUNDARY" --filter=

check "phony-filter"    --phony --filter=an
check "phony-nomatch"   --phony --filter=zzz
check "disabled-filter" --disabled --filter=an
check "disabled-nomatch" --disabled --filter=zzz
printf '\n%d/%d batch cases matched\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
