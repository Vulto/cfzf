#!/usr/bin/env bash
# bench.sh - compare the C23 implementation with the reference fzf on
# startup latency, input ingestion and matching throughput.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
REF=${REF:-$("$ROOT/tests/ref.sh")}
WORK=${WORK:-/tmp/opencode/bench}
mkdir -p "$WORK"

CORPUS="$WORK/corpus.txt"
if [ ! -s "$CORPUS" ]; then
    # 200k lines of mixed text.
    python3 - "$CORPUS" <<'PY'
import sys, random
random.seed(7)
words = ["alpha","beta","gamma","delta","epsilon","zeta","eta","theta",
         "path","to","file","name","src","lib","include","test","case"]
with open(sys.argv[1], "w") as f:
    for i in range(200000):
        n = random.randint(2, 6)
        f.write("-".join(random.choice(words) for _ in range(n)) + "\n")
PY
fi

printf 'lines: %s\n' "$(wc -l < "$CORPUS")"

timeit() {  # timeit <label> <cmd...>
    local label=$1; shift
    local start end
    start=$(date +%s%N)
    "$@" > /dev/null 2>&1
    end=$(date +%s%N)
    printf '%-28s %8.1f ms\n' "$label" "$(( (end - start) / 1000000 ))"
}

run_both() {
    local label=$1; shift
    local s e
    s=$(date +%s%N); "$REF" "$@" < "$CORPUS" > /dev/null 2>&1; e=$(date +%s%N)
    printf 'ref  %-24s %8.1f ms\n' "$label" "$(( (e - s) / 1000000 ))"
    s=$(date +%s%N); "$IMPL" "$@" < "$CORPUS" > /dev/null 2>&1; e=$(date +%s%N)
    printf 'c23  %-24s %8.1f ms\n' "$label" "$(( (e - s) / 1000000 ))"
}

printf '\n-- batch matching --\n'
run_both "filter=alpha"  --filter=alpha
run_both "filter=a"      --filter=a
run_both "filter=src"    --filter=src

printf '\n-- startup (no input) --\n'
timeit "ref --version"   "$REF" --version
timeit "c23 --version"   "$IMPL" --version
