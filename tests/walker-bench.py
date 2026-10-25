#!/usr/bin/env python3
"""Paired end-to-end comparison of pinned fzf, prior cfzf baseline, and current cfzf.

All binaries walk the same generated tree. Repetition order rotates across
binaries, and paired speedup ratios are reported alongside medians/MAD.
This measures interactive CLI behavior, not isolated syscall latency.
"""
import errno
import signal
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

if len(sys.argv) != 4:
    raise SystemExit("usage: walker-bench.py REF BASELINE CURRENT_IMPL")
REF, BASELINE, IMPL = sys.argv[1:4]
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
    pid, master = pty.fork()
    if pid == 0:
        os.chdir(root)
        env = os.environ.copy()
        env["TERM"] = "xterm-256color"
        env.pop("FZF_DEFAULT_COMMAND", None)
        env.pop("FZF_DEFAULT_OPTS", None)
        os.execvpe(binary, [binary, "--walker-root=.", "--no-color"], env)
    fcntl = __import__("fcntl")
    fcntl.ioctl(master, termios.TIOCSWINSZ, struct.pack("HHHH", 100, 30, 0, 0))
    captured = bytearray()
    started = time.perf_counter_ns()
    first_seen_ns = None
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
            if first_seen_ns is None and MARKER in captured:
                first_seen_ns = time.perf_counter_ns()
            if b"\x1b[6n" in chunk:
                try:
                    os.write(master, TERM_REPLY)
                except OSError:
                    pass
        if MARKER not in captured:
            raise RuntimeError(f"{binary} never displayed a generated file for early-exit test")
        # Let initial terminal setup finish while continuing to answer terminal
        # queries; sending a key during the first redraw can be consumed as a
        # terminal-query response instead of a user action.
        settle_deadline = time.monotonic() + 0.05
        while time.monotonic() < settle_deadline:
            ready, _, _ = select.select([master], [], [], 0.005)
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
            if b"\x1b[6n" in chunk:
                try:
                    os.write(master, TERM_REPLY)
                except OSError:
                    pass
        # Enter is an unambiguous exit action, unlike the Escape sequence prefix.
        key_sent_ns = time.perf_counter_ns()
        os.write(master, b"\r")
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
                task_dir = f"/proc/{pid}/task"
                for tid in os.listdir(task_dir):
                    try:
                        with open(f"{task_dir}/{tid}/wchan", encoding="utf-8") as wchan:
                            where = wchan.read().strip()
                        print(f"early_exit_debug tid={tid} wchan={where}")
                    except OSError:
                        pass
                try:
                    with open(f"/proc/{pid}/status", encoding="utf-8") as status_file:
                        for line in status_file:
                            if line.startswith(("State:", "Threads:", "VmRSS:", "VmHWM:")):
                                print("early_exit_debug " + line.strip())
                except OSError:
                    pass
            except OSError:
                pass
            raise RuntimeError(f"{binary} hung after Enter during walker streaming")
        finished_ns = time.perf_counter_ns()
        first_ms = ((first_seen_ns or finished_ns) - started) / 1_000_000
        exit_latency_ms = (finished_ns - key_sent_ns) / 1_000_000
        total_ms = (finished_ns - started) / 1_000_000
        print(f"early_exit binary={os.path.realpath(binary)} "
              f"time_to_first_path_ms={first_ms:.3f} "
              f"exit_latency_ms={exit_latency_ms:.3f} total_to_exit_ms={total_ms:.3f} "
              f"exited=true")
        if exit_latency_ms > 100.0:
            raise RuntimeError(
                f"{binary} exit latency {exit_latency_ms:.3f} ms exceeds 100 ms")
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


def summarize(samples):
    med = statistics.median(samples)
    mad = statistics.median(abs(x - med) for x in samples)
    return med, mad

print("benchmark=paired-cfzf-vs-pinned-fzf-walker-v2")
binaries = (("ref", REF), ("baseline", BASELINE), ("cfzf", IMPL))
print(" ".join(f"{name}={os.path.realpath(path)}" for name, path in binaries))
print(f"repeats={REPEATS} timeout_seconds={TIMEOUT} sizes={','.join(map(str,SIZES))} shapes=flat,nested,deep")
workloads = [(count, shape) for shape in ("flat", "nested") for count in SIZES]
workloads.append((1_000, "deep"))
with tempfile.TemporaryDirectory(prefix="cfzf-walker-bench-") as work:
    for count, shape in workloads:
        root = os.path.join(work, f"tree-{shape}-{count}")
        os.mkdir(root)
        make_tree(root, count, shape)
        samples = {
            f"{prefix}_{metric}": []
            for prefix, _ in binaries
            for metric in ("first", "full", "rss_kb")
        }
        # Warm all binaries on the same generated tree.
        for _, binary in binaries:
            measure(binary, root, count)
        # Rotate order so each implementation runs first/middle/last.
        for i in range(REPEATS):
            offset = i % len(binaries)
            order = binaries[offset:] + binaries[:offset]
            for prefix, binary in order:
                first, full, rss_kb = measure(binary, root, count)
                samples[f"{prefix}_first"].append(first)
                samples[f"{prefix}_full"].append(full)
                samples[f"{prefix}_rss_kb"].append(rss_kb)

        medians = {}
        for prefix, _ in binaries:
            for metric in ("first", "full", "rss_kb"):
                medians[f"{prefix}_{metric}"] = summarize(samples[f"{prefix}_{metric}"])
        ref_full = samples["ref_full"]
        baseline_full = samples["baseline_full"]
        cfzf_full = samples["cfzf_full"]
        paired_ref_speedups = [ref / cfzf for ref, cfzf in zip(ref_full, cfzf_full)]
        paired_baseline_speedups = [base / cfzf for base, cfzf in zip(baseline_full, cfzf_full)]
        paired_ref_med, paired_ref_mad = summarize(paired_ref_speedups)
        paired_buffer_med, paired_buffer_mad = summarize(paired_baseline_speedups)

        row = [f"files={count}", f"shape={shape}"]
        for prefix, _ in binaries:
            first, first_mad = medians[f"{prefix}_first"]
            full, full_mad = medians[f"{prefix}_full"]
            rss, rss_mad = medians[f"{prefix}_rss_kb"]
            row.extend([
                f"{prefix}_first_ms={first:.3f}",
                f"{prefix}_first_MAD_ms={first_mad:.3f}",
                f"{prefix}_full_ms={full:.3f}",
                f"{prefix}_full_MAD_ms={full_mad:.3f}",
                f"{prefix}_paths_per_s={count*1000/full:.0f}",
                f"{prefix}_peak_rss_kb={rss:.0f}",
                f"{prefix}_peak_rss_MAD_kb={rss_mad:.0f}"
            ])
        row.extend([
            f"baseline_speedup_vs_ref={medians['ref_full'][0]/medians['baseline_full'][0]:.3f}",
            f"cfzf_speedup_vs_ref={medians['ref_full'][0]/medians['cfzf_full'][0]:.3f}",
            f"cfzf_speedup_vs_baseline={medians['baseline_full'][0]/medians['cfzf_full'][0]:.3f}",
            f"paired_cfzf_vs_ref_speedup={paired_ref_med:.3f}",
            f"paired_cfzf_vs_ref_MAD={paired_ref_mad:.3f}",
            f"paired_cfzf_vs_baseline_speedup={paired_buffer_med:.3f}",
            f"paired_cfzf_vs_baseline_MAD={paired_buffer_mad:.3f}"
        ])
        print(" ".join(row))
        for key, values in samples.items():
            print(f"raw files={count} shape={shape} metric={key} samples=" +
                  ",".join(f"{v:.3f}" for v in values))

        if (shape == "nested" and count == 100_000 and
                os.environ.get("WALK_BENCH_EARLY_EXIT") == "1"):
            early_exit(IMPL, root)

        if count == 1_000 and shape == "flat":
            # One syscall-count profile per binary. This is not timed evidence:
            # strace perturbs runtime, so only syscall counts are compared.
            wanted = {"getdents64", "newfstatat", "statx", "openat",
                      "read", "write", "writev", "close"}
            for prefix, binary in binaries:
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
