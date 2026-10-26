#!/usr/bin/env bash
# Verify threaded ranked searches preserve the pinned fzf output exactly.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REF=${REF:-$("$ROOT/tests/ref.sh")}
IMPL=${IMPL:-$ROOT/cfzf}
WORK=${WORK:-/tmp/cfzf-thread-diff}
mkdir -p "$WORK"
CORPUS="$WORK/corpus.txt"

# Deterministic corpus large enough to enter the parallel path.
awk 'BEGIN {
    for (i = 0; i < 12000; i++) {
        printf "%s/%s-%05d-%s.txt\n",
            (i % 7 == 0 ? "src" : (i % 3 == 0 ? "lib" : "test")),
            (i % 5 == 0 ? "alpha" : (i % 2 == 0 ? "alphabet" : "beta")),
            i, (i % 11 == 0 ? "alpha" : "omega")
    }
}' > "$CORPUS"

cases=(
    "--filter=alpha"
    "--filter=alpha --tiebreak=length"
    "--filter=src --scheme=path"
    "--filter=alpha --tac"
    "--filter=alpha --threads=2"
    "--filter=alpha --threads=4"
)
for args in "${cases[@]}"; do
    # Intentional word splitting: each case is a shell-like option list.
    read -r -a argv <<< "$args"
    "$REF" "${argv[@]}" < "$CORPUS" > "$WORK/ref.out" 2> "$WORK/ref.err" || rrc=$?
    rrc=${rrc:-0}
    FZF_NUM_THREADS=1 "$IMPL" "${argv[@]}" < "$CORPUS" > "$WORK/one.out" 2> "$WORK/one.err" || one_rc=$?
    one_rc=${one_rc:-0}
    FZF_NUM_THREADS=4 "$IMPL" "${argv[@]}" < "$CORPUS" > "$WORK/four.out" 2> "$WORK/four.err" || four_rc=$?
    four_rc=${four_rc:-0}
    if [[ $rrc -ne $one_rc || $rrc -ne $four_rc ]] ||
       ! cmp -s "$WORK/ref.out" "$WORK/one.out" ||
       ! cmp -s "$WORK/ref.out" "$WORK/four.out" ||
       ! cmp -s "$WORK/ref.err" "$WORK/one.err" ||
       ! cmp -s "$WORK/ref.err" "$WORK/four.err"; then
        printf 'FAIL threaded differential: %s\n' "$args" >&2
        diff -u "$WORK/ref.out" "$WORK/four.out" | head -20 || true
        exit 1
    fi
    unset rrc one_rc four_rc
    printf 'PASS threaded differential: %s\n' "$args"
done
