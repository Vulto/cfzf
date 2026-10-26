#!/usr/bin/env python3
"""Benchmark cfzf ranked batch search against the pinned fzf reference.

Run after ./build.sh. Every implementation run is checked byte-for-byte against
fzf before its timing is included. The harness reports medians, not estimates
based on one noisy run.
"""
from __future__ import annotations

import os
import statistics
import subprocess
import sys
import tempfile
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
REF_SCRIPT = ROOT / "tests" / "ref.sh"
IMPL = Path(os.environ.get("IMPL", ROOT / "cfzf")).resolve()
REPEATS = int(os.environ.get("BENCH_REPEATS", "5"))
ITEMS = int(os.environ.get("BENCH_ITEMS", "100000"))
THREAD_COUNTS = (1, 2, 4, 8)


def invoke(command: list[str], env: dict[str, str], corpus: Path):
    start = time.perf_counter()
    with corpus.open("rb") as source:
        result = subprocess.run(command, stdin=source, stdout=subprocess.PIPE,
                                stderr=subprocess.PIPE, env=env, check=False)
    elapsed = time.perf_counter() - start
    return result, elapsed


def main() -> int:
    if REPEATS < 1 or ITEMS < 10000:
        print("BENCH_REPEATS must be positive and BENCH_ITEMS must be >= 10000",
              file=sys.stderr)
        return 2
    if not IMPL.is_file() or not os.access(IMPL, os.X_OK):
        print(f"implementation binary not found: {IMPL}", file=sys.stderr)
        return 2

    reference = subprocess.run([str(REF_SCRIPT)], stdout=subprocess.PIPE,
                               stderr=subprocess.PIPE, text=True, check=False)
    if reference.returncode:
        print(reference.stderr, file=sys.stderr)
        return reference.returncode
    ref_bin = reference.stdout.strip()
    if not ref_bin:
        print("reference script returned an empty executable path", file=sys.stderr)
        return 2

    with tempfile.TemporaryDirectory(prefix="cfzf-bench-") as tmp:
        corpus = Path(tmp) / "items.txt"
        with corpus.open("w", encoding="utf-8", newline="\n") as out:
            for i in range(ITEMS):
                directory = "src" if i % 7 == 0 else ("lib" if i % 3 == 0 else "test")
                token = "alpha" if i % 5 == 0 else ("alphabet" if i % 2 == 0 else "beta")
                suffix = "alpha" if i % 11 == 0 else "omega"
                out.write(f"{directory}/{token}-{i:06d}-{suffix}.txt\n")

        cases = (
            ("score", ["--filter=alpha"]),
            ("score-length", ["--filter=alpha", "--tiebreak=length"]),
            ("score-v1", ["--algo=v1", "--filter=alpha"]),
        )
        host_env = os.environ.copy()
        results: list[tuple[str, int, float]] = []
        for case_name, args in cases:
            ref_result, ref_elapsed = invoke([ref_bin, *args], host_env, corpus)
            if ref_result.returncode not in (0, 1):
                print(f"reference failed ({case_name}): {ref_result.stderr.decode(errors='replace')}",
                      file=sys.stderr)
                return ref_result.returncode
            if ref_result.stderr:
                print(f"reference wrote stderr for {case_name}: "
                      f"{ref_result.stderr.decode(errors='replace')}", file=sys.stderr)
                return 1
            ref_samples = [ref_elapsed]
            for _ in range(REPEATS - 1):
                again, elapsed = invoke([ref_bin, *args], host_env, corpus)
                if (again.returncode, again.stdout, again.stderr) != (
                        ref_result.returncode, ref_result.stdout, ref_result.stderr):
                    print(f"reference was nondeterministic ({case_name})", file=sys.stderr)
                    return 1
                ref_samples.append(elapsed)
            ref_median = statistics.median(ref_samples)
            print(f"{case_name:12s} fzf median={ref_median * 1000:8.2f} ms")

            for threads in THREAD_COUNTS:
                env = host_env.copy()
                env.pop("FZF_NUM_THREADS", None)
                command = [str(IMPL), *args, f"--threads={threads}"]
                # One untimed warm-up reduces first-run noise.
                warm, _ = invoke(command, env, corpus)
                if (warm.returncode, warm.stdout, warm.stderr) != (
                        ref_result.returncode, ref_result.stdout, ref_result.stderr):
                    print(f"FAIL {case_name} threads={threads}: output differs from fzf",
                          file=sys.stderr)
                    return 1
                samples = []
                for _ in range(REPEATS):
                    actual, elapsed = invoke(command, env, corpus)
                    if (actual.returncode, actual.stdout, actual.stderr) != (
                            ref_result.returncode, ref_result.stdout, ref_result.stderr):
                        print(f"FAIL {case_name} threads={threads}: output differs from fzf",
                              file=sys.stderr)
                        return 1
                    samples.append(elapsed)
                median = statistics.median(samples)
                speedup = ref_median / median if median else float("inf")
                print(f"{'cfzf':12s} threads={threads} median={median * 1000:8.2f} ms "
                      f"vs fzf={speedup:5.2f}x")
                results.append((case_name, threads, speedup))

        if results:
            best = max(results, key=lambda row: row[2])
            print(f"best measured result: {best[0]}, {best[1]} threads, {best[2]:.2f}x fzf")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
