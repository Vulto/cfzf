#!/usr/bin/env python3
"""Differential test harness: run reference fzf and the C23 implementation on
identical inputs/args and compare stdout, stderr, and exit status.

Usage:
    tests/diff.py --ref /usr/bin/fzf --impl ./fzf [--normalize] < casefile

A "case" file is a simple driver script (see tests/cases/*.txt) OR, when no
case file is given, a list of argv-vector specs read from stdin. For
convenience we support a compact JSON-lines format:

    {"args": ["--filter","a"], "stdin": "foo\nbar\n"}
    {"args": [...], "stdin": "...", "env": {...}}

Each case is executed against both binaries; a mismatch is reported with a
diff and the harness exits non-zero. Nondeterministic runs can be normalized
with --normalize (strips terminal control noise, pids, timestamps).
"""
import argparse
import json
import subprocess
import sys
import difflib
import re
import os


def normalize(data: bytes) -> bytes:
    if not data:
        return data
    # Strip ANSI/terminal control sequences and CR, normalise line endings.
    text = data.decode("utf-8", "surrogateescape")
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "<ESC>", text)
    text = re.sub(r"\x1b[()][A-Za-z0-9]", "<ESC>", text)
    text = re.sub(r"\d{4}-\d\d-\d\d[T ]\d\d:\d\d:\d\d[^\s]*", "<TS>", text)
    text = text.replace("\r\n", "\n")
    return text.encode("utf-8", "surrogateescape")


def run(binary, args, stdin_bytes, env):
    e = os.environ.copy()
    e.update(env)
    try:
        p = subprocess.run([binary, *args], input=stdin_bytes,
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           env=e, timeout=30)
        return p.returncode, p.stdout, p.stderr
    except subprocess.TimeoutExpired:
        return "TIMEOUT", b"", b""


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ref", default="/usr/bin/fzf")
    ap.add_argument("--impl", default="./fzf")
    ap.add_argument("--normalize", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    cases = []
    for line in sys.stdin:
        line = line.strip()
        if not line or line.startswith("#"):
            continue
        cases.append(json.loads(line))

    if not cases:
        print("no cases provided on stdin", file=sys.stderr)
        return 2

    passed = failed = 0
    for i, c in enumerate(cases, 1):
        argv = c.get("args", [])
        sin = c.get("stdin", "").encode("utf-8", "surrogateescape")
        env = c.get("env", {})
        rc_r, out_r, err_r = run(args.ref, argv, sin, env)
        rc_i, out_i, err_i = run(args.impl, argv, sin, env)
        if args.normalize:
            out_r, out_i, err_r, err_i = (normalize(out_r), normalize(out_i),
                                          normalize(err_r), normalize(err_i))
        ok = (rc_r == rc_i and out_r == out_i and err_r == err_i)
        if c.get("stdout_only"):
            ok = (out_r == out_i and rc_r == rc_i)
        if ok:
            passed += 1
            if args.verbose:
                print(f"PASS #{i}: {argv}")
        else:
            failed += 1
            print(f"FAIL #{i}: args={argv}")
            print(f"  ref  exit={rc_r!r}  impl exit={rc_i!r}")
            if out_r != out_i:
                d = difflib.unified_diff(
                    out_r.decode('utf-8', 'replace').splitlines(),
                    out_i.decode('utf-8', 'replace').splitlines(),
                    "ref.stdout", "impl.stdout", lineterm="")
                print("\n".join("  " + x for x in d))
            if err_r != err_i:
                print(f"  ref.stderr={err_r!r}")
                print(f"  impl.stderr={err_i!r}")

    print(f"\n{passed}/{passed+failed} cases matched")
    return 0 if failed == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
