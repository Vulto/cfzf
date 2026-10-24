# HANDOVER.md — continuing the C23 fzf rewrite

Everything below is verified state as of the last commit. Work is **local only**:
no remotes are pushed, the branch has no upstream.

## 1. What this project is

A from-scratch **ISO C23 rewrite of junegunn/fzf**, behaviour-compatible with
the reference. The old `mmenu` codebase was removed entirely; the tree is now
a pure fzf clone.

- Reference: **v0.74.3**, commit `15f64c492a08f0840b81540c7d1de35737448086`
- Source of record: `/tmp/opencode/fzf-src` (checked out at that commit)
- Reference binary used by the tests: resolved by `tests/ref.sh`
  (`$FZF_REF` → `/usr/bin/fzf` → a binary built from the pinned commit).
  The system `/usr/bin/fzf` is **not present in this environment** any more,
  so `tests/ref.sh` builds the reference from source into
  `/tmp/opencode/fzf-ref` (it reports `0.74 (devel)`, same code as the tag).

See `FINAL_REPORT.md` for the consolidated status report (PLAN §12).

## 2. Build and test

```sh
sh ./build.sh                                  # -> ./fzf, installs to $PREFIX/bin/fzf
sh tests/batch-diff.sh                         # 51/51  batch options vs reference
sh tests/algo-diff.sh                          # 27/27  ranking / tie-breaks
sh tests/nth-diff.sh                           # 55/55  --nth / --with-nth
sh tests/pty-diff.sh                           # 35/35  interactive screens
sh tests/walker-diff.sh                        #  8/8   default file listing
sh tests/gen-ansi-probe.sh                     # 21/21 + 15500/15500 + 15/15
sh tests/shell-integration.sh                  #  7/7   bash integration
sh tests/bench.sh                              # throughput vs the reference
sh tests/manpage.sh                            #  2/2   man page consistency
```

Strict build (both compilers are clean):
`cc -std=c23 -Wall -Wextra -Wpedantic -Werror -O2 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -Isrc -o fzf src/*.c -lm`

Sanitizers and static analysis are clean:
`cc -std=c23 -g -O1 -fsanitize=address,undefined ... -Isrc -o fzf-asan src/*.c -lm`
`gcc -std=c23 -fanalyzer ... -fsyntax-only src/*.c`

## 3. Architecture

| Module | Responsibility |
|---|---|
| `src/main.c` | CLI parsing, batch (`--filter`) mode, interactive mode, file listing |
| `src/filter.c` | batch search driver over items |
| `src/pattern.c` | extended query grammar, per-term matching, normalization rules |
| `src/algo.c` | fuzzy V1/V2, exact/prefix/suffix/boundary matchers, scoring |
| `src/result.c` | sort criteria, tie-breaks, ranking |
| `src/tokenizer.c` | `--nth`/`--with-nth` ranges, string **and regex** delimiters |
| `src/item.c`, `src/util.c` | item model, UTF-8 primitives, runewidth |
| `src/ansi.c` | ANSI escape/colour parsing (offsets + states) |
| `src/terminal.c` | raw-termios renderer: alt screen, SGR, cursor, border glyphs |
| `src/keys.c`, `src/event.c` | key decoding, event model, key names |
| `src/bind.c` | `--bind` parsing, action model, keymap lookup |
| `src/color.c` | `--color` parsing and the default Dark256 theme |
| `src/walker.c` | default file listing (fzf run without piped input) |
| `src/ui.c` | interactive loop and layout |
| `src/tui.h` | shared `Color`/`Attr` types |
| `fzf.1` | man page; installed by `build.sh` to `$PREFIX/share/man/man1/fzf.1` |

## 4. Verified parity (do not regress these)

- Matching: fuzzy V1/V2, exact/prefix/suffix/equal/exact-boundary, the extended
  query grammar (`'…`, `^…`, `…$`, `!…`, `|`, `\'`), smart case, **Latin
  normalization** (on only when the pattern itself is unchanged by it),
  `--nth`/`--with-nth`, string and regex delimiters, `--scheme`, `--tiebreak`.
- Interactive: fzf 0.74's **bottom-up default layout**, pointer column,
  `N/M` info line, `··` ellipsis truncation, right-hand **scrollbar** column
  (fzf's `getScrollbar` geometry), all ten `--border` shapes, `--color`
  palette, `--reverse`, `--header`, `--no-info`, `--expect`, `--multi`,
  `--bind` with `execute`/`change-*`/`print-query`.
- Walker: the default file listing matches the reference's file **set** for
  every `--walker` combination.

## 5. Remaining work, in priority order

1. **Preview window extras** — `--preview` / `--preview-window` are done
   (see `src/preview.c`): the command runs asynchronously per selection,
   `{}`, `{q}` and `{n}` are expanded, the box and geometry match the
   reference for all four positions (with and without `--border`), and the
   preview's own scrollbar plus the "N/M" line indicator are drawn for long
   output.  Still missing: the `{f}`/`{+}`/`{r}` placeholders and the
   `wrap`/`nowrap`/`cycle` preview options.
2. **Inline layout** (`--height`, `--layout=reverse`) — fzf's non-fullscreen
   mode; `src/terminal.go` line 994 computes `fullscreen` from `opts.Height`.
   The reference's geometry was measured (12-row and 20-row terminals, five
   items, so the content is seven rows: five items + info + prompt):

   | `--height` | UI rows (12-row term) | UI rows (20-row term) |
   |---|---|---|
   | `3`  | 0..2   | 0..2   |
   | `5`  | 0..4   | 0..4   |
   | `10` | 3..9   | 3..9   |
   | `12` | 5..11  | -      |
   | `20` | -      | 13..19 |
   | `20%` / `50%` | 5..11 | 5..11 |
   | `100%`| -     | 13..19 |

   The content is laid out bottom-up inside a window whose bottom row is
   `height - 1`, clipped at row 0.  The *percentage* form does not scale
   with the terminal height the way the absolute form does (fzf's
   `MinHeight` default of -10 becomes 10 for percentages, see
   `src/options.go:3857`).  The window also has to be drawn without the
   alternate screen and erased on exit, which is why this needs an inline
   mode in the renderer.
3. **The list/preview separator** (`--separator=STR`) and the remaining
   layout options (`--padding`, `--margin`, `--oneline`, `--line-number`,
   `--freeze-left`/`--freeze-right`, the `--*-border` and `--*-label`
   options).

4. **Remaining CLI options** — and keep `fzf.1` in sync: `tests/manpage.sh`
   fails if the man page documents an option the binary does not accept. — see `FEATURES.md` for the per-option status
   (51 of 131 still unstarted: `--listen`, `--tmux`, `--popup`, `--with-shell`,
   `--threads`, `--bench`, `--man`, `--scroll-off`, `--hscroll-off`,
   `--separator`, `--walker` extras, layout
   options, scrollbar/gutter options, `--print`/`--expect` variants, …).
5. **Remaining actions/events** — `FEATURES.md` lists 132 actions and 13
   events; 26 actions work today. `--bind` plumbing is in place, so most
   actions are a small addition each.
6. **Allocation-failure injection** (PLAN §6) — no harness yet.
7. **Batch throughput** — currently 1.5×–1.7× slower than Go on 200k lines;
   the cause is per-item allocation in `mm_batch_search` (fzf uses a slab
   allocator and a rune cache). `src/filter.c` is the place to change.
8. **Performance + final report** — `tests/bench.sh` works; the consolidated
   report required by PLAN §12 is not written yet.

## 6. Pitfalls worth knowing

- **Tooling**: file edits made with heredocs/`python - <<PY` inside the shell
  tool have silently failed to persist several times. Always re-read or
  `grep` the file right after an edit, and prefer the `edit` tool.
- **The reference binary is gone** from `/usr/bin`; `tests/ref.sh` rebuilds it.
  A copy of the C23 binary was found installed at `/usr/local/bin/fzf`
  (root-owned, not removable here) — it is *not* a valid reference.
- **The reference's file listing order is nondeterministic** (fastwalk walks
  concurrently), so compare the *set* of paths, not the order.
- **fzf's spinner** (colour 148) is only shown while the input is still being
  read; the C23 port reads synchronously, so `tests/sgrdump.py` ignores 148.
- **Border geometry**: with `--border`, the box spans the *full* width; the
  list content is inset by 2 columns on the left and the right border column
  is at `width-1`; the info line starts at the same column as the list
  content. `--border=horizontal` / `=vertical` only draw the horizontal or
  vertical edges and do not shift the content in the other direction.
- **fzf's `--color` error message** is `invalid color specification: <spec>`
  with exit status 2 — match it exactly.
- Unknown options must be rejected (`unknown option: X`, exit 2); fzf never
  ignores an option silently, and neither may this port.

## 7. Ground rules from `PLAN.md`

- Every feature needs a **differential test against the reference** before it
  counts as done; a feature is not "implemented" because its name appears in
  the code.
- No placeholder implementations, no options that are accepted but ignored,
  no fake success paths.
- Keep `PORTING_STATUS.md` (milestone, completed/pending feature IDs,
  mismatches, blockers, latest test results, benchmarks, next action) and
  `FEATURES.md` (per-feature matrix) up to date after each verified step.
- Commit each verified unit separately, in the repo's style
  (`feat:`, `fix:`, `docs:`, `test:`, `build:`).
