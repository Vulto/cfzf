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

def measure(binary, root, count, trace_path=None, process_trace=False):
    start = time.perf_counter_ns()
    pid, master = pty.fork()
    if pid == 0:
        os.chdir(root)
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env.pop("FZF_DEFAULT_COMMAND", None)
        env.pop("FZF_DEFAULT_OPTS", None)
        command = [binary, "--walker-root=.", "--no-color"]
        if trace_path:
            if process_trace:
                command = ["strace", "-f", "-e", "trace=process", "-o", trace_path, *command]
            else:
                command = ["strace", "-f", "-c", "-o", trace_path, *command]
        os.execvpe(command[0], command, env)
    fcntl = __import__("fcntl")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 100, 30, 0, 0))
    first = None
    complete = None
    max_rss_kb = 0
    captured = bytearray()
    expected = f"{count}/{count}".encode()
    deadline = time.monotonic() + TIMEOUT
    try:
        while time.monotonic() < deadline and complete is None:
            try:
                with open(f"/proc/{pid}/status", encoding="ascii") as status_file:
                    for line in status_file:
                        if line.startswith("VmHWM:"):
                            max_rss_kb = max(max_rss_kb, int(line.split()[1]))
                            break
            except (FileNotFoundError, ProcessLookupError):
                pass
            ready, _, _ = select.select([master], [], [], 0.01)
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
        try:
            with open(f"/proc/{pid}/status", encoding="ascii") as status_file:
                for line in status_file:
                    if line.startswith("VmHWM:"):
                        max_rss_kb = max(max_rss_kb, int(line.split()[1]))
                        break
        except (FileNotFoundError, ProcessLookupError):
            pass
        return first, complete, max_rss_kb
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
            deadline = time.monotonic() + 2.0
            status = None
            while time.monotonic() < deadline:
                try:
                    waited, child_status = os.waitpid(pid, os.WNOHANG)
                except ChildProcessError:
                    status = 0
                    break
                if waited == pid:
                    status = child_status
                    break
                time.sleep(0.02)
            if status is None:
                os.kill(pid, 9)
                _, status = os.waitpid(pid, 0)
        except ProcessLookupError:
            status = 0

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
        samples = {"ref_first": [], "ref_full": [], "ref_rss_kb": [],
                   "cfzf_first": [], "cfzf_full": [], "cfzf_rss_kb": []}
        # One warmup per binary; alternate order for the measured repetitions.
        for binary in (REF, IMPL):
            measure(binary, root, count)
        for i in range(REPEATS):
            order = ((REF, "ref"), (IMPL, "cfzf")) if i % 2 == 0 else ((IMPL, "cfzf"), (REF, "ref"))
            for binary, prefix in order:
                first, full, rss_kb = measure(binary, root, count)
                samples[f"{prefix}_first"].append(first)
                samples[f"{prefix}_full"].append(full)
                samples[f"{prefix}_rss_kb"].append(rss_kb)
        rf, rfmad = summarize(samples["ref_first"])
        cf, cfmad = summarize(samples["cfzf_first"])
        rt, rt_mad = summarize(samples["ref_full"])
        ct, ct_mad = summarize(samples["cfzf_full"])
        rrss, rrss_mad = summarize(samples["ref_rss_kb"])
        crss, crss_mad = summarize(samples["cfzf_rss_kb"])
        print(f"files={count} shape=flat "
              f"first_ref_median_ms={rf:.3f} first_ref_MAD_ms={rfmad:.3f} "
              f"first_cfzf_median_ms={cf:.3f} first_cfzf_MAD_ms={cfmad:.3f} "
              f"first_speedup={rf/cf:.3f} "
              f"full_ref_median_ms={rt:.3f} full_ref_MAD_ms={rt_mad:.3f} "
              f"full_cfzf_median_ms={ct:.3f} full_cfzf_MAD_ms={ct_mad:.3f} "
              f"full_speedup={rt/ct:.3f} "
              f"ref_paths_per_s={count*1000/rt:.0f} cfzf_paths_per_s={count*1000/ct:.0f} "
              f"peak_rss_ref_median_kb={rrss:.0f} peak_rss_ref_MAD_kb={rrss_mad:.0f} "
              f"peak_rss_cfzf_median_kb={crss:.0f} peak_rss_cfzf_MAD_kb={crss_mad:.0f}")
        for key, values in samples.items():
            print(f"raw files={count} metric={key} samples=" +
                  ",".join(f"{v:.3f}" for v in values))
        if count == 1_000:
            # One syscall-count profile per binary. This is not timed evidence:
            # strace perturbs runtime, so only syscall counts are compared.
            wanted = {"getdents64", "newfstatat", "statx", "openat",
                      "read", "write", "writev", "close"}
            for binary, prefix in ((REF, "ref"), (IMPL, "cfzf")):
                trace_path = os.path.join(work, f"strace-{prefix}.txt")
                measure(binary, root, count, trace_path)
                calls = {}
                with open(trace_path, encoding="utf-8") as trace_file:
                    for line in trace_file:
                        parts = line.split()
                        if len(parts) < 5 or parts[-1] not in wanted:
                            continue
                        try:
                            float(parts[0]); float(parts[1])
                            int(parts[2])
                            calls[parts[-1]] = int(parts[3])
                        except ValueError:
                            continue
                exec_trace = os.path.join(work, f"exec-{prefix}.txt")
                measure(binary, root, count, exec_trace, process_trace=True)
                with open(exec_trace, encoding="utf-8") as process_file:
                    execve_count = sum(
                        "execve(" in line or "execveat(" in line
                        for line in process_file
                    )
                if execve_count != 1:
                    raise RuntimeError(
                        f"{prefix} default walker spawned unexpected executable "
                        f"processes: execve_count={execve_count}")
                print(f"processes files=1000 binary={prefix} "
                      f"execve_count={execve_count} child_exec_count={execve_count - 1}")
                print(f"syscalls files=1000 binary={prefix} "
                      f"selected_total={sum(calls.values())} " +
                      " ".join(f"{name}={calls.get(name, 0)}"
                               for name in sorted(wanted)))
