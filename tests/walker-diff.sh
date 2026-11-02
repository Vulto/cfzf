#!/usr/bin/env bash
# walker-diff.sh - differential test of the default file listing (fzf started
# without piped input) against the reference.
#
# The reference walks the tree concurrently, so the order of the entries is
# not deterministic; the test compares the resulting set of paths.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REF=${REF:-$("$HERE/ref.sh")}
IMPL=${IMPL:-$HERE/../cfzf}
WORK=${WORK:-/tmp/opencode/walker-diff}
mkdir -p "$WORK"

TREE="$WORK/tree"
rm -rf "$TREE"
mkdir -p "$TREE/src" "$TREE/lib/sub" "$TREE/.hidden" "$TREE/mylib/sub"
touch "$TREE/b.txt" "$TREE/a.txt" "$TREE/z.txt" "$TREE/.dotfile" \
      "$TREE/src/main.c" "$TREE/src/util.c" "$TREE/lib/x.c" "$TREE/lib/sub/y.c" \
      "$TREE/mylib/sub/keep.c" \
      "$TREE/.hidden/h.txt" "$TREE/README.md" "$TREE/src/lib"
ln -sf a.txt "$TREE/link.txt"
ln -sf src "$TREE/linkdir"

printf 'sleep 300\n' > "$WORK/script.txt"

pass=0; fail=0

list_files() {  # list_files <binary> <args...>
    python3 "$HERE/ptyrun.py" "$1" --script "$WORK/script.txt" --screen \
        --cwd "$TREE" "${@:2}" 2>/dev/null |
        sed -n 's/^▌ //p' | sort
}

check() {
    desc=$1; shift
    list_files "$REF" "$@" > "$WORK/ref.txt"
    list_files "$IMPL" "$@" > "$WORK/impl.txt"
    if diff -q "$WORK/ref.txt" "$WORK/impl.txt" > /dev/null; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$desc"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$desc"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}

check 'default'
check 'walker=dir'          --walker=dir
check 'walker=file'         --walker=file
check 'walker=file,dir'     --walker=file,dir
check 'walker=hidden'       --walker=hidden
check 'walker=all'          --walker=file,dir,follow,hidden
check 'walker-skip'         --walker-skip=lib
check 'walker-skip-path'    --walker-skip=lib/sub
check 'walker-skip-absolute' --walker-root="$TREE" --walker-skip="$TREE/lib"
check 'walker-root'         --walker-root=.
check 'walker-root-multiple' --walker-root="$TREE/src" "$TREE/lib"

printf '\n%d/%d walker cases matched\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
