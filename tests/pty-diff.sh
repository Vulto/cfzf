#!/usr/bin/env bash
# pty-diff.sh - interactive differential test: run the reference fzf and the
# C23 implementation under a pseudo-terminal with the same keystrokes and
# compare the resulting logical screen state.
set -uo pipefail

HERE="$(cd "$(dirname "$0")" && pwd)"
REF=${REF:-$("$HERE/ref.sh")}
IMPL=${IMPL:-$HERE/../cfzf}
WORK=${WORK:-/tmp/opencode/pty-diff}
mkdir -p "$WORK"

if [ ! -x "$IMPL" ]; then
    printf 'implementation binary not found: %s\n' "$IMPL" >&2
    exit 2
fi

printf 'apple\nbanana\ncherry\ndate\nelderberry\n' > "$WORK/items.txt"
printf '10 alpha\n20 beta\n30 gamma\n' > "$WORK/id-items.txt"
printf 'src/main.c\nsrc/util.c\nlib/foo.c\nREADME.md\n' > "$WORK/paths.txt"
# A longer list, so that the list has to scroll.
i=0; : > "$WORK/many.txt"
while [ $i -lt 20 ]; do printf 'item%02d\n' "$i" >> "$WORK/many.txt"; i=$((i+1)); done
# Long items for the list wrapping.
printf 'x%.0s' $(seq 1 80) > "$WORK/long.txt"
printf '\nshort\n' >> "$WORK/long.txt"
printf 'xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxneedle\n' > "$WORK/scroll.txt"
printf '日本語テキスト\n日本語\n' > "$WORK/unicode.txt"

pass=0
fail=0

# Comparing the captured screens can fail when the capture happens in the
# middle of a redraw or when the machine is busy.  Such a failure is
# retried before it is reported.
compare() {
    ref=$1; impl=$2; name=$3
    for attempt in 1 2 3; do
        if diff -q "$ref" "$impl" > /dev/null; then
            return 0
        fi
        if [ "$attempt" -lt 3 ]; then
            sleep 0.3
        fi
    done
    # Keep the failing artifacts for inspection.
    cp "$ref" "$ref.failed" 2>/dev/null
    cp "$impl" "$impl.failed" 2>/dev/null
    return 1
}

run_case() {
    name=$1; script=$2; items=$3; shift 3
    printf '%s\n' "$script" > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$items" --screen --stdout -- "$@" > "$WORK/ref.txt" 2> "$WORK/ref.err"
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$items" --screen --stdout -- "$@" > "$WORK/impl.txt" 2> "$WORK/impl.err"
    if compare "$WORK/ref.txt" "$WORK/impl.txt" "$name"; then
        pass=$((pass + 1))
        printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1))
        printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -30
    fi
    # Compare the foreground colour of every cell as well, so that a
    # difference in the highlighted line or in the attributes is caught.
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$items" --screen --color -- "$@" > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$items" --screen --color -- "$@" > "$WORK/impl.c" 2>/dev/null
    if compare "$WORK/ref.c" "$WORK/impl.c" "$name"; then
        pass=$((pass + 1))
        printf 'PASS %s (color)\n' "$name"
    else
        fail=$((fail + 1))
        printf 'FAIL %s (color)\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -30
    fi
}

run_case 'initial'      'sleep 400'            "$WORK/items.txt"
run_case 'select-1-single-initial-match' 'sleep 400' "$WORK/items.txt" --select-1 --query=apple
run_case 'exit-0-initial-no-match' 'sleep 400' "$WORK/items.txt" --exit-0 --query=missing
run_case 'accept-nth-interactive-template' 'sleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --accept-nth='{n}: {1}'
run_case 'hscroll-match-visible' 'sleep 250\nsend needle\nsleep 250' "$WORK/scroll.txt"
run_case 'no-hscroll-match-visible' 'sleep 250\nsend needle\nsleep 250' "$WORK/scroll.txt" --no-hscroll
run_case 'toggle-hscroll-action' 'sleep 250\nsend needle\nsleep 200\nkey ctrl-h\nsleep 250' "$WORK/scroll.txt" --bind=ctrl-h:toggle-hscroll
run_case 'track-current-item' 'sleep 250\nsend date\nsleep 200\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --track
run_case 'track-toggle-action' 'sleep 250\nsend date\nsleep 200\nkey ctrl-t\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --bind=ctrl-t:toggle-track
run_case 'track-toggle-disable-action' 'sleep 250\nsend date\nsleep 200\nkey ctrl-t\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --track --bind=ctrl-t:toggle-track
run_case 'track-current-action' 'sleep 250\nsend date\nsleep 200\nkey ctrl-t\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --bind=ctrl-t:track-current
run_case 'toggle-track-current-action' 'sleep 250\nsend date\nsleep 200\nkey ctrl-t\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --bind=ctrl-t:toggle-track-current
run_case 'untrack-current-action' 'sleep 250\nsend date\nsleep 200\nkey ctrl-t\nkey ctrl-x\nkey ctrl-u\nsend a\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --bind=ctrl-t:track-current --bind=ctrl-x:untrack-current
run_case 'positional-query' 'sleep 350' "$WORK/items.txt" an
run_case 'positional-query-after-options' 'sleep 350' "$WORK/items.txt" -- an
run_case 'positional-query-leading-dash' 'sleep 350' "$WORK/items.txt" -- -an
run_case 'query'        'sleep 350
send an
sleep 300'                                        "$WORK/items.txt"
run_case 'query-enter'  'sleep 350
send cherry
sleep 200
key enter
sleep 200'                                        "$WORK/items.txt"
run_case 'navigate'     'sleep 350
key down
sleep 150
key down
sleep 150
key up
sleep 200'                                        "$WORK/items.txt"
run_case 'backspace'    'sleep 350
send banana
sleep 200
key bspace
sleep 200'                                        "$WORK/items.txt"
run_case 'query-cursor-insert' 'sleep 250
send banana
sleep 100
key home
key right
send X
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'query-cursor-unicode' 'sleep 250
send 日本語
sleep 100
key home
key right
send X
sleep 200
key enter
sleep 200' "$WORK/unicode.txt"
run_case 'query-cursor-delete' 'sleep 250
send banana
sleep 100
key home
key right
key delete
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'query-cursor-backspace' 'sleep 250
send banana
sleep 100
key home
key right
key bspace
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'query-word-cursor' 'sleep 250
send apple banana cherry
sleep 100
key alt-left
send X
sleep 150
key alt-right
send Y
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'query-kill-before-cursor' 'sleep 250
send apple banana
sleep 100
key home
key alt-right
key ctrl-u
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'query-kill-after-cursor' 'sleep 250
send apple banana
sleep 100
key home
key alt-right
key ctrl-k
sleep 200
key enter
sleep 200' "$WORK/items.txt"
run_case 'bind-backward-char' 'sleep 250
send banana
sleep 100
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:backward-char
run_case 'bind-forward-char' 'sleep 250
send banana
sleep 100
key home
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:forward-char
run_case 'bind-beginning-of-line' 'sleep 250
send banana
sleep 100
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:beginning-of-line
run_case 'bind-end-of-line' 'sleep 250
send banana
sleep 100
key home
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:end-of-line
run_case 'bind-backward-word' 'sleep 250
send apple banana
sleep 100
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:backward-word
run_case 'bind-forward-word' 'sleep 250
send apple banana
sleep 100
key home
key ctrl-x
send X
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:forward-word
run_case 'bind-kill-line' 'sleep 250
send apple banana
sleep 100
key home
key alt-right
key ctrl-x
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:kill-line
run_case 'bind-kill-word' 'sleep 250
send apple banana
sleep 100
key home
key alt-right
key ctrl-x
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:kill-word
run_case 'bind-backward-kill-word' 'sleep 250
send apple banana
sleep 100
key ctrl-x
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:backward-kill-word
run_case 'bind-unix-word-rubout' 'sleep 250
send apple banana
sleep 100
key ctrl-x
sleep 150
key enter
sleep 200' "$WORK/items.txt" --bind=ctrl-x:unix-word-rubout
run_case 'no-match'     'sleep 350
send zzzz
sleep 250'                                        "$WORK/items.txt"
run_case 'paths'        'sleep 350
send c
sleep 250'                                        "$WORK/paths.txt"

run_case 'reverse'      'sleep 300'            "$WORK/items.txt" --reverse
run_case 'gap-default-layout' 'sleep 300' "$WORK/items.txt" --gap
run_case 'gap-disabled-layout' 'sleep 300' "$WORK/items.txt" --no-gap
run_case 'gap-line-layout' 'sleep 300' "$WORK/items.txt" --gap=1 --gap-line='-'
run_case 'gap-line-disabled-layout' 'sleep 300' "$WORK/items.txt" --gap=1 --no-gap-line
run_case 'style-default' 'sleep 300'           "$WORK/items.txt" --style=default
run_case 'style-default-border' 'sleep 300'    "$WORK/items.txt" --style=default --border=rounded
run_case 'list-border-default' 'sleep 300'   "$WORK/items.txt" --list-border
run_case 'list-border-sharp' 'sleep 300'     "$WORK/items.txt" --list-border=sharp
run_case 'list-border-reverse' 'sleep 300'   "$WORK/items.txt" --reverse --list-border=sharp
run_case 'list-label' 'sleep 300' "$WORK/items.txt" --list-border=sharp --list-label='RESULTS'
run_case 'list-label-pos' 'sleep 300' "$WORK/items.txt" --list-border=sharp --list-label='RESULTS' --list-label-pos=4
run_case 'list-label-center' 'sleep 300' "$WORK/items.txt" --list-border=sharp --list-label='RESULTS' --list-label-pos=center
run_case 'list-label-bottom' 'sleep 300' "$WORK/items.txt" --list-border=sharp --list-label='RESULTS' --list-label-pos=4:bottom
run_case 'list-label-reverse' 'sleep 300' "$WORK/items.txt" --reverse --list-border=sharp --list-label='RESULTS'
run_case 'header-border-default' 'sleep 300'  "$WORK/items.txt" --header=HEADER --header-border
run_case 'header-border-sharp' 'sleep 300'    "$WORK/items.txt" --header=HEADER --header-border=sharp
run_case 'header-border-reverse' 'sleep 300'  "$WORK/items.txt" --reverse --header=HEADER --header-border=sharp
run_case 'header-label' 'sleep 300' "$WORK/items.txt" --header=HEADER --header-border=sharp --header-label='GROUPS'
run_case 'header-label-pos' 'sleep 300' "$WORK/items.txt" --header=HEADER --header-border=sharp --header-label='GROUPS' --header-label-pos=4
run_case 'header-label-center' 'sleep 300' "$WORK/items.txt" --header=HEADER --header-border=sharp --header-label='GROUPS' --header-label-pos=center
run_case 'header-label-bottom' 'sleep 300' "$WORK/items.txt" --header=HEADER --header-border=sharp --header-label='GROUPS' --header-label-pos=4:bottom
run_case 'input-border-default' 'sleep 300' "$WORK/items.txt" --input-border
run_case 'input-border-sharp' 'sleep 300' "$WORK/items.txt" --input-border=sharp
run_case 'input-border-reverse' 'sleep 300' "$WORK/items.txt" --reverse --input-border=sharp
run_case 'input-label' 'sleep 300' "$WORK/items.txt" --input-border=sharp --input-label='SEARCH'
run_case 'input-label-pos' 'sleep 300' "$WORK/items.txt" --input-border=sharp --input-label='SEARCH' --input-label-pos=4
run_case 'input-label-center' 'sleep 300' "$WORK/items.txt" --input-border=sharp --input-label='SEARCH' --input-label-pos=center
run_case 'input-label-bottom' 'sleep 300' "$WORK/items.txt" --input-border=sharp --input-label='SEARCH' --input-label-pos=4:bottom
run_case 'gentoo-command-layout' 'sleep 600' "$WORK/items.txt" --multi --ansi --style=full --border=rounded --layout=reverse --height=100% --info=inline-right --scroll-off=5 --cycle --prompt='Portage ❯ ' --header='TYPE TO SEARCH | TAB: SELECT MULTIPLE | ENTER: INSTALL | ESC: CANCEL' --preview='printf "preview: %s\\n" "{}"' --preview-window='right,58%,border-left,wrap'
run_case 'no-cycle' 'sleep 300\nkey down\nkey down\nkey down\nkey down\nkey down\nkey down\nsleep 200' "$WORK/items.txt" --no-cycle
run_case 'cycle' 'sleep 300\nkey down\nkey down\nkey down\nkey down\nkey down\nkey down\nsleep 200' "$WORK/items.txt" --cycle
run_case 'no-info'      'sleep 300'            "$WORK/items.txt" --no-info
run_case 'info-inline-right' 'sleep 300'       "$WORK/items.txt" --info=inline-right
run_case 'info-hidden-style' 'sleep 300'       "$WORK/items.txt" --info=hidden
run_case 'info-inline-right-reverse' 'sleep 300' "$WORK/items.txt" --info=inline-right --reverse
run_case 'highlight-line' 'sleep 350
send a
sleep 250'              "$WORK/many.txt" --highlight-line
run_case 'info-command'   'sleep 300'            "$WORK/items.txt" --info-command 'echo INFO'
run_case 'header'         'sleep 300'            "$WORK/items.txt" --header 'HEAD'
run_case 'header-multiline' 'sleep 300'          "$WORK/items.txt" --header $'A\nB'
run_case 'header-first'   'sleep 300'            "$WORK/items.txt" --header 'HEAD' --header-first
run_case 'header-lines'   'sleep 300'            "$WORK/items.txt" --header-lines=2
run_case 'header-lines-header' 'sleep 300'       "$WORK/items.txt" --header-lines=2 --header 'TOP'
run_case 'header-lines-first' 'sleep 300'        "$WORK/items.txt" --header-lines=2 --header-first
run_case 'header-lines-first-header' 'sleep 300' "$WORK/items.txt" --header-lines=2 --header 'TOP' --header-first
run_case 'reverse-header' 'sleep 300'            "$WORK/items.txt" --reverse --header 'HEAD'
run_case 'reverse-header-first' 'sleep 300'      "$WORK/items.txt" --reverse --header 'HEAD' --header-first
run_case 'reverse-header-lines' 'sleep 300'      "$WORK/items.txt" --reverse --header-lines=2
run_case 'reverse-header-lines-first' 'sleep 300' "$WORK/items.txt" --reverse --header-lines=2 --header-first
run_case 'footer-reverse' 'sleep 300'            "$WORK/items.txt" --reverse --footer 'FOOT'
run_case 'multi'        'sleep 350
key tab
sleep 150
key tab
sleep 150
key enter
sleep 200'                                        "$WORK/items.txt" --multi
run_case 'prompt'       'sleep 300'            "$WORK/items.txt" --prompt 'search> '
run_case 'expect'       'sleep 350
send app
sleep 150
key ctrl-a
sleep 200'                                        "$WORK/items.txt" --expect=ctrl-a

run_case 'bind-jump-down'  'sleep 350
key down
sleep 150
key down
sleep 150
key up
sleep 200'                                        "$WORK/items.txt" --bind='up:up,down:down'
run_case 'bind-first-last' 'sleep 350
key end
sleep 150
key home
sleep 200'                                        "$WORK/items.txt" --bind='home:first,end:last'
run_case 'bind-toggle'    'sleep 350
key tab
sleep 150
key enter
sleep 200'                                        "$WORK/items.txt" --bind='tab:toggle' --multi
run_case 'bind-abort'     'sleep 350
key esc
sleep 200'                                        "$WORK/items.txt" --bind='esc:abort'

# Preview window: the layout, the command output and the box are compared.
run_preview_case() {
    name=$1; shift
    # The preview command runs asynchronously; give it time to produce output.
    printf 'sleep 800\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 -- "$@" \
        > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 -- "$@" \
        > "$WORK/impl.txt" 2>/dev/null
    if compare "$WORK/ref.txt" "$WORK/impl.txt" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}

# The history file and the history navigation actions.
run_history_case() {
    name=$1; keys=$2
    hist="$WORK/history.txt"
    printf 'apple\nbanana\ncherry\n' > "$hist"
    printf 'sleep 300\n%s\nsleep 300\n' "$keys" > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 \
        -- --history "$hist" --bind 'alt-up:prev-history,alt-down:next-history' \
        > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 \
        -- --history "$hist" --bind 'alt-up:prev-history,alt-down:next-history' \
        > "$WORK/impl.txt" 2>/dev/null
    if compare "$WORK/ref.txt" "$WORK/impl.txt" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}

run_history_case 'history-prev'  'key alt-up'
run_history_case 'history-prev2' 'key alt-up
key alt-up'
run_history_case 'history-next'  'key alt-down'

# The print and become actions write to stdout, which the harness captures.
run_stdout_case() {
    name=$1; bind=$2; keys=$3
    printf '%s' "$keys" > "$WORK/script.txt"
    # The logical screen is compared as well, so that only the output of
    # the action matters and not the exact escape sequences.
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --stdout -- --bind "$bind" \
        > "$WORK/ref.out" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --stdout -- --bind "$bind" \
        > "$WORK/impl.out" 2>/dev/null
    if compare "$WORK/ref.out" "$WORK/impl.out" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.out" "$WORK/impl.out" | head -10
    fi
}

run_stdout_case 'action-print'  'ctrl-t:print(hello)' 'sleep 350
key ctrl-t
sleep 150
key enter
sleep 300'
run_stdout_case 'action-become' 'ctrl-t:become(echo BECAME)' 'sleep 350
key ctrl-t
sleep 300'

# The reload action replaces the list with the output of a command.
run_reload_case() {
    name=$1; bind=$2
    printf 'sleep 200\nkey ctrl-t\nsleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color -- --bind "$bind" \
        > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color -- --bind "$bind" \
        > "$WORK/impl.c" 2>/dev/null
    if diff -q "$WORK/ref.c" "$WORK/impl.c" > /dev/null; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -10
    fi
}

run_reload_case 'action-reload' 'ctrl-t:reload(seq 1 3)'
run_reload_case 'action-reload-empty' 'ctrl-t:reload(true)'

run_reload_track_case() {
    name=$1; args=$2
    printf 'sleep 200\nkey up\nsleep 100\nkey ctrl-t\nsleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color -- $args \
        --bind 'ctrl-t:reload(seq 1 3)' > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color -- $args \
        --bind 'ctrl-t:reload(seq 1 3)' > "$WORK/impl.c" 2>/dev/null
    if diff -q "$WORK/ref.c" "$WORK/impl.c" > /dev/null; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -10
    fi
}
run_reload_track_case 'action-reload-track' '--track'
run_case 'id-nth-reload-track' 'sleep 250\nkey up\nsleep 100\nkey ctrl-t\nsleep 300\nkey enter\nsleep 200' "$WORK/id-items.txt" --track --id-nth=1 --bind='ctrl-t:reload(printf "20 beta updated\\n10 alpha\\n30 gamma\\n")'
run_case 'id-nth-reload-selection' 'sleep 250\nkey up\nkey tab\nsleep 100\nkey ctrl-t\nsleep 300\nkey enter\nsleep 200' "$WORK/id-items.txt" --multi --id-nth=1 --bind='ctrl-t:reload(printf "10 alpha\\n30 gamma\\n20 beta updated\\n")'

run_preview_case 'layout-reverse-list' --layout=reverse-list
run_preview_case 'no-bold'           --no-bold
run_preview_case 'no-color'          --no-color
run_preview_case 'ellipsis-custom'   --ellipsis '...'
run_preview_case 'pointer-custom'    --pointer '>'
run_preview_case 'marker-custom'     --multi --marker '*'
run_preview_case 'multi-select'      --multi
run_preview_case 'preview-default'   --preview 'echo {}'
run_preview_case 'preview-window-comma-border-left' --preview 'echo {}' --preview-window='right,58%,border-left,wrap'
run_preview_case 'preview-query'     --preview 'echo {q}'
run_preview_case 'preview-static'    --preview 'echo hello'
run_preview_case 'preview-scroll'    --preview 'seq 1 20'
run_preview_case 'preview-text'     --preview 'echo hi'
run_preview_case 'gap-1'            --gap=1
run_preview_case 'gap-3'            --gap=3
run_preview_case 'gap-3-preview'    --gap=3 --preview 'echo hi'
run_preview_case 'gap-line'         --gap=2 --gap-line=-
run_preview_case 'preview-wrap'      --preview 'seq 1 30' --preview-window wrap
run_preview_case 'preview-wrap-word' --preview 'printf "one two three four five six seven eight nine ten\\\\n"' --preview-window='right,30%,wrap-word'

run_preview_word_action_case() {
    printf 'sleep 300\nkey ctrl-t\nsleep 500\n' > "$WORK/script.txt"
    local opts=(--preview='printf "one two three four five six seven eight nine ten"' --preview-window='right,30%,nowrap' --bind=ctrl-t:toggle-preview-wrap-word)
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 -- "${opts[@]}" \
        > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols 40 --rows 12 -- "${opts[@]}" \
        > "$WORK/impl.txt" 2>/dev/null
    if compare "$WORK/ref.txt" "$WORK/impl.txt" 'preview-toggle-wrap-word'; then
        pass=$((pass + 1)); printf 'PASS preview-toggle-wrap-word\n'
    else
        fail=$((fail + 1)); printf 'FAIL preview-toggle-wrap-word\n'
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}
run_preview_word_action_case

run_preview_case 'preview-wrap-sign' --preview 'printf "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\\n"' --preview-window='right,30%,wrap' --preview-wrap-sign='>>'
run_preview_case 'preview-wrap-sign-wide' --preview 'printf "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz\\n"' --preview-window='right,30%,wrap' --preview-wrap-sign='·· '
run_preview_case 'preview-long'     --preview 'seq 1 100'
run_preview_case 'preview-right-30'  --preview-window=right:30% --preview 'echo {}'
run_preview_case 'preview-left'      --preview-window=left:50% --preview 'echo {}'
run_preview_case 'preview-top'       --preview-window=top:40% --preview 'echo {}'
run_preview_case 'preview-bottom'    --preview-window=bottom:40% --preview 'echo {}'
run_preview_case 'preview-border'    --preview 'echo {}' --border
run_preview_case 'preview-border-default' --preview 'echo {}' --preview-border
run_preview_case 'preview-border-sharp'   --preview 'echo {}' --preview-border=sharp
run_preview_case 'preview-border-none'    --preview 'echo {}' --preview-border=none
run_preview_case 'preview-label'          --preview 'echo {}' --preview-label='PACKAGE DETAILS'
run_preview_case 'preview-label-sharp'    --preview 'echo {}' --preview-border=sharp --preview-label='DETAILS'
run_preview_case 'preview-label-pos'      --preview 'echo {}' --preview-label='DETAILS' --preview-label-pos=4
run_preview_case 'preview-label-bottom'   --preview 'echo {}' --preview-label='DETAILS' --preview-label-pos=4:bottom
run_preview_case 'preview-header'    --preview 'echo {}' --border --header H
run_preview_case 'layout-no-info'    --no-info
run_preview_case 'layout-header'     --header H

# The list wrapping (--wrap) and the layout options.
run_long_case() {
    name=$1; shift
    printf 'sleep 400\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/long.txt" --screen --color --cols 40 --rows 12 -- "$@" \
        > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/long.txt" --screen --color --cols 40 --rows 12 -- "$@" \
        > "$WORK/impl.c" 2>/dev/null
    if compare "$WORK/ref.c" "$WORK/impl.c" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -12
    fi
}

run_long_case 'wrap'                --wrap
run_long_case 'wrap-sign'           --wrap --wrap-sign '>>> '
run_long_case 'wrap-sign-long'      --wrap --wrap-sign 'LONGSIGN-'
run_long_case 'wrap-gap'            --wrap --gap=2
run_long_case 'wrap-reverse'        --wrap --reverse
run_long_case 'wrap-reverse-list'   --wrap --layout=reverse-list
run_long_case 'layout-reverse'      --layout=reverse
run_long_case 'gap-1-long'          --gap=1
run_long_case 'wrap-word'           --wrap=word
run_long_case 'wrap-word-long'      --wrap-word
run_long_case 'wrap-char'           --wrap=char
run_case 'phony'          'sleep 350
send zzz
sleep 300'                                        "$WORK/items.txt" --phony
run_case 'disabled'       'sleep 350
run_case 'disabled-toggle-search' 'sleep 300\nsend an\nsleep 200\nkey ctrl-s\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --disabled --bind=ctrl-s:toggle-search
run_case 'disabled-enable-search-action' 'sleep 300\nsend an\nsleep 200\nkey ctrl-s\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --disabled --bind=ctrl-s:enable-search
run_case 'disable-search-action' 'sleep 300\nsend an\nsleep 200\nkey ctrl-s\nsleep 250\nkey enter\nsleep 200' "$WORK/items.txt" --bind=ctrl-s:disable-search
send zzz
sleep 300'                                        "$WORK/items.txt" --disabled
run_long_case 'gap-3-long'          --gap=3

# A live stream of items: the list grows while the program is running.
run_stream_case() {
    name=$1
    printf 'sleep 350\nitem second\nsleep 350\nitem third\nsleep 350\nstdin close\nsleep 350\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --keep-stdin --screen --color \
        --cols 40 --rows 12 > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --keep-stdin --screen --color \
        --cols 40 --rows 12 > "$WORK/impl.c" 2>/dev/null
    if compare "$WORK/ref.c" "$WORK/impl.c" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -12
    fi
}

run_stream_case 'live-stream'

# The navigation direction and the scroll offset.
run_scroll_case() {
    name=$1; keys=$2; shift 2
    printf 'sleep 300\n%s\nsleep 300\n' "$keys" > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/many.txt" --screen --color --cols 40 --rows 12 -- "$@" \
        > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/many.txt" --screen --color --cols 40 --rows 12 -- "$@" \
        > "$WORK/impl.c" 2>/dev/null
    if compare "$WORK/ref.c" "$WORK/impl.c" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -12
    fi
}

run_scroll_case 'nav-down'  'key down
key down
key down'
run_scroll_case 'nav-up'    'key up'
run_scroll_case 'nav-mixed' 'key down
key up
key down'
run_scroll_case 'scroll-off-0'  'key down
key down
key down
key down' --scroll-off=0
run_scroll_case 'scroll-off-1'  'key down
key down
key down
key down' --scroll-off=1
run_scroll_case 'scroll-off-5'  'key down
key down
key down
key down' --scroll-off=5
run_scroll_case 'scroll-off-20' 'key down
key down
key down
key down' --scroll-off=20

# The footer, the gutter and the ASCII glyphs.
run_case 'footer'          'sleep 200'                                       "$WORK/items.txt" --footer=FOOT
run_case 'gutter'          'sleep 200'                                       "$WORK/items.txt" --gutter=+
run_case 'gutter-raw'      'sleep 200'                                       "$WORK/items.txt" --gutter-raw=:
run_case 'padding-1'       'sleep 200'                                       "$WORK/items.txt" --padding=1
run_case 'padding-3'       'sleep 200'                                       "$WORK/items.txt" --padding=3
run_case 'tabstop'         'sleep 200'                                       "$WORK/items.txt" --tabstop=4
run_case 'margin-1'        'sleep 200'                                       "$WORK/items.txt" --margin=1
run_case 'margin-2'        'sleep 200'                                       "$WORK/items.txt" --margin=2
run_case 'separator'       'sleep 200'                                       "$WORK/items.txt" --separator=--
run_case 'separator-char'  'sleep 200'                                       "$WORK/items.txt" --separator=|

run_case 'no-unicode-glyphs' 'sleep 350
key tab
sleep 200
key enter
sleep 200'                                        "$WORK/items.txt" --no-unicode --multi

# Border shapes are compared as logical screens.
run_border_case() {
    name=$1; shape=$2; shift 2
    printf 'sleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color --cols 40 --rows 12 \
        -- "--border=$shape" "$@" > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color --cols 40 --rows 12 \
        -- "--border=$shape" "$@" > "$WORK/impl.txt" 2>/dev/null
    if compare "$WORK/ref.txt" "$WORK/impl.txt" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}

for shape in rounded sharp bold double block thinblock dashed horizontal vertical top bottom left right; do
    run_border_case "border-$shape" "$shape"
done
for shape in rounded sharp top bottom horizontal; do
    run_border_case "border-label-$shape" "$shape" --border-label=LABEL
done
run_border_case "border-label-pos-left" rounded --border-label=LABEL --border-label-pos=2:top
run_border_case "border-label-pos-right" rounded --border-label=LABEL --border-label-pos=-2:top
run_border_case "border-label-pos-bottom" rounded --border-label=LABEL --border-label-pos=center:bottom

# The execute action writes the current item to a file; compare the file
# contents produced by the reference and the implementation.
run_execute_case() {
    name=$1; bind=$2
    rm -f "$WORK/exec-ref.txt" "$WORK/exec-impl.txt"
    printf 'sleep 350
key ctrl-t
sleep 300
' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen \
        -- --bind="ctrl-t:execute(echo {} > $WORK/exec-ref.txt)" > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen \
        -- --bind="ctrl-t:execute(echo {} > $WORK/exec-impl.txt)" > "$WORK/impl.txt" 2>/dev/null
    if cmp -s "$WORK/exec-ref.txt" "$WORK/exec-impl.txt"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        printf '  ref : %s\n' "$(cat "$WORK/exec-ref.txt" 2>/dev/null)"
        printf '  impl: %s\n' "$(cat "$WORK/exec-impl.txt" 2>/dev/null)"
    fi
}

run_execute_case 'bind-execute' 'ctrl-t'

run_case 'bind-change-query' 'sleep 350
key ctrl-t
sleep 250'                                        "$WORK/items.txt" --bind='ctrl-t:change-query(cherry)'

# Compare the SGR (color/attribute) codes the two programs emit, so that
# --color handling is verified and not only the text layout.
run_color_case() {
    name=$1; shift
    printf 'sleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/sgrdump.py" "$REF" "$WORK/script.txt" "$WORK/items.txt" "$@" \
        > "$WORK/ref.sgr" 2>/dev/null
    python3 "$HERE/sgrdump.py" "$IMPL" "$WORK/script.txt" "$WORK/items.txt" "$@" \
        > "$WORK/impl.sgr" 2>/dev/null
    if diff -q "$WORK/ref.sgr" "$WORK/impl.sgr" > /dev/null; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.sgr" "$WORK/impl.sgr" | head -20
    fi
}

# Regression: the info line separator used to overflow a fixed stack buffer
# on wide terminals (>= 178 columns) and to underflow on very narrow ones.
run_size_case() {
    name=$1; cols=$2; rows=$3
    printf 'sleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols "$cols" --rows "$rows" \
        > "$WORK/ref.txt" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --cols "$cols" --rows "$rows" \
        > "$WORK/impl.txt" 2>/dev/null
    if compare "$WORK/ref.txt" "$WORK/impl.txt" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.txt" "$WORK/impl.txt" | head -12
    fi
}

run_size_case 'width-80'   80  24
run_size_case 'width-200'  200 24
run_size_case 'width-500'  500 24
run_size_case 'width-20'   20  10
run_size_case 'width-10'   10  6

# The inline layout (--height) is compared at several terminal sizes.
run_height_case() {
    name=$1; height=$2; cols=$3; rows=$4
    printf 'sleep 300\n' > "$WORK/script.txt"
    python3 "$HERE/ptyrun.py" "$REF" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color --cols "$cols" --rows "$rows" \
        "--height=$height" > "$WORK/ref.c" 2>/dev/null
    python3 "$HERE/ptyrun.py" "$IMPL" --script "$WORK/script.txt" \
        --stdin-file "$WORK/items.txt" --screen --color --cols "$cols" --rows "$rows" \
        "--height=$height" > "$WORK/impl.c" 2>/dev/null
    if compare "$WORK/ref.c" "$WORK/impl.c" "$name"; then
        pass=$((pass + 1)); printf 'PASS %s\n' "$name"
    else
        fail=$((fail + 1)); printf 'FAIL %s\n' "$name"
        diff -u "$WORK/ref.c" "$WORK/impl.c" | head -12
    fi
}

run_height_case 'height-percent' 20%  40 12
run_height_case 'height-3'        3     40 12
run_height_case 'height-10'       10    40 12
run_height_case 'height-full'     12    40 12
run_height_case 'height-tall'     20%   40 20
run_height_case 'height-tall-10'  10    40 20
run_height_case 'height-min-5'    20%   40 12
run_height_case 'height-min-8'    20%   40 12
run_height_case 'height-min-3'    20%   40 12
run_height_case 'height-plus'     50%   40 12

run_color_case 'color-default'
run_color_case 'color-pointer-info' --color=pointer:cyan,info:yellow

printf '\n%d/%d interactive cases matched\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
