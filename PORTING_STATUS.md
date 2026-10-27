# PORTING_STATUS.md — fzf → C23 rewrite

Persistent status report for the behavioral rewrite of `fzf` in ISO C23.
See `PLAN.md` (authoritative) and `FEATURES.md` (per-feature inventory).

## Upstream reference

- Project: junegunn/fzf
- Version: **0.74.3**
- Commit: **15f64c492a08f0840b81540c7d1de35737448086**
- Source of record: `/tmp/opencode/fzf-src` (checked out at that commit;
  `git describe` = `v0.74.3`)
- Reference binary: `tests/ref.sh` builds a fresh executable from this
  exact commit in an isolated Git worktree. It never falls back to a system
  `fzf` or trusts a pre-existing binary. `FZF_REF` is an explicit override
  and must be supplied only after independently verifying its provenance.
  A source-built binary may report `0.74 (devel)`; the source commit is the
  compatibility identity.

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

The authoritative execution status is `TASKS.md`. Milestone 1
(trustworthy baseline) remains **in progress** until the complete existing
differential suite has passed from a clean local build and the evidence and
inventory documents have been reconciled. GitHub Actions is disabled by user
instruction; do not use CI. Individual implementation or historical test
results do not complete a milestone.

## Previously reported feature coverage (verification scope varies)

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

## Historical test results (not all rerun by current CI)

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

These counts were copied from prior reports and are not a substitute for fresh
local baseline evidence. Record exact commit IDs, commands, results, and scope
in `BASELINE_EVIDENCE.md`; do not use GitHub Actions. The pty harness compares
logical screen states through `tests/screen.py`.

## Known mismatches / open gaps

- The current `FEATURES.md` inventory marks 34 CLI options unstarted and
  20 implemented without differential verification. Its 77 `verified` labels
  are inherited inventory claims; the current CI suite covers only the cases
  recorded in `BASELINE_EVIDENCE.md`. Some unsupported options are rejected,
  but exact CLI parity has not been certified.
- The latest source audit finds 127 action rows: 54 marked implemented, 56
  implemented but unverified, and 17 unstarted. IDs 060, 065, 100, 122, and 123
  are absent from the inventory and need reconciliation against upstream. No
  action is marked differentially verified in `FEATURES.md`. All 13 events
  remain marked unstarted; infrastructure presence is not equivalent to event
  parity.
- Interactive `--select-1` / `--exit-0` now auto-exit from the initial
  result set, deferring the decision until asynchronous input is complete.
  PTY regression cases were added; differential execution is still pending.
- Preview `wrap-word` and `toggle-preview-wrap-word` now distinguish
  word-boundary wrapping from character wrapping. The new PTY cases have not
  yet been executed against the pinned reference.
- `select-down` and `deselect-up` have interactive dispatcher
  implementations and PTY differential cases. Those cases have not been run.
- `hide-header`, `show-header`, and `toggle-header` preserve and
  restore the configured header rather than inventing replacement text.
  Differential cases are present but not executed.
- Preview visibility actions now hide/show the preview pane without
  destroying its command runner; PTY differential cases are present but not
  executed.
- `backward-kill-subword` and `forward-subword` now move/delete by UTF-8
  codepoint boundaries, punctuation separators, and camel-case transitions.
  Differential execution remains pending.
- `unbind(key[,key...])` and `unbind(*)` now remove active key bindings;
  a PTY differential case was added but has not been run.
- `put(text)` inserts text at the query cursor and re-runs search;
  `reload-sync` is routed to the existing synchronous reload path. The
  `put` PTY regression case is not yet executed.
- Runtime `unbind` now retains removed key bindings, and `rebind`
  restores them, including wildcard forms. Differential cases are present but
  not executed.
- `select` now selects the current item within multi-select limits;
  `toggle-in` / `toggle-out` dispatch according to layout direction. PTY
  cases are present but unexecuted.
- GitHub Actions CI and performance workflow files have been removed; this
  project now uses local-only verification by instruction.
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

### New interactive action work

- `toggle-multi-line` now switches embedded-newline items between multiline and single-line display. Added a NUL-delimited PTY differential case; not executed.

- `show-input` and `toggle-input` now control runtime prompt/query visibility. PTY differential cases were added but not executed.

- `transform(command)` now captures stdout from the configured shell and parses the returned text as an action chain, with a PTY regression case for `change-query(cherry)`. The regression has not been run.

- Preview scroll actions now support top, bottom, line, page, and half-page movement for unwrapped preview output, with a matching scrollbar thumb and line indicator. PTY regression cases have been added but are not executed. Wrapped preview scrolling remains incomplete.

## Dependency blockers

- GitHub Actions workflows are intentionally disabled by user instruction; no CI runs are to be triggered. No `gh`, `shellcheck`, or `sk`.
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

- Active development branch: `main`.
- Changes are committed directly to `main`; do not use a feature branch or
  open a pull request for this work.
- Build entry point: `./build.sh`; it produces the executable at
  `FZF_BUILD_OUTPUT` (default `cfzf`) and installs it under `PREFIX`.
- No legacy `mmenu` code remains.

## Next work

Follow `TASKS.md` in milestone order, one task at a time. Do not begin a later
milestone until the current milestone's exit gate is backed by fresh test
evidence. Current evidence and its scope are recorded in `BASELINE_EVIDENCE.md`.

- `exclude` and `exclude-multi` now mark current/selected items excluded from subsequent searches, including unfiltered mode; PTY differential cases were added but not executed. Exclusions are held by item index for the session and are not persisted across reload replacement.

- `bg-transform` now launches a shell command asynchronously, captures stdout without blocking key input, and applies the returned action chain at EOF. A new background transform replaces the previous job; `bg-cancel` terminates and clears the active job. PTY differential cases were added but not run.

- `become(command)` now expands placeholders, exits the UI through normal terminal cleanup, then replaces the process with the configured shell command. If exec fails, it reports an error and returns status 2. A PTY differential case was added but not executed.

- Reconciled action inventory entries for subword deletion, forward-subword, put, rebind, reload-sync, select, show-header, show-preview, toggle-in, and toggle-out with the current dispatcher and regression-case names. The new reload-sync, select, and toggle-out PTY cases have not been executed.

- Implemented `change-nth(spec)` to parse and replace the active field ranges, rerun search, and retain the previous configuration on invalid specifications. A PTY differential case changes matching from field 1 to field 2; it has not been executed.

- Implemented seven runtime label/content actions: `change-border-label`, `change-footer`, `change-ghost`, `change-header-label`, `change-input-label`, `change-list-label`, and `change-preview-label`. PTY differential cases were added but not executed.
