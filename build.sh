#!/bin/sh
set -eu

Root=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
CC=${CC:-cc}
Output=${FZF_BUILD_OUTPUT:-"$Root/cfzf"}

if [ "$#" -gt 0 ]; then
    printf 'Usage: %s\n' "$0" >&2
    exit 2
fi

if ! command -v "$CC" >/dev/null 2>&1; then
    printf 'Compiler not found: %s\n' "$CC" >&2
    exit 127
fi

CFLAGS=${CFLAGS:--O2 -Wall -Wextra -Wpedantic }

# Select the newest supported C23 language mode for this compiler.
ProbeDir=$(mktemp -d)
trap 'rm -rf "$ProbeDir"' EXIT HUP INT TERM
: > "$ProbeDir/empty.c"
CSTD=''
for Flag in -std=c23 -std=c2x; do
    if "$CC" "$Flag" -c "$ProbeDir/empty.c" -o "$ProbeDir/empty.o" >/dev/null 2>&1; then
        CSTD=$Flag
        break
    fi
done
if [ -z "$CSTD" ]; then
    printf 'No supported C23 language mode found for %s.\n' "$CC" >&2
    exit 1
fi

# Portable feature-test macros: _POSIX_C_SOURCE for termios/ioctl, _DEFAULT_SOURCE
# for the rest. We do not use ncurses; the terminal layer talks to termios
# directly.
FEATURES='-D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE'

mkdir -p "$(dirname -- "$Output")"
# shellcheck disable=SC2086 # CFLAGS, CSTD, FEATURES intentionally expand.
"$CC" $CFLAGS $CSTD $FEATURES -I"$Root/src" -o "$Output" "$Root"/src/*.c -lm -pthread

Prefix=${PREFIX:-/usr/local}
# Pick the highest-priority man directory this user can actually write to,
# falling back to the prefix. Never invoke privilege escalation.
Mandir=${MANDIR:-}
if [ -z "$Mandir" ]; then
    for Candidate in $(manpath 2>/dev/null | tr ':' ' '); do
        [ -n "$Candidate" ] || continue
        if mkdir -p "$Candidate/man1" 2>/dev/null && [ -w "$Candidate/man1" ]; then
            Mandir=$Candidate
            break
        fi
    done
fi
[ -n "$Mandir" ] || Mandir="$Prefix/share/man"

mkdir -p "$Prefix/bin" "$Mandir/man1"
install -m 0755 "$Output" "$Prefix/bin/cfzf"
if [ -f "$Root/cfzf.1" ]; then
    install -m 0644 "$Root/cfzf.1" "$Mandir/man1/cfzf.1"
    printf 'Installed %s\n' "$Mandir/man1/cfzf.1"
fi
printf 'Built %s\n' "$Output"
printf 'Installed %s\n' "$Prefix/bin/cfzf"
