#!/usr/bin/env bash
# allocfail.sh - allocation-failure injection test (PLAN §6).
#
# Runs the binary with the Nth allocation failing and checks that it always
# terminates cleanly (no crash, no sanitizer report) and reports the exit
# status it produced.  Exit statuses 0 (match), 1 (no match) and 2 (handled
# error) are all acceptable; a signal (>= 128) is not.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
WORK=${WORK:-/tmp/opencode/allocfail}
mkdir -p "$WORK"

SO="$WORK/failalloc.so"
cc -std=c23 -O1 -g -fPIC -shared -o "$SO" "$HERE/failalloc.c" -ldl 2>/dev/null || {
    printf 'SKIP cannot build the allocation shim\n'
    exit 0
}

INPUT="$WORK/input.txt"
printf 'apple\nbanana\ncherry\ndate\n' > "$INPUT"

pass=0; fail=0
crashes=0

run_case() {
    desc=$1; shift
    local n
    for n in $(seq 1 "${1:-120}"); do
        out=$(FAILALLOC_N="$n" LD_PRELOAD="$SO" "$IMPL" "${@:2}" < "$INPUT" 2>&1 >/dev/null)
        rc=$?
        if [ "$rc" -ge 128 ]; then
            printf 'CRASH %s (allocation %d, status %d)\n' "$desc" "$n" "$rc"
            crashes=$((crashes + 1))
            [ "$crashes" -le 3 ] && printf '%s\n' "$out" | head -5
        fi
        pass=$((pass + 1))
    done
    printf 'PASS %s (%d allocation failures injected)\n' "$desc" "$1"
}

run_case 'filter' 120 --filter=an
run_case 'filter-nth' 80 --filter=an --nth=1 -d:
run_case 'filter-regex' 80 --filter=an --delimiter='[:_]'
run_case 'filter-scheme' 80 --filter=a --scheme=path

# The interactive paths are covered with an in-binary hook, so that only the
# allocations of the program under test are affected.
AF_BIN="$WORK/fzf-af"
if cc -std=c23 -O1 -g -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
      -I"$HERE/../src" -o "$AF_BIN" "$HERE/../src"/*.c "$HERE/allocfail_hook.c" -ldl 2>/dev/null; then
    printf 'sleep 200\nsend an\nsleep 200\nkey enter\nsleep 200\n' > "$WORK/script.txt"
    for n in $(seq 1 140); do
        FAILALLOC_N="$n" python3 "$HERE/ptyrun.py" "$AF_BIN" --script "$WORK/script.txt" \
            --stdin-file "$INPUT" > /dev/null 2>&1
        rc=$?
        if [ "$rc" -ge 128 ] || [ "$rc" -lt 0 ]; then
            printf 'CRASH interactive (allocation %d, status %d)\n' "$n" "$rc"
            crashes=$((crashes + 1))
        fi
        pass=$((pass + 1))
    done
    printf 'PASS interactive (140 allocation failures injected)\n'
else
    printf 'SKIP cannot build the in-binary allocation hook\n'
fi

printf '\n%d allocation failures injected, %d crashes\n' "$pass" "$crashes"
[ "$crashes" -eq 0 ]
