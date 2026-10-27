# cfzf C23 Rewrite Task List

## Primary objective

Deliver a **complete, drop-in-compatible rewrite of fzf in C23**. The pinned
upstream Go implementation is the behavioral specification. Testing,
inventories, documentation, and benchmarks exist to drive and verify the rewrite;
they are not the deliverable by themselves.

Reference contract: `junegunn/fzf` v0.74.3, commit
`15f64c492a08f0840b81540c7d1de35737448086`.

A feature is complete only when implemented in C23 and backed by reproducible
tests against the pinned reference. Source presence, historical reports, and
unverified feature labels do not establish parity. Keep working directly on
`main`, in small, testable commits. Fix failures before moving to unrelated work.

## Phase 0 — Make progress measurable (supporting work)

These tasks enable implementation; they must not turn test-infrastructure work
into the project's main objective or indefinitely block feature development.

- [ ] Pin the reference executable and fail closed if it cannot be built or run.
- [ ] Ensure differential tests compare relevant stdout, stderr, exit status,
  and PTY behavior; regenerate fixtures rather than trusting stale artifacts.
- [ ] Establish a strict C23 build and a reproducible baseline CI run.
- [ ] Keep the feature/parity inventory and project documentation consistent.
- [x] Add a CI inventory audit that reports declared statuses and missing IDs without presenting them as a completion percentage.
- [ ] Add per-feature links to named differential cases and their observed results; only then calculate feature-level verified coverage.
- [ ] Record baseline evidence in `BASELINE_EVIDENCE.md`, including tested
  commit, reference commit, toolchain, commands, results, and skipped checks.

**Gate:** the differential harness is trustworthy enough to validate each fix.
Finish or repair individual infrastructure gaps as needed; do not postpone
implementing known missing functionality merely to polish unrelated tooling.

## Phase 1 — Map the implementation and prioritize real gaps

- [ ] Inspect the C23 codebase and map executable entry points, modules,
  dependencies, build commands, and tests.
- [ ] Derive the canonical option/alias, action, event, binding, and integration
  inventories from the pinned upstream source.
- [ ] For each subsystem, record C implementation status, upstream behavior,
  a reproducer/test, and dependencies on other subsystems.
- [ ] Identify the largest functional gaps and build/test blockers; order work by
  compatibility impact and prerequisite dependencies.
- [ ] Convert every discovered mismatch or missing feature into a concrete
  implementation task with a regression test.

## Phase 2 — Complete core non-interactive fzf behavior in C23

- [ ] Implement and verify CLI parsing, aliases, defaults, validation, help,
  version, errors, output streams, and exit statuses.
- [ ] Match input ingestion, streaming, records, delimiters, field processing,
  ANSI handling, Unicode/encoding behavior, and malformed/empty input.
- [ ] Match query grammar, all supported matching modes, scoring, match
  positions, ranking, tie-breaking, sorting, and filtering semantics.
- [ ] Match output formatting, selection/output modes, no-match behavior,
  pipelines, broken pipes, and non-TTY operation.
- [ ] Test option interactions and edge cases against the reference, not only
  isolated happy paths.
- [ ] Fix every reproduced mismatch and retain it as a regression test.

**Gate:** covered batch/CLI behavior matches the pinned reference, with all
remaining gaps explicitly listed rather than implied to be complete.

## Phase 3 — Complete interactive fzf behavior in C23

- [ ] Reproduce application state transitions, query editing, cursor movement,
  candidate navigation, scrolling, selection, and multi-selection.
- [ ] Implement and verify all upstream actions, action chains, key bindings,
  events, placeholders, and relevant configuration behavior.
- [ ] Match terminal rendering, styles, layout, resize behavior, cursor state,
  terminal modes, and restoration on normal exit, errors, and signals.
- [ ] Match streaming updates, reload/cancellation, preview windows/commands,
  subprocess lifecycle, asynchronous work, and cleanup.
- [ ] Verify shell integration and scripting/programmatic interfaces.
- [ ] Compare PTY screen state and relevant control sequences; normalize only
  differences proven irrelevant or nondeterministic.
- [ ] Add regression coverage for every discovered interactive discrepancy.

**Gate:** all implemented interactive paths have repeatable differential tests,
and every known gap is recorded with a next action.

## Phase 4 — Close the complete compatibility gap

- [ ] Reconcile the feature inventory against the complete upstream surface;
  investigate absent IDs, unimplemented features, and untested interactions.
- [ ] Run the full differential suite from clean builds and resolve every
  reproducible, unexplained mismatch.
- [ ] Verify the harness detects an intentionally wrong reference or
  implementation binary.
- [ ] Run fuzzing, allocation-failure injection, ASan/UBSan/LSan, static
  analysis, and stress tests where supported; fix identified defects.
- [ ] Test supported platforms/toolchains where available and document any
  concrete environmental blocker or justified platform-specific difference.
- [ ] Keep verified, implemented-but-unverified, missing, failing, blocked, and
  not-applicable statuses distinct.

**Gate:** no unexplained differences remain in the covered scope; uncovered or
unverified behaviors are explicitly reported, not mislabeled as parity.

## Phase 5 — Measure and optimize without regressing parity

- [ ] Benchmark both implementations on identical small, medium, large, and
  stress workloads.
- [ ] Measure ingestion, matching, sorting, interactive update latency, startup,
  peak RSS, and allocations separately where practical.
- [ ] Record repeated-run statistics, corpus characteristics, toolchain flags,
  and exact revisions.
- [ ] Profile actual bottlenecks before changing algorithms, data layout, or
  concurrency.
- [ ] After each optimization, rerun focused tests and the full compatibility
  suite; never trade away semantics to improve a benchmark.

## Current known state (must be refreshed as work proceeds)

- `tests/ref.sh` builds the pinned upstream commit in a fresh Git worktree and
  does not silently choose an arbitrary system `fzf`.
- `tests/batch-diff.sh`, `tests/algo-diff.sh`, and `tests/nth-diff.sh`
  regenerate test corpora and compare output streams and exit status.
- The last recorded inventory had 131 option rows (77 verified, 20 implemented
  but unverified, 34 unstarted), 125 action rows (64 implemented but
  unverified, 61 unstarted), and 13 event rows (all unstarted). Seven action
  IDs in the 001–132 range were absent. These counts are a warning that the
  inventory needs reconciliation, not proof of implementation completeness.
- Historical test totals and benchmark numbers are not current evidence.
  `BASELINE_EVIDENCE.md` must identify the exact tested commit and run.
- Interactive `--select-1` / `--exit-0` now check the initial result set and
  wait for asynchronous input completion before auto-exiting; PTY regression
  cases were added, but have not yet been run against the pinned reference.
- Refresh these facts from the repository and latest CI before relying on them.

## Reporting and completion rules

- Update this checklist when work is actually complete, and commit code, tests,
  and status changes together when practical.
- Work on one concrete implementation or validation task at a time; build and
  test it, inspect failures, fix root causes, and rerun before proceeding.
- Do not claim a feature or phase complete based only on a green build, source
  inspection, or test counts that do not cover that behavior.
- The project is done only when the C23 application covers fzf's targeted
  behavior, the available compatibility and safety suites pass, remaining
  limitations are explicit, and performance claims are backed by reproducible
  measurements.

The deliverable is the C23 fzf rewrite—not the task list, test harness, or
documentation.
