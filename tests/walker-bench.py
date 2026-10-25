#!/usr/bin/env python3
"""Measure fzf's interactive filesystem walker through a PTY.

The first visible marker measures time until a candidate is displayed. The
N/N info counter measures when all expected files have entered the result set.
The tree is identical for both binaries; timings are not reported unless both
binaries reach both markers. This is a coarse CLI-level measure, not a claim
to isolate individual filesystem syscalls.
"""
import errno
import os
import pty
import select
import statistics
import struct
import subprocess
import sys
import tempfile
import termios
import time

REF, IMPL = sys.argv[1:3]
REPEATS = int(os.environ.get("WALK_BENCH_REPEATS", "5"))
if REPEATS < 3:
    raise SystemExit("WALK_BENCH_REPEATS must be at least 3")
TIMEOUT = float(os.environ.get("WALK_BENCH_TIMEOUT", "45"))
SIZES = (1_000, 5_000, 20_000)
MARKER = b"a-first-marker.txt"
TERM_REPLY = b"\x1b[1;1R\x1b[?2004;2$y\x1b[1;1R"

def make_tree(root, count):
    # Flat directories intentionally expose the cost of reading, classifying,
    # allocating and sorting a large single-directory entry set.
    with open(os.path.join(root, "a-first-marker.txt"), "w") as f:
        f.write("marker\n")
    for i in range(count - 2):
        with open(os.path.join(root, f"file-{i:08d}.txt"), "w") as f:
            f.write("payload\n")
    with open(os.path.join(root, "z-last-marker.txt"), "w") as f:
        f.write("marker\n")

def measure(binary, root, count):
    pid, master = pty.fork()
    if pid == 0:
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        os.execvpe(binary, [binary, "--walker-root=.", "--no-color"], env)
    fcntl = __import__("fcntl")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 100, 30, 0, 0))
    start = time.perf_counter_ns()
    first = None
    complete = None
    captured = bytearray()
    expected = f"{count}/{count}".encode()
    deadline = time.monotonic() + TIMEOUT
    try:
        while time.monotonic() < deadline and complete is None:
            ready, _, _ = select.select([master], [], [], 0.05)
            if not ready:
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError as e:
                if e.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            now = time.perf_counter_ns()
            captured.extend(chunk)
            if b"\x1b[6n" in chunk:
                try:
                    os.write(master, TERM_REPLY)
                except OSError:
                    pass
            if first is None and MARKER in captured:
                first = (now - start) / 1_000_000
            if expected in captured:
                complete = (now - start) / 1_000_000
        if first is None or complete is None:
            raise RuntimeError(
                f"{binary} did not reach markers for {count} files "
                f"(first={first}, complete={complete}, captured={len(captured)} bytes)"
            )
        return first, complete
    finally:
        try:
            os.write(master, b"\x03")
        except OSError:
            pass
        try:
            os.close(master)
        except OSError:
            pass
        try:
            _, status = os.waitpid(pid, 0)
        except ChildProcessError:
            status = 0
        if not os.WIFEXITED(status) and not os.WIFSIGNALED(status):
            raise RuntimeError(f"could not reap {binary} walker process")

def summarize(samples):
    med = statistics.median(samples)
    mad = statistics.median(abs(x - med) for x in samples)
    return med, mad

print("benchmark=cfzf-vs-pinned-fzf-walker-v1")
print(f"reference={os.path.realpath(REF)} implementation={os.path.realpath(IMPL)}")
print(f"repeats={REPEATS} timeout_seconds={TIMEOUT} sizes={','.join(map(str,SIZES))}")
with tempfile.TemporaryDirectory(prefix="cfzf-walker-bench-") as work:
    for count in SIZES:
        root = os.path.join(work, f"tree-{count}")
        os.mkdir(root)
        make_tree(root, count)
        samples = {"ref_first": [], "ref_full": [], "cfzf_first": [], "cfzf_full": []}
        # One warmup per binary; alternate order for the measured repetitions.
        for binary in (REF, IMPL):
            measure(binary, root, count)
        for i in range(REPEATS):
            order = ((REF, "ref"), (IMPL, "cfzf")) if i % 2 == 0 else ((IMPL, "cfzf"), (REF, "ref"))
            for binary, prefix in order:
                first, full = measure(binary, root, count)
                samples[f"{prefix}_first"].append(first)
                samples[f"{prefix}_full"].append(full)
        rf, rfmad = summarize(samples["ref_first"])
        cf, cfmad = summarize(samples["cfzf_first"])
        rt, rt_mad = summarize(samples["ref_full"])
        ct, ct_mad = summarize(samples["cfzf_full"])
        print(f"files={count} shape=flat "
              f"first_ref_median_ms={rf:.3f} first_ref_MAD_ms={rfmad:.3f} "
              f"first_cfzf_median_ms={cf:.3f} first_cfzf_MAD_ms={cfmad:.3f} "
              f"first_speedup={rf/cf:.3f} "
              f"full_ref_median_ms={rt:.3f} full_ref_MAD_ms={rt_mad:.3f} "
              f"full_cfzf_median_ms={ct:.3f} full_cfzf_MAD_ms={ct_mad:.3f} "
              f"full_speedup={rt/ct:.3f}")
        for key, values in samples.items():
            print(f"raw files={count} metric={key}_ms samples=" +
                  ",".join(f"{v:.3f}" for v in values))
