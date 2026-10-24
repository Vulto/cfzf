#!/usr/bin/env python3
"""Pseudo-terminal differential harness for interactive fzf behavior.

Runs a binary under a pty with a fixed size, writes a scripted keystroke
sequence, and captures the resulting screen. Two binaries can be compared.

Keystroke script syntax (one per line):
    send TEXT            # type literal text
    key  NAME            # named key: enter esc ctrl-c up down tab bspace
    sleep MS             # wait
    resize COLS ROWS     # set window size (SIGWINCH)

Because full-screen TUIs repaint continuously, comparison is done on a
normalized capture: we strip the alternate-screen setup/teardown, absolute
cursor addressing, and collapse to the final visible cells via pyte if
available; otherwise we compare the raw capture with escapes stripped.
"""
import argparse
import os
import pty
import select
import signal
import struct
import sys
import time
import fcntl
import termios
import subprocess
import sys as _sys

_sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

KEYMAP = {
    "enter": b"\r",
    "esc": b"\x1b",
    "tab": b"\t",
    "bspace": b"\x7f",
    "up": b"\x1b[A",
    "down": b"\x1b[B",
    "right": b"\x1b[C",
    "left": b"\x1b[D",
    "ctrl-c": b"\x03",
    "ctrl-a": b"\x01",
    "ctrl-e": b"\x05",
    "ctrl-u": b"\x15",
    "ctrl-k": b"\x0b",
    "ctrl-w": b"\x17",
    "ctrl-j": b"\x0a",
    "ctrl-n": b"\x0e",
    "ctrl-p": b"\x10",
    "ctrl-t": b"\x14",
    "ctrl-r": b"\x12",
    "ctrl-o": b"\x0f",
    "ctrl-s": b"\x13",
    "ctrl-v": b"\x16",
    "ctrl-x": b"\x18",
    "ctrl-y": b"\x19",
    "ctrl-z": b"\x1a",
    "f1": b"\x1bOP",
    "f2": b"\x1bOQ",
    "page-up": b"\x1b[5~",
    "page-down": b"\x1b[6~",
    "home": b"\x1b[H",
    "end": b"\x1b[F",
    "delete": b"\x1b[3~",
    "alt-up": b"\x1b[1;3A",
    "alt-down": b"\x1b[1;3B",
    "alt-left": b"\x1b[1;3D",
    "alt-right": b"\x1b[1;3C",
    "shift-tab": b"\x1b[Z",
}


def set_winsize(fd, cols, rows):
    fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", rows, cols, 0, 0))


def _child_setup(slave_fd):
    """Put the pty in a new session and make it the controlling terminal so
    that /dev/tty inside the child refers to the pty."""
    os.setsid()
    fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)


def _child_setup_std(slave_fd):
    """Fallback: claim the controlling terminal via an already-open fd."""
    try:
        os.setsid()
    except OSError:
        pass
    try:
        fcntl.ioctl(slave_fd, termios.TIOCSCTTY, 0)
    except OSError:
        pass


def run_pty(binary, args, script, cols=80, rows=24, env=None, timeout=10.0,
            stdin_data=None, cwd=None, keep_stdin=False):
    master, slave = pty.openpty()
    set_winsize(slave, cols, rows)
    e = os.environ.copy()
    e["TERM"] = "xterm-256color"
    if env:
        e.update(env)
    # Model real usage: the item list arrives on stdin (a pipe, like
    # "ls | fzf"), the selection goes to stdout (a pipe), and the UI is
    # drawn on the controlling terminal (/dev/tty), which is the pty.
    child_stdin = subprocess.PIPE if stdin_data is not None else slave
    proc = subprocess.Popen([binary, *args], stdin=child_stdin,
                            stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                            env=e, close_fds=True, pass_fds=(slave,), cwd=cwd,
                            preexec_fn=lambda: _child_setup(slave))
    os.close(slave)
    if stdin_data is not None:
        try:
            proc.stdin.write(stdin_data)
            if not keep_stdin:
                proc.stdin.close()
            else:
                proc.stdin.flush()
        except (BrokenPipeError, OSError):
            pass

    out = bytearray()
    deadline = time.time() + timeout
    step = 0

    def answer_queries(chunk):
        # The programs ask the terminal for its capabilities at startup and
        # wait for the answers: the cursor position, the bracketed paste mode
        # and the cursor position again (src/tui/light.go startupQueries).
        if b"\x1b[6n" not in chunk:
            return
        try:
            os.write(master, b"\x1b[1;1R\x1b[?2004;2$y\x1b[1;1R")
        except OSError:
            pass

    def drain(dur):
        nonlocal out
        end = time.time() + dur
        while time.time() < end:
            r, _, _ = select.select([master], [], [], 0.05)
            if master in r:
                try:
                    chunk = os.read(master, 65536)
                except OSError:
                    return
                if not chunk:
                    return
                out += chunk
                answer_queries(chunk)

    # Wait for the program to draw its first screen, so that the keys are
    # not delivered before the terminal is set up.
    ready = time.time() + 3.0
    while not out and time.time() < ready:
        drain(0.05)
    drain(0.2)
    while step < len(script) and time.time() < deadline:
        line = script[step].strip()
        step += 1
        if not line or line.startswith("#"):
            continue
        parts = line.split(None, 1)
        cmd = parts[0]
        arg = parts[1] if len(parts) > 1 else ""
        if cmd == "send":
            os.write(master, arg.encode("utf-8", "surrogateescape"))
        elif cmd == "key":
            os.write(master, KEYMAP.get(arg, arg.encode()))
        elif cmd == "item":
            # Append a complete item to the live standard input stream.
            try:
                proc.stdin.write(arg.encode("utf-8", "surrogateescape") + b"\n")
                proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                pass
        elif cmd == "stdin":
            # Write to the standard input of the program, so that a live
            # stream of items can be simulated.
            try:
                if arg == "close":
                    proc.stdin.close()
                else:
                    proc.stdin.write(arg.encode("utf-8", "surrogateescape"))
                    proc.stdin.flush()
            except (BrokenPipeError, OSError, ValueError):
                pass
        elif cmd == "sleep":
            drain(int(arg) / 1000.0)
            continue
        elif cmd == "resize":
            c, r = arg.split()
            set_winsize(master, int(c), int(r))
            os.kill(proc.pid, signal.SIGWINCH)
        else:
            os.write(master, line.encode())
        drain(0.15)

    # Give the program time to finish its last redraw, so that the screen is
    # not captured in the middle of a frame.
    drain(0.6)
    try:
        stdout, stderr = proc.communicate(timeout=0.5)
    except subprocess.TimeoutExpired:
        # The program may have spawned preview/execute children that inherited
        # stdout or stderr. Killing only the parent can leave communicate()
        # blocked forever on those pipes. The child starts a new session above,
        # so terminate its process group to enforce the harness timeout.
        try:
            os.killpg(proc.pid, signal.SIGKILL)
        except ProcessLookupError:
            proc.kill()
        stdout, stderr = proc.communicate()
    try:
        os.close(master)
    except OSError:
        pass
    return proc.returncode, bytes(out), stdout or b"", stderr or b""


def strip_escapes(data: bytes) -> str:
    import re
    text = data.decode("utf-8", "replace")
    text = re.sub(r"\x1b\[[0-9;?]*[ -/]*[@-~]", "", text)
    text = re.sub(r"\x1b[()][A-Za-z0-9]", "", text)
    text = re.sub(r"\x1b[=>]", "", text)
    text = re.sub(r"\x1b\][^\x07]*\x07", "", text)
    return text


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("binary")
    ap.add_argument("--cols", type=int, default=80)
    ap.add_argument("--rows", type=int, default=24)
    ap.add_argument("--script", required=True, help="keystroke script file")
    ap.add_argument("--stdin-file", help="file whose contents are piped to stdin")
    ap.add_argument("--stdin", help="literal string piped to stdin")
    ap.add_argument("--keep-stdin", action="store_true",
                    help="keep stdin open so that the script can append items")
    ap.add_argument("--timeout", type=float, default=10.0)
    ap.add_argument("--screen", action="store_true",
                    help="render the final logical screen instead of raw output")
    ap.add_argument("--stdout", action="store_true",
                    help="also print the captured stdout (the selection)")
    ap.add_argument("--cwd", help="working directory for the child process")
    ap.add_argument("--color", action="store_true",
                    help="include the foreground colour of every cell")
    ap.add_argument("args", nargs="*")
    # The options of the program under test are separated from the options
    # of this script by a "--".  Without the separator they are collected as
    # the unknown arguments, but their order would be lost.
    argv = sys.argv[1:]
    prog_args = []
    if "--" in argv:
        idx = argv.index("--")
        prog_args = argv[idx + 1:]
        argv = argv[:idx]
    args, extra = ap.parse_known_args(argv)
    args.args = prog_args if prog_args else args.args + extra

    with open(args.script) as f:
        script = f.readlines()

    stdin_data = None
    if args.stdin_file:
        with open(args.stdin_file, "rb") as f:
            stdin_data = f.read()
    elif args.stdin is not None:
        stdin_data = args.stdin.encode()

    rc, out, stdout, stderr = run_pty(args.binary, args.args, script,
                                      args.cols, args.rows,
                                      timeout=args.timeout,
                                      stdin_data=stdin_data,
                                      cwd=args.cwd,
                                      keep_stdin=args.keep_stdin)
    if args.screen:
        import screen
        if args.color:
            sc = screen.Screen(args.cols, args.rows).feed(out)
            sys.stdout.write(sc.colored())
        else:
            sys.stdout.write(screen.render(out, args.cols, args.rows))
    else:
        sys.stdout.write(strip_escapes(out))
    if args.stdout:
        sys.stdout.write(stdout.decode("utf-8", "replace"))
    sys.stderr.write(f"\n[exit {rc}]\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
