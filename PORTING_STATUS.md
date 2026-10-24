# PORTING_STATUS.md — fzf → C23 rewrite

Persistent status report for the behavioral rewrite of `fzf` in ISO C23.
See `PLAN.md` (authoritative) and `FEATURES.md` (per-feature inventory).

## Upstream reference

- Project: junegunn/fzf
- Version: **0.74.3**
- Commit: **15f64c492a08f0840b81540c7d1de35737448086**
- Source of record: `/tmp/opencode/fzf-src` (checked out at that commit;
  `git describe` = `v0.74.3`)
- Reference binary: resolved by `tests/ref.sh`, which uses `$FZF_REF`, then
  `/usr/bin/fzf`, then a binary it builds from the pinned commit in
  `$FZF_SRC` (default `/tmp/opencode/fzf-src`). A reference built from the
  source reports `0.74 (devel)` because no version tag is injected at build
  time; it is otherwise the same code as the tagged release.

## Project layout

Pure C23 fzf rewrite. All legacy `mmenu` code was removed (old `main.c`,
`mmenu.h`, `mmenu.1`, the ncurses build, the old docs and the old test tree).

- `src/main.c` — CLI entry point, option parsing, batch (`--filter`) mode.
- `src/filter.c` — batch search driver.
- `src/pattern.c` — extended query parser and matcher.
- `src/algo.c` — fuzzy/exact/prefix/suffix matchers and scoring.
- `src/result.c` — sort criteria, tie-breaks, ranking.
- `src/tokenizer.c` — `--nth`/`--with-nth` ranges, string and regex delimiters.
- `src/item.c`, `src/util.c` — item model, UTF-8 primitives.
- `src/ansi.c`, `src/tui.h` — ANSI escape/color parsing.
- `src/terminal.c` — raw-termios renderer (no ncurses).
- `src/keys.c`, `src/event.c` — key decoding and event model.
- `src/ui.c` — interactive loop.
- `src/walker.c` — default file listing (fzf started without piped input).
- `src/preview.c` — preview window: command execution and output collection.
- `fzf.1` — man page; `build.sh` installs it to `$PREFIX/share/man/man1/fzf.1`.
- `tests/` — differential harnesses (batch, pty, ANSI) and fixtures.

## Build

- `build.sh` — no-arg build + install; probes `-std=c23` then `-std=c2x`;
  builds `src/*.c` into `fzf`; installs to `$PREFIX/bin/fzf`.
- Manual strict build:
  `cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O2 -Isrc -o fzf src/*.c -lm`
- Toolchains present: GCC 16.2.1, Clang 22.1.8 (both accept `-std=c23`).

## Current milestone

M1 (matching core + batch parity) — **complete and verified**.
M2 (interactive TUI) — core loop, layout and key handling **implemented and
verified**; the remaining `--bind`/preview/border machinery is not started.
M3+ (options, actions, integrations) — **not started**.

## Completed feature groups (verified)

| Area | Module | Evidence |
|------|--------|----------|
| UTF-8 / char primitives | `src/util.{c,h}` | unit + differential |
| Fuzzy match + scoring V1/V2 | `src/algo.{c,h}` | 27/27 algo-diff cases |
| Extended query parser | `src/pattern.{c,h}` | 50+ query shapes |
| Latin normalization | `src/algo.c`, `src/pattern.c` | batch-diff |
| nth / with-nth tokenizer | `src/tokenizer.{c,h}` | nth 55/55 |
| String + regex delimiters | `src/tokenizer.{c,h}` | batch-diff |
| Ranking / tiebreak | `src/result.{c,h}` | 51/51 batch-diff cases |
| Batch `--filter` CLI | `src/main.c` | 51/51 batch-diff cases |
| ANSI escape/color parsing | `src/ansi.{c,h}` | 21/21 + 15500/15500 + 15/15 |
| Terminal renderer (termios) | `src/terminal.{c,h}` | pty screen diff |
| Key decoding | `src/keys.c`, `src/event.c` | pty screen diff |
| Interactive loop & layout | `src/ui.c` | 35/35 pty-diff cases |
| `--bind` actions | `src/bind.{c,h}` | 35/35 pty-diff cases |
| `--color` theme | `src/color.{c,h}` | 35/35 pty-diff cases (SGR palette compared) |
| Border shapes (`--border`) | `src/terminal.c`, `src/ui.c` | 9/9 pty-diff cases |
| Preview window (`--preview`) | `src/preview.{c,h}`, `src/ui.c` | 13/13 pty-diff cases |
| Default file listing (walker) | `src/walker.c` | 8/8 walker-diff cases |

## Test results (latest run)

| Suite | Command | Result |
|-------|---------|--------|
| Batch differential | `bash tests/batch-diff.sh` | **55/55** |
| Interactive (pty) differential | `bash tests/pty-diff.sh` | **126/126** (text **and** colour) |
| Algo / ordering differential | `bash tests/algo-diff.sh` | **27/27** |
| nth differential | `bash tests/nth-diff.sh` | **55/55** |
| ANSI differential | `bash tests/gen-ansi-probe.sh` | **21/21**, **15500/15500**, **15/15** |
| Shell integration (bash) | `bash tests/shell-integration.sh` | **7/7** |
| File listing (walker) | `bash tests/walker-diff.sh` | **8/8** |
| Man page | `bash tests/manpage.sh` | **2/2** |
| `--pointer`, `--marker`, `--ghost`, `--history`, `--with-shell`, `--man` | `src/ui.c`, `src/history.c` | 54/54 pty-diff cases |
| Allocation failures | `bash tests/allocfail.sh` | **360 injections, 0 crashes** |
| Sanitizers (ASan+UBSan) | `cc -fsanitize=address,undefined` over 21 option/input combinations | **0 findings** |
| Parser fuzzing | 2400 random `--bind`/`--color`/`--filter`/`--expect`/`--nth`/`--delimiter`/`--tiebreak`/`--with-nth` inputs under ASan+LSan | **0 findings** |
| Static analysis | `gcc -fanalyzer -fsyntax-only` over `src/*.c` | **0 findings** |
| Strict build | `gcc` and `clang`, `-std=c23 -Wall -Wextra -Wpedantic -Werror` | **clean** |

The pty harness compares *logical screen states* (a small ANSI screen
emulator in `tests/screen.py`), not raw bytes, so that fzf's incremental
repaint and the C23 full repaint are compared on equal terms.

## Known mismatches / open gaps

- **~38 of 131 CLI options are not implemented** (`--listen`, `--tmux`/`--popup`,
  `--with-shell`, `--threads`, `--bench`, `--man`, layout, scrolling,
  `--bind`, walker, shell integration). Several options are implemented but
  not yet covered by differential tests. `--highlight-line`, `--header-first`,
  `--info-command` are implemented and verified through differential tests.
  Unknown options are rejected with `unknown option: X` and exit 2, matching
  fzf's error style, rather than being silently ignored.
- `--bind` is implemented (`src/bind.c`): key specifications, `+`-chained
  actions and 68 actions are honoured. Event binding infrastructure is
  implemented (start, load, change, result triggers), but no events are
  yet verified through differential tests. All 13 events in `FEATURES.md`
  remain `unstarted`.
- `--select-1` / `--exit-0` are parsed but not acted on in interactive mode.
- fzf shows a spinner in the info line while the input is still being read;
  the C23 port reads the input synchronously and never shows one, so the
  spinner colour is excluded from the palette comparison.
- fzf's file listing walks the tree concurrently, so the order of the entries
  is not deterministic in the reference either; the walker test compares the
  resulting set of paths.
- The upstream **bash** shell integration is vendored unchanged in `shell/`
  (`key-bindings.bash`, `completion.bash`, `common.sh`). zsh and fish
  integrations are out of scope and are not shipped.
  `tests/shell-integration.sh` verifies that the three scripts are present,
  that they pass `bash -n`, that the bash completion loads and defines its
  helpers, and it reports the options the scripts depend on that this
  implementation does not support yet (`--height`, `--min-height`, `--bind`,
  `--tmux`, `--preview`, `--pointer`, `--marker`, `--jump-labels`).
- Shell integration is limited to **bash**; zsh and fish are out of scope.
- The option, query and delimiter parsers have been fuzzed with 2400 random
  inputs under ASan/UBSan/LSan (clean).  The allocation-failure harness
  (`tests/allocfail.sh`) injects 500 failures — 360 over the batch paths
  with an `LD_PRELOAD` shim and 140 over the interactive paths with an
  in-binary hook — and found and fixed a double free in the V2 matcher and
  a dozen unchecked allocations; it now reports 0 crashes.  `valgrind` is not installed in this
  environment, so leak checking relies on
  AddressSanitizer/LeakSanitizer.
- The batch driver allocates per item, so a large input costs more
  allocations than fzf's slab allocator (see the benchmark below).
- The first paint shows `0/N` in the info line where fzf shows `N/N` until
  the items are read; only the final screen state is identical.
- The list area reserves fzf's right-hand scrollbar column and truncates
  long items with fzf's `··` ellipsis; both were verified against the
  reference at 10, 20, 80, 200 and 500 columns.
- Performance: the C23 implementation is 1.5×–1.7× slower than the Go
  reference on some 200k-line batch workloads and slightly faster on others
  (see below); startup is comparable.

## Dependency blockers

- No `gh` (cannot open PRs or query CI). No `shellcheck`. No `sk`.
- The system `fzf` package binary (`/usr/bin/fzf`) is no longer present in
  this environment, so the differential tests build the reference from the
  pinned source (`tests/ref.sh`). A copy of the C23 binary was also found
  installed at `/usr/local/bin/fzf` (root-owned, not removable from this
  account); it is **not** used as a reference.
- `valgrind` is not installed, so leak checking relies on
  AddressSanitizer/LeakSanitizer.
- Interactive rendering needs a pty; `tests/ptyrun.py` provides one.

## Benchmarks

`bash tests/bench.sh`, 200 000 generated lines, same machine, `-O2`:

| Workload | Reference (Go) | C23 | Ratio |
|----------|----------------|-----|-------|
| `--filter=alpha` | 77 ms | 89 ms | 1.16× |
| `--filter=a` | 225 ms | 130 ms | 0.58× |
| `--filter=src` | 78 ms | 71 ms | 0.91× |
| `--version` (startup) | 4 ms | 5 ms | 1.25× |

After the optimisations (a shared scratch buffer for the decoded runes, a
reusable matcher workspace, a single-pass `mchars_init()`, a radix sort for
the results and item reuse in the interactive mode) the C23 port is faster
than the Go reference on two of the three workloads and at most 16 % slower
on the third.

## Final report

`FINAL_REPORT.md` holds the consolidated report required by PLAN §12
(reference, architecture, feature statistics, test results, differential
compatibility, sanitizer results, performance, platforms, limitations,
commits).

## Repository state

- Branch: `fzf-c23-rewrite`, clean working tree. Development is local only:
  nothing is pushed and the branch has no upstream configured.
- Latest commit: `1701885 fix: free the compiled delimiter and keymap on exit`.
- Build: `sh ./build.sh` produces `fzf` and installs it to `$PREFIX/bin/fzf`.
- Source size: ~5 700 lines of C23 in `src/`, ~1 700 lines of test harness
  in `tests/`.
- No legacy `mmenu` code remains.

## Exact next action

1. Implement the options the vendored bash shell integration needs
   (`--height`, `--bind`, `--tmux`, `--preview`, `--pointer`, `--marker`,
   `--jump-labels`) so the key bindings and completions work end to end.
2. Extend the action system: `transform`, `reload`, `become`, `preview`,
   the `execute` placeholder set (`{+}`, `{f}`, `{r}`, `{q}`) and the
   `every()` / event bindings.
3. Add the preview window, borders, colors and `--layout`/`--height` handling
   to `src/terminal.c`/`src/ui.c`.
4. Add allocation-failure injection and a fuzzer for the option, query and
   delimiter parsers; run `-fanalyzer` over the tree.
5. Optimize the batch driver (slab-allocate items, cache decoded runes) to
   close the throughput gap.
6. Add allocation-failure injection and fuzzers for the option, query and
   delimiter parsers.
