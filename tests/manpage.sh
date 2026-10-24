#!/usr/bin/env bash
# manpage.sh - verify the man page:
#   * it is valid roff and renders without warnings
#   * every option it documents is accepted by the binary
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
MAN=${MAN:-$ROOT/cfzf.1}

pass=0; fail=0
ok()  { pass=$((pass + 1)); printf 'PASS %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; }

if [ ! -f "$MAN" ]; then
    printf 'man page not found: %s\n' "$MAN" >&2
    exit 2
fi

# 1. It must render without groff warnings.
if command -v groff >/dev/null 2>&1; then
    err=$(groff -man -Tascii -ww "$MAN" >/dev/null 2>&1; groff -man -Tascii -ww "$MAN" 2>&1 >/dev/null)
    if [ -z "$err" ]; then ok "man page renders without warnings"
    else bad "man page renders without warnings"; printf '%s\n' "$err" | head -10; fi
elif command -v nroff >/dev/null 2>&1; then
    if nroff -man "$MAN" >/dev/null 2>&1; then ok "man page renders"
    else bad "man page renders"; fi
else
    printf 'SKIP no groff/nroff available\n'
fi

# 2. Every documented option must be accepted by the binary.
if [ -x "$IMPL" ]; then
    # Collect the option names documented in the ".B \-\-option" lines.
    # The roff escapes ("\-") are turned back into plain hyphens first.
    opts=$(grep -E '^\.B ' "$MAN" | sed 's/\\-/-/g' \
           | grep -oE -- '--[a-z0-9][a-z0-9-]*' | sort -u)
    bad_opts=""
    for o in $opts; do
        case "$o" in
            -) continue ;;
        esac
        out=$("$IMPL" "$o" --filter=zzz </dev/null 2>&1 >/dev/null)
        case "$out" in
            *"unknown option"*)
                # Options that take a value are only rejected in their
                # valueless form; try the "=value" form as well.
                out=$("$IMPL" "$o=v1" --filter=zzz </dev/null 2>&1 >/dev/null)
                case "$out" in
                    *"unknown option"*) bad_opts="$bad_opts $o" ;;
                esac
                ;;
        esac
    done
    if [ -z "$bad_opts" ]; then
        ok "all documented options are accepted"
    else
        bad "all documented options are accepted"
        printf '      rejected:%s\n' "$bad_opts"
    fi
else
    printf 'SKIP binary not found: %s\n' "$IMPL"
fi

printf '\n%d/%d man page checks passed\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
