# Mission: Build a Behaviorally Identical, High-Performance C23 Reimplementation of fzf

Execution tracking: `TASKS.md` is the authoritative milestone checklist; `BASELINE_EVIDENCE.md` records commit-specific local verification evidence. GitHub Actions workflows are intentionally disabled; do not use GitHub CI. Do not treat this mission specification or historical reports as proof that a task is complete.

## 1. Role and objective

Act as a senior systems programmer, reverse engineer, testing engineer, and performance engineer.

Your task is to transform my C23 implementation into a functionally equivalent, drop-in-compatible reimplementation of the original Go application, **fzf**.

The original Go implementation is the authoritative specification. Its actual observable behavior—not assumptions about what fzf should do—is the standard against which every relevant part of the C implementation must be evaluated.

This is a full application compatibility project.

You must reproduce the original application's behavior across its complete scope, including:

- Core fuzzy-matching algorithms and scoring.
- Query parsing and matching semantics.
- Filtering, ranking, sorting, and result ordering.
- Input ingestion and stream processing.
- Command-line arguments, defaults, and option interactions.
- Interactive application state and terminal user interface.
- Keyboard input, bindings, actions, and event processing.
- Selection, multi-selection, and output generation.
- Preview commands and subprocess management.
- Shell integration, scripting, and programmatic usage.
- Standard input, standard output, standard error, and exit codes.
- Unicode, text processing, delimiters, and encoding edge cases.
- Concurrency, asynchronous work, cancellation, and process lifecycle.
- Errors, interruptions, cleanup, and resource management.
- Performance, memory consumption, and scalability.
- Every other user-observable behavior supported by the targeted version of the original.

The goal is not to create an application inspired by fzf. The goal is to make the C23 version behave as though it were the original application, while achieving measurable performance and resource-efficiency improvements wherever possible.

**The compatibility rule: if a valid experiment shows that the original and C implementation behave differently, treat the difference as a bug until it is explained and verified otherwise.**

Do not sacrifice correctness for performance.

## 2. Repository configuration

Identify and verify these paths and versions before making changes:

- Original Go repository: `<PATH_TO_ORIGINAL_FZF>`
- C23 repository: `<PATH_TO_C23_FZF>`
- Target Go version or commit: `<ORIGINAL_COMMIT_OR_VERSION>`
- Starting C commit: `<C_COMMIT_OR_VERSION>`
- Target platforms: `<TARGET_PLATFORMS>`
- Build and test commands: discover from the repositories.

Do not assume a build system, API, directory structure, feature set, or implementation architecture without inspecting the repositories.

Pin down the exact original version that defines the compatibility contract. Do not unknowingly compare against a newer installed fzf binary or a different source revision.

Determine how each implementation is built, invoked, configured, tested, and integrated with its environment.

Preserve the existing C project's working functionality. Avoid unnecessary rewrites or architectural changes until you understand its current design and the differences that need to be corrected.

## 3. The source of truth

Use the following evidence hierarchy:

1. Reproducible runtime behavior of the designated original binary.
2. Tests from the corresponding original source revision.
3. Documentation and examples for that revision.
4. The corresponding Go implementation's source code.
5. Git history, design discussions, and other explanatory material, when available.

Runtime behavior is particularly important when documentation and implementation differ.

When the behavior is ambiguous, write a minimal experiment that distinguishes competing interpretations. Execute it against the original, record the result, and turn the observation into an explicit compatibility test.

Separate three kinds of conclusions:

- **Verified:** observed through execution or established by an authoritative test.
- **Inferred:** supported by source code or indirect evidence, but not yet independently verified.
- **Unknown:** insufficient evidence to make a reliable claim.

Do not silently convert inferences or assumptions into verified behavior.

Do not alter the intended compatibility target to accommodate an accidental limitation of the C implementation.

## 4. First phase: map the entire application

Before implementing new behavior, inspect both repositories and construct a comprehensive application map.

Identify:

- All executable entry points.
- All command-line options and their defaults.
- Input parsing and source ingestion.
- Core matching and scoring algorithms.
- Query representation and parsing.
- Filtering and result management.
- Ranking and sorting logic.
- Item and selection models.
- Application state transitions.
- Interactive event processing.
- Terminal rendering and input handling.
- Preview execution and subprocess management.
- Actions, key bindings, and event handlers.
- Output generation and exit behavior.
- Non-interactive and scripting paths.
- Configuration and shell integration.
- Concurrency and cancellation mechanisms.
- Error handling and cleanup paths.
- Existing unit, integration, property-based, and end-to-end tests.

Follow the control flow from process startup to termination, including both interactive and non-interactive execution.

Identify the corresponding implementation—or missing functionality—in the C repository for every important subsystem.

Create a dependency map showing which subsystems affect others. For example, a scoring change can affect ordering, the current item, preview contents, selection, output, and responsiveness. A bug in input parsing can affect the matcher and every feature built on top of it.

Do not treat subsystems as independent when the application couples their behavior.

## 5. Build a complete behavioral specification

Create a machine-readable feature inventory and a detailed parity matrix.

Each feature or behavior must include:

- Feature identifier and category.
- Original behavior.
- Relevant source files or code paths in both repositories.
- Whether the C implementation supports it.
- Preconditions and configuration.
- Reproducible test scenario.
- Exact expected result.
- Actual result from each implementation.
- Differences found.
- Severity and affected subsystems.
- Evidence and reproduction artifacts.
- Current verification status.
- Regression test identifier.
- Fix or next action.

Use explicit statuses such as:

- Not investigated.
- Investigated but unverified.
- Missing.
- Failing.
- Passing.
- Blocked.
- Not applicable, with justification.

Do not mark a feature as complete merely because similarly named functions exist in both codebases.

Define compatibility at the level of inputs, state transitions, externally observable results, and documented or experimentally verified side effects.

The inventory must include interactions between features, not only isolated option-by-option checks.

## 6. Core algorithms and matching semantics

Treat the matching engine as a first-class compatibility requirement.

Inspect the original's actual matching implementation, query parser, scoring functions, and configuration-dependent execution paths. Reproduce the behavior of the targeted revision rather than implementing a generic fuzzy matcher.

Verify all matching modes and query expressions supported by the original.

Where applicable, test:

- Fuzzy matching.
- Exact matching.
- Prefix and suffix matching.
- Extended query expressions.
- Inverse or exclusion terms.
- OR expressions and other supported logical operators.
- Anchored matching.
- Case-sensitive, case-insensitive, and smart-case behavior.
- Whole-word and related boundary modes.
- Field restrictions and transformations.
- Tokenization and delimiter-dependent matching.
- Multiple search terms.
- Empty queries and empty patterns.
- Invalid or unusual query expressions.
- Match positions and highlighted character ranges.
- Match scoring and tie-breaking.
- Query parsing errors and fallback behavior.

Use the feature inventory from the original to determine which modes exist in the target revision. Do not introduce behaviors from another version merely because they are familiar from fzf.

For each relevant case, compare:

1. Whether an item matches.
2. The locations of matched characters.
3. The computed score, if exposed or needed to reproduce ordering.
4. The result's relative rank.
5. Tie-breaking behavior.
6. The behavior under each applicable configuration.
7. The downstream UI and output consequences.

Pay close attention to algorithm details that can look inconsequential but change results:

- Character positions and gaps.
- Consecutive matches.
- Word and path boundaries.
- Case changes.
- Separators and punctuation.
- Leading and trailing characters.
- Candidate length.
- Unicode handling.
- Score bonuses and penalties.
- Integer ranges and overflow behavior.
- Floating-point behavior, if used.
- Match-path selection when multiple alignments are possible.
- Stable versus unstable sorting.
- Deterministic tie-breaking.

Build exact, small test fixtures with known results before moving to large randomized inputs.

Whenever the original exposes a stable observable outcome, the C implementation must reproduce it. Do not accept a result as equivalent merely because the correct items matched in a different order.

## 7. Input ingestion and data processing

Compare how the original acquires, parses, stores, transforms, and processes candidate items.

Cover all input mechanisms supported by the target revision, including standard input, files, generated input, reload actions, and other configurable sources where applicable.

Verify:

- Input ordering.
- Empty input.
- Empty records.
- Trailing newlines and missing final newlines.
- Delimiters and custom record separators.
- Embedded whitespace.
- Leading and trailing spaces.
- Large records.
- Duplicate records.
- ANSI escape sequences in candidate text.
- UTF-8 and malformed byte sequences.
- Binary-like input, where applicable.
- Field extraction and transformation.
- Character and field offsets.
- Input arriving incrementally.
- Input arriving faster than it can be processed.
- Input streams that remain open.
- Large input volumes and memory pressure.
- Errors while reading input.
- Cancellation and early termination.

Determine how the original treats separators, newline boundaries, record identities, and text transformations. These decisions may influence matching, display, selection, and output.

Test both complete-input and streaming scenarios. The C implementation must not accidentally change semantics because it reads an entire input before processing while the original behaves incrementally, or vice versa.

## 8. Ranking, sorting, and filtering

Verify the full pipeline from candidates and queries to the final ordered result set.

Compare:

- Filtering semantics.
- Matching and non-matching item handling.
- Result counts.
- Scoring and ranking.
- Match-order options.
- Sorting modes and sort-disabled behavior.
- Tie-breaking and stability.
- Reverse order and custom ordering.
- Multi-term searches.
- Field-based transformations.
- Changing the query while processing candidates.
- Asynchronous updates and stale results.
- Empty and exceptionally large result sets.

When scores or ordering differ, locate the earliest divergence. Do not hide a ranking bug by sorting the final list in an arbitrary way that happens to fix one test.

The same underlying results must drive the interface, selection state, preview state, and final output as they do in the original.

## 9. Application state and event processing

Reproduce the original's state machine rather than building a UI that only resembles its current screen.

Identify all relevant state variables and transitions, including query state, candidate state, result ordering, cursor position, selection, scroll position, preview state, application modes, and pending operations.

For every significant event, determine:

- The state before the event.
- The triggering input or operation.
- The resulting state.
- Which operations are performed.
- Whether filtering, sorting, rendering, preview updates, or output are triggered.
- The resulting externally observable behavior.

Investigate keyboard events, action dispatch, custom bindings, event handlers, cancellation, resize events, reloads, and asynchronous work.

Test event sequences, not just individual key presses.

Include rapid consecutive inputs, repeated actions, input received during a redraw, and state changes that occur while computation or subprocess work is pending.

Check whether the original debounces, coalesces, cancels, queues, or discards work. Reproduce the observable effect of those policies.

A C implementation may have a completely different internal state representation, but its externally observable state transitions must remain compatible.

## 10. Terminal UI and rendering

Implement and verify complete terminal parity.

Use a pseudo-terminal for interactive comparisons, with identical input events and equivalent environments.

Compare:

- Initial rendering and time to first stable screen.
- Query prompt and editable query.
- Cursor location, visibility, and movement.
- Candidate order and visible subset.
- Current item and selection highlighting.
- Multi-selection markers.
- Headers, footers, labels, and counts.
- Borders, margins, padding, and alignment.
- Color schemes and text attributes.
- Truncation, wrapping, and clipping.
- Scrolling and positioning.
- Preview windows and layout.
- Resize behavior.
- Clearing and redrawing.
- Terminal control sequences.
- Terminal modes and cursor restoration.
- Output and cleanup after successful or abnormal termination.

Compare both rendered terminal cell grids and raw control sequences. Normalize only differences proven irrelevant to the observable result.

Test terminal sizes and capabilities systematically. Include small, large, and narrow terminal geometries; color-capable and reduced-capability terminals; Unicode text; ANSI-styled candidates; and other cases supported by the original.

A visually identical frame is insufficient if the cursor is wrong, input editing behaves differently, or the terminal is left in a different state after exit.

## 11. Command-line interface and configuration

Enumerate every CLI flag, alias, default, validation rule, and relevant interaction from the target original revision.

Do not hardcode the options you remember. Derive the inventory from the actual original.

Test:

- Valid arguments.
- Invalid arguments.
- Missing argument values.
- Repeated options.
- Conflicting options.
- Options whose order changes the result.
- Option combinations.
- Defaults and configuration overrides.
- Environment variables.
- Configuration file behavior, if supported.
- Help and version output.
- Error messages.
- Exit statuses.

Compare parsing behavior, validation, defaults, side effects, and resulting application configuration.

Preserve output formatting and newline behavior wherever compatibility requires it. Account for operating-system-specific details explicitly rather than masking differences without evidence.

Check shell quoting and escaping for arguments that contain spaces, Unicode, special characters, or embedded delimiters.

## 12. Actions, bindings, and subprocesses

Inventory the action system and all applicable extension points in the original.

Verify the supported actions, key bindings, event handlers, action chains, transformations, reload behavior, preview behavior, and custom command integration.

Test subprocess lifecycle, arguments, environment, input, output, working directory, errors, termination, cancellation, and concurrent execution wherever relevant.

Use harmless deterministic test commands for repeatable subprocess tests.

Check whether output is captured, streamed, displayed, transformed, or passed to another process. Identify any relevant quoting rules, command templates, placeholders, escaping conventions, and exit-status behavior.

Do not invoke potentially destructive user-configured commands merely to test parity. Use isolated fixtures and controlled commands.

Verify shell integrations and scripts separately from the interactive UI, because they may rely on precise argument expansion, output formatting, environment handling, and exit codes.

## 13. Output and external compatibility

Treat stdout, stderr, the terminal screen, and the process exit status as distinct observable interfaces.

For every relevant mode, compare:

- Exact output bytes.
- Record order.
- Delimiters and trailing newline behavior.
- Selection formatting.
- ANSI escape handling.
- Output destination.
- Error messages and error destination.
- Exit status.
- Process termination.
- Behavior under closed pipes and broken output streams.
- Cleanup and signal handling.
- Behavior when standard input or output is not a TTY.

Exercise pipelines, redirections, empty input, no matches, multiple selections, cancellation, and invalid arguments.

If the original exposes machine-readable output or automation interfaces, preserve their contracts exactly.

The C implementation must work as a compatible command-line application, not merely as an interactive terminal program.

## 14. Unicode, encoding, and portability

Identify the text and encoding assumptions of the original and its dependencies.

Test UTF-8 multibyte characters, combining marks, wide characters, emoji, invalid sequences, tabs, control characters, path separators, and non-ASCII case behavior as applicable.

Distinguish bytes, Unicode code points, grapheme clusters, and terminal display columns.

Compare the original's observable handling of malformed or unsupported sequences instead of assuming a theoretically preferable behavior is compatible.

Test all designated operating systems, compilers, and terminals where possible. Record platform-specific differences separately, and establish which behaviors should match exactly across platforms.

Avoid undefined behavior, memory corruption, unbounded buffer assumptions, locale-dependent surprises, and undocumented compiler extensions in the C implementation.

## 15. Concurrency, cancellation, and resource management

Inspect how the original coordinates concurrent matching, asynchronous input, redraws, preview commands, subprocesses, reloads, and termination.

Determine when operations start, finish, become visible, and get cancelled or superseded.

Test for:

- Stale query results.
- Results computed from outdated state.
- Races between input and redraws.
- Out-of-order subprocess output.
- Cancellation and shutdown races.
- Deadlocks and hangs.
- Broken pipes.
- Excessive resource consumption.
- Correct propagation of failures.
- Leaked child processes.
- Terminal restoration on interruption.

Match the original's observable scheduling and cancellation behavior wherever it is part of the application contract.

Do not replace an asynchronous implementation with a simplistic synchronous one without measuring the semantic consequences. Conversely, do not add concurrency unless it preserves ordering, cancellation, and state consistency.

Use sanitizers, leak detection, stress tests, and race-detection tools where available and appropriate to the implementation.

## 16. Differential testing framework

Build a reusable harness that compares the actual behavior of the original and C implementations.

The harness should support:

1. Running both binaries with equivalent arguments and environments.
2. Providing identical input data.
3. Replaying interactive input events.
4. Capturing PTY output and terminal state.
5. Comparing screen content and terminal attributes.
6. Comparing stdout, stderr, exit codes, and observable side effects.
7. Running deterministic regression tests.
8. Generating additional input and event sequences.
9. Saving reproducible failure artifacts.
10. Reporting differences with enough information to identify the likely source of the bug.

Use appropriate testing strategies for each subsystem:

- Unit tests for deterministic algorithms and state transitions.
- Integration tests for subsystem interactions.
- Differential tests against the original executable.
- End-to-end tests for CLI and terminal behavior.
- Property-based tests for general invariants.
- Fuzzing for parsers and robustness.
- Stress tests for large data sets and extended interactive sessions.
- Performance benchmarks for verified equivalent workloads.

Generated tests must use identical inputs and reproducible seeds for both implementations.

When a test fails, preserve the seed, arguments, environment, fixture, input sequence, terminal size, and other relevant state.

For every discovered defect, create a minimal reproducer and a regression test.

Do not stop at a green unit-test suite. An implementation can pass all its own tests while remaining incompatible with the original.

## 17. Differential comparison rules

Use multiple levels of comparison.

**Level 1: Exact external contract**

Compare exact output bytes, exit status, argument behavior, and other interfaces for which exact equality is expected.

**Level 2: Semantic behavior**

Compare matched candidates, score-related behavior, result order, selection, events, query transitions, state changes, and subprocess outcomes.

**Level 3: Terminal behavior**

Compare terminal cell contents, styling, cursor state, scroll regions, and relevant terminal state transitions.

**Level 4: Timing and performance**

Compare startup latency, update latency, throughput, memory consumption, and other measurements under equivalent workloads.

Define normalization rules explicitly. A normalization is permitted only when the distinction is genuinely irrelevant and evidence supports that conclusion.

Never normalize away a difference simply because fixing it is inconvenient.

When a timing difference is expected because the implementations have different performance, distinguish a harmless timing variation from an observable semantic difference. The C implementation should not be forced to wait artificially if it can produce a correct result sooner; however, it must not skip required work or expose incorrect intermediate states.

## 18. Root-cause debugging and fixes

For each discrepancy, follow this process:

1. Reproduce the failure on both implementations.
2. Confirm that the test environment is equivalent.
3. Identify the earliest observable divergence.
4. Minimize the input or event sequence.
5. Determine the expected behavior using the original executable.
6. Trace the corresponding Go and C code paths.
7. Identify the actual root cause.
8. Document the expected behavioral contract.
9. Implement the smallest appropriate fix.
10. Add or update a regression test.
11. Run focused unit and integration tests.
12. Run the broader differential suite.
13. Check for regressions in dependent subsystems.
14. Record the evidence and mark the parity matrix accordingly.

Do not patch symptoms while leaving an underlying state or algorithm error intact.

Do not make large architectural changes in response to a small discrepancy unless the evidence shows the architecture itself is responsible.

When multiple fixes are possible, favor the change that most faithfully reproduces the original behavior while keeping the C implementation safe, maintainable, and efficient.

## 19. C23 engineering requirements

Implement the clone as a real C23 application.

Follow the repository's existing coding conventions and architecture where practical. Improve them when a verified problem warrants a change.

Prioritize:

- Correct memory ownership and lifetime.
- Explicit error propagation.
- Safe bounds checking.
- Correct integer arithmetic.
- Correct UTF-8 handling and terminal-width calculations.
- Reliable terminal input and output.
- Explicit state transitions.
- Robust subprocess management.
- Predictable cancellation and shutdown.
- Avoidance of undefined behavior.
- Appropriate separation of concerns.
- Efficient data structures and algorithms.
- Clear, maintainable interfaces between subsystems.

Use the project's supported C23 compiler configuration, useful compiler warnings, and sanitizers when available.

Do not remove warnings by disabling diagnostics. Do not hide memory bugs by weakening tests. Do not introduce nonportable assumptions without documenting them.

Avoid prematurely translating the original Go implementation line by line. Reproduce the behavior, not necessarily the architecture.

The C implementation may use better-suited data structures, memory layouts, algorithms, and system APIs provided that its observable behavior remains compatible.

Where the original relies on an external dependency whose behavior affects compatibility, inspect the dependency and its version. Determine whether the C implementation needs equivalent functionality or whether the behavior can be reproduced directly.

## 20. Performance optimization

Correctness is the acceptance gate; performance is the optimization objective.

After establishing a behavioral baseline, profile both implementations on equivalent workloads.

Include representative small, medium, large, and stress workloads covering input loading, query processing, matching, sorting, UI refreshes, previews, and other identified hot paths.

Measure relevant metrics, including:

- Process startup latency.
- Time to first stable render.
- Input-to-visible-update latency.
- Matching and sorting throughput.
- End-to-end query update latency.
- Peak resident memory.
- Allocation frequency and volume, where measurable.
- CPU consumption.
- Scaling with candidate count and candidate length.
- Behavior under rapid repeated updates.

Use multiple runs, reproducible inputs, and meaningful statistical summaries. Report the workload and environment alongside measurements.

Separate algorithmic costs from terminal output, process startup, input ingestion, and other overhead wherever practical.

Use profiling evidence to guide optimization. Potential opportunities include avoiding unnecessary allocations, reducing copying, improving memory locality, reusing buffers, choosing efficient data structures, and eliminating redundant work.

Do not assume a C implementation is automatically faster than a Go implementation. Demonstrate improvements using actual measurements.

Never obtain a faster benchmark by weakening matching semantics, changing sort order, skipping candidates, reducing the visible feature set, suppressing necessary updates, or omitting correctness checks from the compared workload.

## 21. Required execution strategy

Follow these phases in order, updating the parity matrix as evidence accumulates.

### Phase A: Baseline and inventory

- Inspect both repositories.
- Confirm the original revision and supported platforms.
- Discover build, test, and execution commands.
- Build and run both implementations.
- Record the initial state of the C implementation.
- Inventory the original's behavior and the C implementation's feature coverage.
- Identify existing tests and immediate build or runtime failures.

Deliverable: a repository map, baseline report, and prioritized parity matrix.

### Phase B: Test infrastructure

- Create a reproducible differential testing harness.
- Establish PTY-based testing for interactive mode.
- Capture CLI outputs, exit codes, and terminal state.
- Build fixtures and input-event replay.
- Implement comparison and failure reporting.
- Verify the harness itself with known matching and intentionally differing cases.

Deliverable: a usable test harness that can detect and reproduce discrepancies.

### Phase C: Core compatibility

- Test input processing.
- Test query parsing.
- Test matching and scoring.
- Test filtering, ranking, and sorting.
- Test output and exit semantics.
- Test option parsing and configuration.
- Add regression tests for each discrepancy.

Deliverable: verified compatibility for the core non-UI application behavior, with remaining gaps explicitly documented.

### Phase D: Interactive and integrated behavior

- Test application state transitions.
- Test keyboard processing and bindings.
- Test selection and action execution.
- Test terminal layout and rendering.
- Test previews, reloads, subprocesses, and events.
- Test resizing, Unicode, cancellation, and cleanup.
- Verify shell integration and scripting behavior where applicable.

Deliverable: a verified end-to-end compatibility suite across the application's supported modes.

### Phase E: Close compatibility gaps

- Run the entire differential suite.
- Investigate every unexplained mismatch.
- Fix defects and add regression coverage.
- Expand tests for interactions between options and subsystems.
- Repeat until all discovered differences are fixed or explicitly categorized with evidence.

Deliverable: a complete parity report with no unexplained high-severity discrepancies.

### Phase F: Performance and hardening

- Establish equivalent benchmark workloads.
- Profile both implementations.
- Optimize measured bottlenecks.
- Rerun regression and differential tests after every meaningful optimization.
- Run sanitizers, stress tests, fuzzing, and portability checks where available.

Deliverable: a behaviorally verified C23 implementation with reproducible performance measurements.

## 22. Operating rules for the coding agent

Be proactive. Inspect the repository, execute commands, read the source, build the code, run tests, and implement fixes instead of merely describing what someone should do.

Do not ask me to approve every reasonable implementation step. Use the available evidence and make the best technical decisions consistent with this specification.

Do not invent test results, claim tests passed without executing them, or mark a subsystem complete based only on source inspection.

Do not overwrite existing user changes, discard unrelated modifications, or perform destructive Git operations without a specific justification and authorization.

Do not silently modify the reference implementation to make the comparison pass. Keep the original as the behavioral authority.

If a dependency, platform, tool, or environment prevents verification, identify the blocker precisely and continue with the parts that can be tested. Clearly distinguish incomplete verification from success.

When a discovered behavior is intentionally different, document the exact difference, its justification, its observable consequences, and its impact on compatibility. Do not classify accidental incompatibilities as intentional merely to reach completion.

At the end of each work session, summarize:

- What was inspected or implemented.
- Which tests were executed.
- Which tests passed or failed.
- Which parity gaps remain.
- Which regressions were discovered.
- Which performance measurements were obtained.
- What evidence supports each conclusion.
- The most important next actions.

Always keep the actual code, the regression suite, and the parity matrix consistent with one another.

## 23. Definition of done

Do not declare the reimplementation complete just because it compiles, looks right, or passes a few sample tests.

The project is complete only when:

1. The targeted original revision is pinned and reproducible.
2. The original's public behavior has been comprehensively inventoried.
3. Every discovered in-scope feature has a recorded compatibility status.
4. Core algorithms and matching semantics have been tested against the original.
5. CLI behavior and machine-consumable output contracts have been verified.
6. Interactive UI, state transitions, actions, and terminal lifecycle have been tested.
7. Relevant subprocesses, events, scripting paths, and integration behavior have been tested.
8. Regression tests cover the defects found during implementation.
9. The full available test suite passes.
10. The differential suite shows no unexplained differences in the covered scenarios.
11. Unverified features, platform limitations, and remaining deviations are explicitly documented.
12. Memory safety and robustness checks reveal no unresolved known critical defects.
13. Performance claims are backed by reproducible measurements under equivalent workloads.

Distinguish tested coverage from the theoretical impossibility of proving equality for every possible input. Never claim exhaustive mathematical proof when the evidence consists of finite tests.

## 24. Begin now

Start with Phase A.

Inspect both repositories and establish a concrete baseline. Then create the initial behavioral inventory and prioritize the work by compatibility risk and dependency.

Do not stop after delivering a plan. Once the initial analysis is complete, begin implementing the test harness and correcting real discrepancies, preserving the project state and recording progress in the repository.

**The final objective is simple: a drop-in-compatible fzf implementation written in C23 that preserves the original application's behavior across its full supported surface and demonstrates measurable improvements in performance, memory usage, or resource efficiency.**
