# FINAL_REPORT.md — C23 rewrite of fzf

Status report required by `PLAN.md` §12. Everything below was measured on
the machine the work was done on; estimated numbers are marked as such.

## 1. Upstream reference

| Item | Value |
|---|---|
| Project | junegunn/fzf |
| Version | 0.74.3 |
| Commit | `15f64c492a08f0840b81540c7d1de35737448086` (`v0.74.3`) |
| Source of record | `/tmp/opencode/fzf-src`, checked out at that commit |
| Reference binary | resolved by `tests/ref.sh`: `$FZF_REF`, else `/usr/bin/fzf`, else built from the pinned commit into `/tmp/opencode/fzf-ref` |

The system `/usr/bin/fzf` is no longer installed in this environment, so the
harnesses build the reference from the pinned source. A copy of the C23
binary was found installed at `/usr/local/bin/fzf` (root-owned, not removable
from this account); it is **not** used as a reference.

## 2. Architecture

Pure ISO C23, no ncurses, no external dependencies beyond libc and POSIX
`<regex.h>`.

| Module | Responsibility |
|---|---|
| `src/main.c` | CLI parsing, batch (`--filter`) mode, interactive mode, file listing |
| `src/filter.c` | batch search driver |
| `src/pattern.c` | extended query grammar, per-term matching, normalization rules |
| `src/algo.c` | fuzzy V1/V2, exact/prefix/suffix/equal/boundary matchers, scoring |
| `src/result.c` | sort criteria, tie-breaks, ranking |
| `src/tokenizer.c` | `--nth`/`--with-nth` ranges, string and regex delimiters |
| `src/item.c`, `src/util.c` | item model, UTF-8 primitives, runewidth |
| `src/ansi.c` | ANSI escape/colour parsing (offsets and states) |
| `src/terminal.c` | raw-termios renderer: alt screen, SGR, cursor, border glyphs |
| `src/keys.c`, `src/event.c` | key decoding, event model, key names |
| `src/bind.c` | `--bind` parsing, action model, keymap |
| `src/color.c` | `--color` parsing, Dark256 default theme |
| `src/preview.c` | preview window: async command execution, output collection |
| `src/walker.c` | default file listing |
| `src/ui.c` | interactive loop and layout |
| `src/tui.h` | shared `Color`/`Attr` types |
| `fzf.1` | man page, installed by `build.sh` |

Size: ~6 300 lines of C in `src/`, ~1 900 lines of harness in `tests/`.

## 3. Feature inventory completion

`FEATURES.md` holds the per-feature matrix (131 options, 132 actions,
13 events).

| Group | Verified | Total |
|---|---|---|
| CLI options | 70 | 131 |
| `--bind` actions | 39 | 132 |
| Events | 0 | 13 |

"Verified" means implemented **and** covered by an automated differential
test against the reference. A feature is never marked verified because its
name appears in the code.

## 4. Test results

| Suite | Command | Result |
|---|---|---|
| Batch differential | `bash tests/batch-diff.sh` | **55/55** |
| Interactive (pty, logical screens + colours) | `bash tests/pty-diff.sh` | **126/126** |
| Ranking / tie-breaks | `bash tests/algo-diff.sh` | **27/27** |
| `--nth` / `--with-nth` | `bash tests/nth-diff.sh` | **55/55** |
| File listing (walker) | `bash tests/walker-diff.sh` | **8/8** |
| ANSI escapes and colours | `bash tests/gen-ansi-probe.sh` | **21/21**, **15500/15500** fuzz, **15/15** |
| Bash shell integration | `bash tests/shell-integration.sh` | **7/7** |
| Man page consistency | `bash tests/manpage.sh` | **2/2** |
| Allocation-failure injection | `bash tests/allocfail.sh` | **500 injections (360 batch + 140 interactive), 0 crashes** |
| Memory safety | ASan + UBSan over the option matrix and the interactive path | **0 findings** |
| Parser fuzzing | 2 400 random option/query/delimiter inputs under ASan/LSan | **0 findings** |
| Static analysis | `gcc -fanalyzer` over `src/*.c` | **0 findings** |
| Strict build | gcc and clang, `-std=c23 -Wall -Wextra -Wpedantic -Werror` | **clean** |

The interactive harness compares *logical screen states* (a small ANSI screen
emulator in `tests/screen.py`), so fzf's incremental repaint and the C23
full repaint are compared on equal terms. The `--color` cases additionally
compare the palette of SGR codes that both programs emit.

## 5. Differential compatibility

Verified against the reference, byte for byte or screen for screen:

- matching: fuzzy V1/V2, exact/prefix/suffix/equal/exact-boundary, the
  extended query grammar (`'…`, `^…`, `…$`, `!…`, `|`, `\'`), smart case,
  **Latin normalization** (enabled only when the pattern itself is unchanged
  by it), `--nth`/`--with-nth`, string **and regex** delimiters, `--scheme`,
  `--tiebreak`;
- interactive: fzf 0.74's bottom-up layout, pointer column, `N/M` info line,
  `--pointer`/`--marker`/`--ghost`, the query history file and its
  `prev-history`/`next-history` actions, alt-modified key decoding,
  `··` ellipsis truncation, the right-hand scrollbar (fzf's `getScrollbar`
  geometry), all ten `--border` shapes, `--color` palette, `--reverse`,
  `--header`, `--no-info` (including the separator that replaces the info
  line), `--expect`, `--multi`, `--bind` with 39 actions (including
  `execute` with its `{}`/`{q}`/`{n}`/`{+}`/`{f}`/`{r}` placeholders,
  `print`, `become` and `reload`), the inline layout (`--height`,
  `--min-height`), `--layout=reverse-list`, `--ellipsis`, `--no-bold`,
  `--no-color`, and the preview window in all four positions with and
  without `--border`;
- walker: the default file listing matches the reference's file **set** for
  every `--walker` combination (the reference walks the tree concurrently, so
  its order is not deterministic either);
- errors: `unknown option: X`, `invalid color specification: …` and the exit
  statuses match the reference byte for byte.

**Known exceptions** (each individually justified):

1. fzf's file listing order is nondeterministic in the reference itself
   (concurrent walk), so the walker test compares the resulting set.
2. fzf shows a spinner (colour 148) while the input is still being read;
   the C23 port reads the input synchronously and never shows one, so that
   colour is excluded from the palette comparison.
3. fzf's preview window has a scrollbar and an `N/M` indicator for long
   output — implemented; the remaining preview gap is the `{f}`/`{+}`/`{r}`
   placeholder set and the `wrap`/`cycle` options.

## 6. Sanitizers and static analysis

- AddressSanitizer + UndefinedBehaviorSanitizer: clean over the option
  matrix, the interactive path and the walker.
- LeakSanitizer: clean; the allocation-failure harness found and fixed a
  **double free** in the V2 matcher (`free(H); free(C)` before a `goto
  cleanup` that freed them again) and a dozen unchecked allocations.
- `gcc -fanalyzer`: no findings.
- `valgrind` is not installed in this environment, so leak checking relies on
  AddressSanitizer/LeakSanitizer.

## 7. Performance

`bash tests/bench.sh`, 200 000 generated lines, same machine, `-O2`:

| Workload | Reference (Go) | C23 | Ratio |
|---|---|---|---|
| `--filter=alpha` | 77 ms | 89 ms | 1.16× |
| `--filter=a` | 225 ms | 130 ms | 0.58× |
| `--filter=src` | 78 ms | 71 ms | 0.91× |
| `--version` (startup) | 4 ms | 5 ms | 1.25× |

After the optimisations the C23 implementation is faster than the Go
reference on the broad single-character query and on a mid-selective query,
and only slightly slower on the most selective one.  The optimisations
were:

- the batch driver decodes every item into one reusable scratch buffer
  instead of allocating a rune buffer per item;
- the interactive mode decodes the items once and reuses them for every
  search, instead of re-decoding the whole list on each keystroke;
- `mchars_init()` computes the length, the code point count and the ASCII
  flag in a single pass over the bytes;
- the matcher keeps its score matrices in a reusable workspace instead of
  allocating them per item;
- the results are sorted with a stable LSD radix sort over a packed 64-bit
  key instead of `qsort()` with an indirect comparator.

## 8. Platforms and dependencies

- POSIX: Linux (tested on Gentoo, x86-64), glibc.
- Toolchains: GCC 16.2.1 and Clang 22.1.8, both with `-std=c23`.
- Libraries: libc, POSIX `<regex.h>` (for regex `--delimiter`), `libm`.
- No ncurses, no other third-party dependency.
- The shell integration covers **bash** only; zsh and fish are out of scope.

## 9. Known limitations

- 51 of 131 CLI options, 93 of 132 actions and all 13 events are not
  implemented. Unimplemented options are **rejected** with the reference's
  `unknown option: X` message and exit status 2 — never silently ignored.
- No `--listen`, `--tmux`, `--popup`, `--with-shell`, `--threads`, `--bench`
  or `--man`.
- The batch driver is faster than the reference on two of the three
  measured workloads and up to 16 % slower on the third (see §7).
- Allocation-failure injection covers the batch paths (360 injections) and
  the interactive paths (140 injections).
- The man page documents the implemented feature set, not the full upstream
  one.

## 10. Commits

Branch `fzf-c23-rewrite` (local only; nothing is pushed and the branch has
no upstream configured). The port is a sequence of verified units:

```
6daecc3 feat: add ANSI escape/color parser with fzf differential fixtures
6b6415c refactor: remove legacy mmenu code and build pure C23 fzf
a740261 feat: add terminal renderer, key decoder and interactive UI matching fzf layout
2d12f2e feat: add Latin normalization, regex delimiters, expect/reverse/header options, strict option validation
cb6ff02 feat: add --color theme parsing matching fzf's palette and error messages
217808b feat: add border shapes and the default file listing walker
e0c107b docs: add man page with a consistency test for the documented options
1976a71 feat: add the preview window with reference-matching layout
60b8f44 fix: handle allocation failures; add an allocation-failure injection harness
fb7363f feat: add the preview scrollbar and line indicator
```

CI: `.github/workflows/ci.yml` builds with gcc, runs the differential suites
and a `-fanalyzer` pass. It has not been executed in this environment (no
network access and no `gh`); the results above were produced locally with
the same commands the workflow runs.

## 11. Verified facts vs estimates

Everything in §4, §5, §6 and §7 was measured. The only estimates are the
feature-completion percentages in §3, which are counts over the inventory in
`FEATURES.md`, and the note that the C23 port is "comparable" to the
reference on interactive latency — the interactive path was verified
screen-by-screen for the covered scenarios, not benchmarked.
