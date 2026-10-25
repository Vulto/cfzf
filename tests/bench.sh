#!/usr/bin/env bash
# Reproducible batch-performance comparison against the pinned fzf reference.
# Correctness is checked before any timing is reported.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMPL=${IMPL:-$ROOT/cfzf}
REF=${REF:-$("$ROOT/tests/ref.sh")}
WORK=${WORK:-/tmp/opencode/bench}
REPEATS=${BENCH_REPEATS:-5}
mkdir -p "$WORK"

python3 - "$REF" "$IMPL" "$WORK" "$REPEATS" <<'PY'
import os, random, statistics, subprocess, sys, time

ref, impl, work, repeats = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
if repeats < 3:
    raise SystemExit("BENCH_REPEATS must be at least 3")

print("benchmark=cfzf-vs-pinned-fzf-batch-v1")
for name, binary in (("reference", ref), ("cfzf", impl)):
    version = subprocess.run([binary, "--version"], text=True, stdout=subprocess.PIPE,
                             stderr=subprocess.STDOUT, check=False).stdout.strip()
    print(f"{name}_binary={os.path.realpath(binary)}")
    print(f"{name}_version={version}")

rng = random.Random(7)
words = ["alpha", "beta", "gamma", "delta", "epsilon", "zeta", "eta",
         "theta", "path", "to", "file", "name", "src", "lib", "include",
         "test", "case"]
sizes = (10_000, 100_000, 200_000)
queries = (("filter=alpha", ["--filter=alpha"]),
           ("filter=a", ["--filter=a"]),
           ("filter=src", ["--filter=src"]))
corpora = {}
for size in sizes:
    path = os.path.join(work, f"corpus-{size}.txt")
    with open(path, "w", encoding="utf-8") as out:
        for i in range(size):
            n = rng.randint(2, 6)
            out.write("-".join(rng.choice(words) for _ in range(n)) + "\n")
    corpora[size] = path
    print(f"corpus_lines={size} bytes={os.path.getsize(path)} seed=7")

def invoke(binary, args, corpus, capture):
    with open(corpus, "rb") as inp:
        return subprocess.run([binary, *args], stdin=inp,
                              stdout=subprocess.PIPE if capture else subprocess.DEVNULL,
                              stderr=subprocess.PIPE if capture else subprocess.DEVNULL,
                              check=False)

def timed(binary, args, corpus):
    start = time.perf_counter_ns()
    p = invoke(binary, args, corpus, False)
    elapsed = time.perf_counter_ns() - start
    if p.returncode not in (0, 1):
        raise RuntimeError(f"{binary} {args} failed with status {p.returncode}")
    return elapsed / 1_000_000

print("metric=median_ms; dispersion=MAD_ms; speedup=reference_median/cfzf_median (>1 means cfzf faster)")
for size, corpus in corpora.items():
    for label, args in queries:
        a, b = invoke(ref, args, corpus, True), invoke(impl, args, corpus, True)
        if (a.returncode, a.stdout) != (b.returncode, b.stdout):
            raise SystemExit(f"CORRECTNESS FAILURE: {label}, lines={size}, "
                             f"ref_status={a.returncode}, cfzf_status={b.returncode}, "
                             f"stdout_equal={a.stdout == b.stdout}")
        # Warm both binaries once, then alternate execution order to reduce
        # order and cache bias. Use medians, not the fastest observed sample.
        timed(ref, args, corpus)
        timed(impl, args, corpus)
        rt, ct = [], []
        for i in range(repeats):
            order = ((ref, rt), (impl, ct)) if i % 2 == 0 else ((impl, ct), (ref, rt))
            for binary, samples in order:
                samples.append(timed(binary, args, corpus))
        rm, cm = statistics.median(rt), statistics.median(ct)
        rmad = statistics.median(abs(x - rm) for x in rt)
        cmad = statistics.median(abs(x - cm) for x in ct)
        print(f"case={label} lines={size} ref_median_ms={rm:.3f} "
              f"ref_MAD_ms={rmad:.3f} cfzf_median_ms={cm:.3f} "
              f"cfzf_MAD_ms={cmad:.3f} speedup={rm/cm:.3f} "
              f"ref_samples_ms={','.join(f'{x:.3f}' for x in rt)} "
              f"cfzf_samples_ms={','.join(f'{x:.3f}' for x in ct)}")

# Startup latency is a separate metric, measured without input.
def startup(binary):
    start = time.perf_counter_ns()
    p = subprocess.run([binary, "--version"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=False)
    elapsed = time.perf_counter_ns() - start
    if p.returncode != 0:
        raise RuntimeError(f"{binary} --version failed: {p.returncode}")
    return elapsed / 1_000_000

rt, ct = [], []
startup(ref); startup(impl)
for i in range(repeats + 2):
    order = ((ref, rt), (impl, ct)) if i % 2 == 0 else ((impl, ct), (ref, rt))
    for binary, samples in order:
        samples.append(startup(binary))
rm, cm = statistics.median(rt), statistics.median(ct)
print(f"case=startup ref_median_ms={rm:.3f} cfzf_median_ms={cm:.3f} "
      f"speedup={rm/cm:.3f}")
PY
