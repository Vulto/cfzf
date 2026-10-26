# cfzf

A C23 implementation of [fzf](https://github.com/junegunn/fzf), targeting
behavioral compatibility with the pinned upstream release recorded in
`tests/ref.sh`.

## Build

```sh
./build.sh
```

## Differential tests

Build the pinned fzf reference once through the test harness, then run the
compatibility suites:

```sh
python3 tests/status.py
bash tests/batch-diff.sh
bash tests/thread-diff.sh
bash tests/algo-diff.sh
bash tests/nth-diff.sh
bash tests/walker-diff.sh
bash tests/pty-diff.sh
bash tests/gen-ansi-probe.sh
bash tests/shell-integration.sh
bash tests/manpage.sh
```

The reference source checkout must be available at the path configured by
`FZF_SRC`, or provide a verified binary through `FZF_REF`.

## Ranked-search benchmark

After building `cfzf` and preparing the pinned reference, run:

```sh
python3 tests/bench-search.py
```

The benchmark generates a deterministic corpus, verifies byte-for-byte output,
stderr, and exit status against fzf for each tested thread count, then reports
median runtimes and speedups. Adjust the workload with `BENCH_ITEMS` (minimum
10,000), the repetition count with `BENCH_REPEATS`, and the implementation
binary with `IMPL`. The benchmark is opt-in and is not a substitute for the
differential regression suites.
