#!/usr/bin/env python3
"""Correctness-gated serial/parallel cfzf comparison against pinned fzf."""
import os
import random
import statistics
import subprocess
import time
from pathlib import Path

root = Path(__file__).resolve().parents[1]
ref = os.environ.get("REF", "/tmp/opencode/fzf-ref")
impl = os.environ.get("IMPL", str(root / "cfzf"))
work = Path(os.environ.get("WORK", "/tmp/cfzf-thread-bench"))
repeats = int(os.environ.get("THREAD_BENCH_REPEATS", "5"))
if repeats < 3:
    raise SystemExit("THREAD_BENCH_REPEATS must be at least 3")
work.mkdir(parents=True, exist_ok=True)
corpus = work / "corpus-100000.txt"
rng = random.Random(73)
words = ("alpha", "alphabet", "beta", "gamma", "src", "lib", "include",
         "test", "path", "file", "name", "delta", "omega")
with corpus.open("w", encoding="utf-8") as out:
    for i in range(100_000):
        out.write("/".join((rng.choice(("src", "lib", "test")),
                            "-".join(rng.choice(words) for _ in range(4)),
                            f"{i:06d}.c")) + "\n")

cases = (
    ("ranked-alpha", ["--filter=alpha"]),
    ("ranked-src", ["--filter=src"]),
    ("ranked-length-tiebreak", ["--filter=alpha", "--tiebreak=length"]),
)

def invoke(binary, args, threads=None, capture=True):
    env = os.environ.copy()
    if threads is not None:
        env["FZF_NUM_THREADS"] = str(threads)
    with corpus.open("rb") as inp:
        return subprocess.run(
            [binary, *args], stdin=inp,
            stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
            stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
            env=env, check=False)

def timed(binary, args, threads=None):
    start = time.perf_counter_ns()
    p = invoke(binary, args, threads, capture=False)
    elapsed = (time.perf_counter_ns() - start) / 1_000_000
    if p.returncode not in (0, 1):
        raise RuntimeError(f"{binary} returned {p.returncode} for {args}")
    return elapsed

def median_mad(values):
    med = statistics.median(values)
    return med, statistics.median(abs(x - med) for x in values)

print("benchmark=cfzf-ranked-threading-v1")
print(f"corpus_lines=100000 bytes={corpus.stat().st_size} seed=73")
print(f"reference={ref}")
print(f"implementation={impl}")
print(f"repeats={repeats}")
print("metric=median_ms; dispersion=MAD_ms; speedup=reference_median/candidate_median")
for name, args in cases:
    baseline = invoke(ref, args)
    serial = invoke(impl, args, threads=1)
    parallel = invoke(impl, args, threads=4)
    for label, candidate in (("serial", serial), ("parallel", parallel)):
        if (baseline.returncode, baseline.stdout, baseline.stderr) != (
                candidate.returncode, candidate.stdout, candidate.stderr):
            raise SystemExit(f"CORRECTNESS FAILURE case={name} mode={label}")
    samples = {"reference": [], "serial": [], "parallel": []}
    # Warm every mode once before collecting paired samples.
    timed(ref, args)
    timed(impl, args, 1)
    timed(impl, args, 4)
    for i in range(repeats):
        order = [("reference", ref, None), ("serial", impl, 1), ("parallel", impl, 4)]
        if i % 2:
            order.reverse()
        for label, binary, threads in order:
            samples[label].append(timed(binary, args, threads))
    med = {}
    for label, values in samples.items():
        med[label] = median_mad(values)
    rm = med["reference"][0]
    sm = med["serial"][0]
    pm = med["parallel"][0]
    print(f"case={name} ref_median_ms={rm:.3f} ref_MAD_ms={med['reference'][1]:.3f} "
          f"serial_median_ms={sm:.3f} serial_MAD_ms={med['serial'][1]:.3f} "
          f"parallel_median_ms={pm:.3f} parallel_MAD_ms={med['parallel'][1]:.3f} "
          f"parallel_vs_serial={sm/pm:.3f} parallel_vs_fzf={rm/pm:.3f}")
