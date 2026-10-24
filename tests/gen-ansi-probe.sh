#!/usr/bin/env bash
# gen-ansi-probe.sh - regenerate ANSI differential test fixtures from the fzf
# reference source. Requires a Go toolchain and the fzf source checked out at
# the reference commit. Writes TSV fixtures consumed by tests/ansiprobe.c and
# tests/ansicolor.c.
set -euo pipefail
FZF_SRC="${FZF_SRC:-/tmp/opencode/fzf-src}"
REF_COMMIT="${REF_COMMIT:-15f64c492a08f0840b81540c7d1de35737448086}"
OUT="${OUT:-/tmp/opencode}"
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ ! -e "$FZF_SRC/.git" ]]; then
    printf 'reference source repository not found: %s\n' "$FZF_SRC" >&2
    exit 1
fi
actual_commit="$(git -C "$FZF_SRC" rev-parse HEAD)"
if [[ "$actual_commit" != "$REF_COMMIT" ]]; then
    printf 'reference commit mismatch: expected %s, got %s\n' "$REF_COMMIT" "$actual_commit" >&2
    exit 1
fi

cleanup() {
    rm -f "$FZF_SRC/src/zzprobe_test.go" "$FZF_SRC/src/zzcolor_test.go"
}
trap cleanup EXIT HUP INT TERM
mkdir -p "$OUT"
rm -f "$OUT/ansi-probe.json" "$OUT/ansi-fuzz.json" "$OUT/ansi-color.json"
cp "$HERE/ansi-probe.go" "$FZF_SRC/src/zzprobe_test.go"
cp "$HERE/ansi-color.go" "$FZF_SRC/src/zzcolor_test.go"
( cd "$FZF_SRC" && go test -run 'TestZZ(Probe|Fuzz|Color)' -count=1 ./src/ )

python3 - "$OUT" <<'PY'
import json, sys
out = sys.argv[1]
for name in ['ansi-probe', 'ansi-fuzz']:
    d = json.load(open(f'{out}/{name}.json'))
    with open(f'{out}/{name}.tsv', 'w') as f:
        for e in d:
            s = e['s'].encode('utf-8', 'surrogateescape')
            seqs = ' '.join(f"{a},{b}" for a, b in e['seqs'])
            f.write(s.hex() + "\t" + seqs + "\n")
d = json.load(open(f'{out}/ansi-color.json'))
with open(f'{out}/ansi-color.tsv', 'w') as f:
    for e in d:
        i = e['in'].encode('utf-8', 'surrogateescape').hex()
        o = e['out'].encode('utf-8', 'surrogateescape').hex()
        offs = ';'.join(','.join(str(x) for x in (x['B'], x['E'], x['Fg'], x['Bg'],
                          x['Ul'], x['Attr'], x['Lbg'])) for x in e['offsets'])
        st = ','.join(str(x) for x in e['st'])
        f.write(f"{i}\t{o}\t{offs}\t{st}\n")
PY

cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O1 -I"$HERE/../src" \
   -o "$OUT/ansiprobe" "$HERE/ansiprobe.c" "$HERE/../src/ansi.c" "$HERE/../src/util.c"
cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O1 -I"$HERE/../src" \
   -o "$OUT/ansicolor" "$HERE/ansicolor.c" "$HERE/../src/ansi.c" "$HERE/../src/util.c"

"$OUT/ansiprobe" < "$OUT/ansi-probe.tsv"
"$OUT/ansiprobe" < "$OUT/ansi-fuzz.tsv"
"$OUT/ansicolor" < "$OUT/ansi-color.tsv"
