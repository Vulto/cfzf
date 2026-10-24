#!/usr/bin/env python3
"""Print the distinct SGR (color/attribute) codes a program emits under a
pty. Used by tests/pty-diff.sh to verify --color handling, which the logical
screen comparison cannot see."""
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ptyrun import run_pty  # noqa: E402


def main():
    binary = sys.argv[1]
    script = sys.argv[2]
    items = sys.argv[3]
    args = sys.argv[4:]
    with open(script) as f:
        script_lines = f.readlines()
    stdin_data = None
    if items and items != "-":
        with open(items, "rb") as f:
            stdin_data = f.read()
    _, out, _, _ = run_pty(binary, args, script_lines, 80, 24,
                           stdin_data=stdin_data)
    # Compare the effective colors rather than the exact SGR strings: the
    # two renderers group the parameters differently, but the palette they
    # use must be the same.
    colors = set()
    for sgr in re.findall(rb"\x1b\[;([0-9;]*)m", out):
        params = sgr.decode().split(";")
        i = 0
        while i < len(params):
            p = params[i]
            if p in ("38", "48") and i + 2 < len(params) and params[i + 1] == "5":
                if not (p == "38" and params[i + 2] == "148"):
                    # fzf's spinner colour (148) is only shown while the input
                    # is still being read; the C23 port reads the input
                    # synchronously and never shows a spinner.
                    colors.add(f"{p};5;{params[i + 2]}")
                i += 3
                continue
            if p.isdigit() and 30 <= int(p) <= 37:
                colors.add(p)
            elif p.isdigit() and 90 <= int(p) <= 97:
                colors.add(p)
            elif p.isdigit() and 40 <= int(p) <= 47:
                colors.add(p)
            elif p.isdigit() and 100 <= int(p) <= 107:
                colors.add(p)
            elif p.isdigit() and int(p) == 148:
                # fzf's spinner colour, used only while the input is still
                # being read. The C23 port reads the input synchronously and
                # never shows a spinner, so this colour is not comparable.
                pass
            i += 1
    for c in sorted(colors):
        print(c)


if __name__ == "__main__":
    main()
