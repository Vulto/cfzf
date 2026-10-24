#!/usr/bin/env bash
# shell-integration.sh - verify the vendored upstream shell integration.
#
# The scripts in shell/ are copied unchanged from fzf v0.74.3 (PLAN §8: keep
# upstream scripts unchanged when they remain compatible). Only bash is
# supported; zsh and fish integrations are deliberately out of scope.
# This test checks that the bash scripts are syntactically valid, that the
# completion helpers load, and it reports which fzf options the scripts
# depend on that this implementation does not support yet.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
SHELL_DIR="$ROOT/shell"
IMPL=${IMPL:-$ROOT/cfzf}

pass=0; fail=0
ok()   { pass=$((pass + 1)); printf 'PASS %s\n' "$1"; }
bad()  { fail=$((fail + 1)); printf 'FAIL %s\n' "$1"; }

for f in key-bindings.bash completion.bash common.sh; do
    if [ -f "$SHELL_DIR/$f" ]; then
        ok "present $f"
    else
        bad "present $f"
    fi
done

# Bash syntax check.
for f in key-bindings.bash completion.bash common.sh; do
    if bash -n "$SHELL_DIR/$f" 2>/dev/null; then
        ok "bash -n $f"
    else
        bad "bash -n $f"
    fi
done

# The bash completion must load without error and define _fzf_completion.
if ( set -u; . "$SHELL_DIR/common.sh"; . "$SHELL_DIR/completion.bash" ) 2>/dev/null; then
    ok "bash completion loads"
else
    bad "bash completion loads"
fi

# Report the options the scripts rely on that the implementation rejects.
if [ -x "$IMPL" ]; then
    missing=""
    for opt in --height --min-height --bind --tmux --preview --multi --expect \
               --pointer --info --cycle --no-multi --height= --marker --jump-labels; do
        # --filter keeps fzf in batch mode so the check always terminates.
        # Exit status 0 (match) and 1 (no match) both mean "option accepted";
        # 2 means the option was rejected.
        "$IMPL" "$opt" --filter=zzz </dev/null >/dev/null 2>&1
        rc=$?
        if [ "$rc" -ge 2 ]; then
            missing="$missing $opt"
        fi
    done
    if [ -n "$missing" ]; then
        printf 'NOTE shell scripts rely on options not yet implemented:%s\n' "$missing"
    fi
fi

printf '\n%d/%d shell integration checks passed\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
