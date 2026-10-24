# cfzf Rewrite Task List

Reference contract: `junegunn/fzf` v0.74.3, commit
`15f64c492a08f0840b81540c7d1de35737448086`.

A task is **complete** only when its acceptance criteria are backed by a fresh,
reproducible test run. Source presence, old report text, and a test count without
a recorded revision do not establish parity.

## Milestone 1 — Trustworthy baseline (current)

- [ ] **M1.1 — Pin the reference executable.** Differential tests must use the
  designated upstream commit. Never silently prefer an arbitrary system
  `fzf` binary or a stale build.
- [ ] **M1.2 — Make test failures fail closed.** Compare stdout, stderr, and exit
  status where relevant; fail if the reference is missing, a command fails to
  execute, or a case is skipped unexpectedly.
- [ ] **M1.3 — Make test inputs reproducible.** Rebuild generated corpora from
  the current script/version, rather than trusting stale files in `/tmp`.
- [ ] **M1.4 — Record evidence.** Record cfzf commit, reference commit, compiler,
  build flags, commands, pass/fail totals, and skipped tests for every baseline.
- [ ] **M1.5 — Reconcile documentation.** Make `PLAN.md`, `PORTING_STATUS.md`,
  `FEATURES.md`, and `FINAL_REPORT.md` agree on status and totals. Keep
  measured facts distinct from claims that have not been rerun.
- [ ] **M1 exit gate.** Fresh strict build and all existing differential suites
  pass against the pinned reference. Any test failure or unexplained difference
  blocks later milestones.

## Milestone 2 — Complete CLI and batch behavior

- [ ] Derive the canonical option and alias list from the pinned upstream source.
- [ ] Differentially test each option's parsing, defaults, validation, errors,
  output streams, exit status, and interactions.
- [ ] Verify query grammar, scoring, ranking, tie-breaking, delimiters, fields,
  ANSI handling, Unicode, empty input, record boundaries, and malformed input.
- [ ] Add regression fixtures for every discovered mismatch.

## Milestone 3 — Complete interactive behavior

- [ ] Inventory all upstream actions, events, key bindings, and placeholders.
- [ ] Differentially test state transitions and action sequences through a PTY.
- [ ] Verify streaming input, reload/cancellation, preview, terminal resize,
  selection, shell integration, subprocess lifecycle, and terminal restoration.
- [ ] Compare logical screen state and relevant output/control sequences; only
  normalize differences demonstrated to be irrelevant or nondeterministic.

## Milestone 4 — Safety and compatibility certification

- [ ] Run all differential suites from clean builds.
- [ ] Run allocation-failure injection, fuzzing, ASan/UBSan/LSan, and static
  analysis where available.
- [ ] Confirm test harnesses fail when given an intentionally wrong reference
  or implementation binary.
- [ ] Resolve every reproducible difference; document genuine upstream
  nondeterminism with evidence rather than masking it.

## Milestone 5 — Profile and optimize

- [ ] Benchmark input ingestion, matching, sorting, and end-to-end latency
  separately over multiple corpus sizes and line-length distributions.
- [ ] Record repeated-run medians, peak RSS, allocation counts, compiler flags,
  and machine/reference revisions.
- [ ] Profile the reported large-input slowdown before changing data layout or
  threading.
- [ ] Re-run the full compatibility and safety suites after each optimization.

## Current evidence and known inventory gaps

- `tests/ref.sh` now builds from the pinned source commit in a fresh Git
  worktree and does not silently select a system executable.
- `tests/batch-diff.sh`, `tests/algo-diff.sh`, and `tests/nth-diff.sh`
  regenerate their input corpus and compare output streams and exit status.
- `FEATURES.md` currently has 131 option rows (77 marked verified, 20
  implemented but unverified, 34 unstarted), 125 action rows (64 implemented
  but unverified, 61 unstarted), and 13 event rows (all unstarted). Seven
  action IDs from the 001–132 range are absent; the canonical action inventory
  must be reconciled against the pinned upstream source.
- Historical test totals and benchmark numbers are not evidence of a current
  baseline. `BASELINE_EVIDENCE.md` records the latest clean CI run and its
  exact coverage. Benchmark claims remain historical until rerun in milestone 5.

## Reporting rule

Update this file only when the corresponding work is actually complete. Never
mark a milestone complete solely because its issues or implementation tasks
were created.
