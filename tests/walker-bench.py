#!/usr/bin/env python3
"""Measure fzf's interactive filesystem walker through a PTY.

The first visible generated-file prefix measures time until a candidate is displayed. The
N/N info counter measures when all expected files have entered the result set.
The tree is identical for both binaries; timings are not reported unless both
binaries reach both markers. This is a coarse CLI-level measure, not a claim
to isolate individual filesystem syscalls.
"""
import errno
import signal
import os
import pty
import select
import statistics
import struct
from collections import deque
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
SIZES = (1_000, 5_000, 20_000, 100_000)
MARKER = b"file-"
TERM_REPLY = b"\x1b[1;1R\x1b[?2004;2$y\x1b[1;1R"

def make_tree(root, count, shape):
    # Flat stresses one large readdir/sort; nested stresses recursive fan-out;
    # deep stresses path construction and recursion through 50 directory levels.
    if shape == "flat":
        dirs = [root]
    elif shape == "nested":
        dir_count = min(500, max(10, count // 200))
        dirs = [os.path.join(root, f"d{i:04d}") for i in range(dir_count)]
        for directory in dirs:
            os.mkdir(directory)
    elif shape == "deep":
        directory = root
        for i in range(50):
            directory = os.path.join(directory, f"d{i:03d}")
            os.mkdir(directory)
        dirs = [directory]
    else:
        raise ValueError(f"unknown tree shape: {shape}")

    with open(os.path.join(dirs[0], "a-first-marker.txt"), "w") as f:
        f.write("marker\n")
    for i in range(count - 2):
        parent = dirs[i % len(dirs)] if shape == "nested" else dirs[0]
        with open(os.path.join(parent, f"file-{i:08d}.txt"), "w") as f:
            f.write("payload\n")
    with open(os.path.join(dirs[-1], "z-last-marker.txt"), "w") as f:
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
        # Fast walkers may finish before the terminal refresh exposes a
        # candidate string (especially when many nested directories are fed
        # concurrently). In that case the first observable candidate is the
        # completed result set, so conservatively use the completion timestamp.
        if first is None and complete is not None:
            first = complete
        if first is None or complete is None:
            raise RuntimeError(
                f"{binary} did not reach full-count marker for {count} files "
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

def early_exit(binary, root):
    """Ensure quitting while a large walk is streaming does not hang."""
    trace_path = os.path.join(tempfile.gettempdir(), f"cfzf-early-exit-{os.getpid()}.trace")
    pid, master = pty.fork()
    if pid == 0:
        os.chdir(root)
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env.pop("FZF_DEFAULT_COMMAND", None)
        env.pop("FZF_DEFAULT_OPTS", None)
        command = ["strace", "-f", "-tt", "-o", trace_path,
                   binary, "--walker-root=.", "--no-color"]
        os.execvpe(command[0], command, env)
    fcntl = __import__("fcntl")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 100, 30, 0, 0))
    captured = bytearray()
    started = time.perf_counter_ns()
    reaped = False
    try:
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline and MARKER not in captured:
            ready, _, _ = select.select([master], [], [], 0.01)
            if not ready:
                continue
            try:
                chunk = os.read(master, 65536)
            except OSError as exc:
                if exc.errno == errno.EIO:
                    break
                raise
            if not chunk:
                break
            captured.extend(chunk)
            if b"\\x1b[6n" in chunk:
                try:
                    os.write(master, TERM_REPLY)
                except OSError:
                    pass
        if MARKER not in captured:
            raise RuntimeError(f"{binary} never displayed a generated file for early-exit test")
        # Escape is a terminal byte rather than an ISIG-generated signal, so
        # the strace wrapper cannot intercept it instead of the child.
        os.write(master, b"\\x1b")
        deadline = time.monotonic() + 2.0
        status = None
        while time.monotonic() < deadline:
            try:
                waited, child_status = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                status = 0
                reaped = True
                break
            if waited == pid:
                status = child_status
                reaped = True
                break
            time.sleep(0.02)
        if status is None:
            try:
                with open(trace_path, encoding="utf-8", errors="replace") as trace_file:
                    tail = deque(trace_file, maxlen=80)
                print("EARLY_EXIT_TRACE_BEGIN")
                print("".join(tail), end="")
                print("EARLY_EXIT_TRACE_END")
            except OSError:
                pass
            raise RuntimeError(f"{binary} hung after Escape during walker streaming")
        elapsed = (time.perf_counter_ns() - started) / 1_000_000
        print(f"early_exit binary={os.path.realpath(binary)} "
              f"after_first_path_ms={elapsed:.3f} exited=true")
    finally:
        try:
            os.close(master)
        except OSError:
            pass
        if not reaped:
            try:
                os.killpg(pid, signal.SIGKILL)
            except ProcessLookupError:
                pass
            try:
                os.waitpid(pid, 0)
            except ChildProcessError:
                pass
        try:
            os.unlink(trace_path)
        except OSError:
            pass


def summarize(samples):
    med = statistics.median(samples)
    mad = statistics.median(abs(x - med) for x in samples)
    return med, mad

print("benchmark=cfzf-vs-pinned-fzf-walker-v1")
print(f"reference={os.path.realpath(REF)} implementation={os.path.realpath(IMPL)}")
print(f"repeats={REPEATS} timeout_seconds={TIMEOUT} sizes={','.join(map(str,SIZES))} shapes=flat,nested,deep")
workloads = [(count, shape) for shape in ("flat", "nested") for count in SIZES]
workloads.append((1_000, "deep"))
with tempfile.TemporaryDirectory(prefix="cfzf-walker-bench-") as work:
    for count, shape in workloads:
        root = os.path.join(work, f"tree-{shape}-{count}")
        os.mkdir(root)
        make_tree(root, count, shape)
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
        print(f"files={count} shape={shape} "
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
            print(f"raw files={count} shape={shape} metric={key} samples=" +
                  ",".join(f"{v:.3f}" for v in values))
        if (shape == "flat" and count == 100_000 and
                os.environ.get("WALK_BENCH_EARLY_EXIT") == "1"):
            early_exit(IMPL, root)
        if count == 1_000 and shape == "flat":
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
