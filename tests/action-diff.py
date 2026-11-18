#!/usr/bin/env python3
"""Differential checks for actions that were missing from the C port.

Shell integrations --fish, --zsh, and --nushell are out of scope and are
not compared with fzf.
"""
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REF = os.environ.get("REF", "/tmp/opencode/fzf-ref")
IMPL = os.environ.get("IMPL", os.path.join(os.path.dirname(HERE), "cfzf"))
ITEMS = "alpha\nbeta\ngamma\n"


def run(binary, args, keys, stdin=ITEMS, timeout=4.0):
    with tempfile.TemporaryDirectory() as work:
        script = os.path.join(work, "script.txt")
        src = os.path.join(work, "items.txt")
        with open(script, "w", encoding="utf-8") as f:
            f.write(keys)
        with open(src, "w", encoding="utf-8") as f:
            f.write(stdin)
        proc = subprocess.run(
            [sys.executable, os.path.join(HERE, "ptyrun.py"), binary,
             "--script", script, "--stdin-file", src, "--stdout",
             "--timeout", str(timeout), "--", *args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
    text = proc.stdout.decode("utf-8", "replace")
    err = proc.stderr.decode("utf-8", "replace")
    marked = re.findall(r"RESULT (.*?)$", text, re.M)
    exit_code = None
    for line in err.splitlines():
        if line.startswith("[exit ") and line.endswith("]"):
            exit_code = int(line[6:-1])
    return marked, exit_code, text


def main():
    press = "sleep 250\nkey ctrl-t\nsleep 250\n"
    cases = [
        ("search-item", ["--bind", "ctrl-t:search(beta)+become(printf 'RESULT %s\\n' {})"], press, None),
        ("search-display", ["--bind", "ctrl-t:search(beta)+become(printf 'RESULT Q[%s]\\n' {q})"], press, None),
        ("kill-subword", ["--query", "fooBarBaz", "--bind",
                          "ctrl-t:beginning-of-line+kill-subword+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("kill-subword-sep", ["--query", "foo_bar", "--bind",
                              "ctrl-t:beginning-of-line+kill-subword+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("eof-empty", ["--bind", "ctrl-t:delete-char/eof+become(printf 'RESULT NO\\n')"], press, 130),
        ("eof-keep", ["--query", "ab", "--bind",
                      "ctrl-t:delete-char/eof+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("eof-delete", ["--query", "ab", "--bind",
                        "ctrl-t:beginning-of-line+delete-char/eof+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("beof-empty", ["--bind", "ctrl-t:backward-delete-char/eof+become(printf 'RESULT NO\\n')"], press, 130),
        ("beof-delete", ["--query", "ab", "--bind",
                         "ctrl-t:backward-delete-char/eof+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("hide-input", ["--bind", "ctrl-t:hide-input+become(printf 'RESULT %s\\n' {})"], press, None),
        ("hide-already-hidden", ["--no-input", "--bind",
                                 "ctrl-t:hide-input+become(printf 'RESULT RAN\\n')"], press, None),
        ("toggle-bind", ["--bind",
                         "ctrl-t:toggle-bind(enter)+toggle-bind(enter),enter:become(printf 'RESULT ON\\n')"],
         "sleep 250\nkey ctrl-t\nsleep 100\nkey enter\nsleep 200\n", None),
        ("transform-query", ["--bind",
                             "ctrl-t:transform-query(printf beta)+become(printf 'RESULT %s\\n' {})"], press, None),
        ("transform-search", ["--bind",
                              "ctrl-t:transform-search(printf beta)+become(printf 'RESULT Q[%s] %s\\n' {q} {})"], press, None),
        ("trigger", ["--bind", "ctrl-x:become(printf 'RESULT T\\n'),ctrl-t:trigger(ctrl-x)"], press, None),
        ("search-then-accept", ["--bind",
                                "ctrl-t:search(beta),enter:become(printf 'RESULT %s\\n' {})"],
         "sleep 250\nkey ctrl-t\nsleep 200\nkey enter\nsleep 200\n", None),
        ("transform-query-then-accept", ["--bind",
                                         "ctrl-t:transform-query(printf beta),enter:become(printf 'RESULT %s\\n' {})"],
         "sleep 250\nkey ctrl-t\nsleep 200\nkey enter\nsleep 200\n", None),
        ("yank", ["--query", "fooBar", "--bind",
                  "ctrl-t:beginning-of-line+kill-subword+yank+become(printf 'RESULT %s\\n' {q})"], press, None),
        ("ctrl-d-eof", [], "sleep 200\nkey ctrl-d\nsleep 150\n", 130),
    ]
    failed = 0
    for name, args, keys, expect_exit in cases:
        ref = run(REF, args, keys)
        impl = run(IMPL, args, keys)
        ok = ref[0] == impl[0] and ref[1] == impl[1]
        if expect_exit is not None and impl[1] != expect_exit:
            ok = False
        if ok:
            print(f"PASS {name} exit={impl[1]} lines={impl[0]}")
        else:
            failed += 1
            print(f"FAIL {name}")
            print(f"  ref  exit={ref[1]} lines={ref[0]}")
            print(f"  cfzf exit={impl[1]} lines={impl[0]}")
    shell_fail = 0
    for flag in ("--fish", "--zsh", "--nushell"):
        proc = subprocess.run([IMPL, flag], stdout=subprocess.PIPE, stderr=subprocess.PIPE, check=False)
        err = proc.stderr.decode()
        if proc.returncode != 2 or f"unknown option: {flag}" not in err:
            shell_fail += 1
            print(f"FAIL shell {flag} rc={proc.returncode} err={err!r}")
        else:
            print(f"PASS shell {flag} rejected")
    help_text = subprocess.run([IMPL, "--help"], stdout=subprocess.PIPE, text=True, check=False).stdout
    for word in ("--fish", "--zsh", "--nushell"):
        if word in help_text:
            shell_fail += 1
            print(f"FAIL help still documents {word}")
    if "--bash" not in help_text:
        shell_fail += 1
        print("FAIL help dropped --bash")
    else:
        print("PASS help keeps --bash and omits fish/zsh/nushell")
    if failed or shell_fail:
        print(f"{failed + shell_fail} failed")
        return 1
    print(f"all {len(cases)} action cases passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
