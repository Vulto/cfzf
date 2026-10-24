#!/bin/bash
# differential sweep: nth / with-nth / delimiter vs fzf --filter
set -u
MM=/tmp/opencode/fzf
CORP=/tmp/opencode/bigcorpus.txt
pass=0; fail=0
run() {
  local q="$1"; shift
  local a b
  a=$(fzf --filter="$q" "$@" < "$CORP" 2>/dev/null)
  b=$("$MM" --filter="$q" "$@" < "$CORP" 2>/dev/null)
  if [ "$a" = "$b" ]; then pass=$((pass+1)); else fail=$((fail+1)); echo "FAIL q=$q $*"; diff <(printf '%s' "$a") <(printf '%s' "$b") | head -6; fi
}
for q in a oo foo the e; do
  for nth in 1 2 3 1..2 2..3 -1 -2 1,2 1,3 ..2 2.. ; do
    run "$q" --nth="$nth"
  done
done
echo "pass=$pass fail=$fail"
