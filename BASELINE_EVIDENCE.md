# Performance Baseline Evidence

Date: 2026-10-25

> This is historical evidence from a GitHub-hosted performance workflow run
> before the workflows were disabled. It is not evidence for the current HEAD.
> Per project instruction, do not run GitHub Actions; append fresh local-only
> verification records when the relevant tests can be executed locally.

## Environment and revisions

- Performance workflow run: https://github.com/Vulto/cfzf/actions/runs/40192337634
- cfzf repository commit used for the run: `21216130be5c897a1f4712b659275804925e0fb4`
- Reference: fzf `15f64c492a08f0840b81540c7d1de35737448086`
- OS/kernel: Linux `7.0.0-1017-azure`, x86_64, GitHub-hosted runner
- C compiler: `cc (Ubuntu 15.2.0-16ubuntu1) 15.2.0`
- Go: `go1.25.14 linux/amd64`
- cfzf flags: `-O2 -Wall -Wextra -Wpedantic -Werror` and the newest compiler-supported C23 mode (`-std=c23` or `-std=c2x`)
- The tree was generated under a temporary directory and reused for both binaries. Five measured repetitions followed one warmup per binary; binary order alternated. Summary is the median and median absolute deviation (MAD).
- The walker benchmark is a CLI/PTY end-to-end measurement: time-to-first visible marker and time until the UI counter reaches the expected `N/N`. It includes startup and rendering; it is not a syscall-isolated timer. The initial tree shapes are flat directories containing only regular files. The later syscall profile is separate because `strace` perturbs timing.

## Walker results

Speedup is defined as `fzf median / cfzf median`; values above 1 mean cfzf is faster. Throughput is `file_count / full_enumeration_median_seconds`.

| Files | fzf first path (ms, median ± MAD) | cfzf first path (ms, median ± MAD) | fzf full (ms, median ± MAD) | cfzf full (ms, median ± MAD) | Speedup | fzf paths/s | cfzf paths/s | Peak RSS fzf / cfzf (KiB) |
|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 1,000 | 21.926 ± 0.178 | 6.637 ± 0.359 | 22.099 ± 0.167 | 6.637 ± 0.359 | 3.330× | 45,250 | 150,670 | 10,740 / 10,688 |
| 5,000 | 22.442 ± 0.568 | 14.199 ± 0.161 | 22.614 ± 0.519 | 20.103 ± 0.577 | 1.125× | 221,101 | 248,719 | 10,720 / 10,720 |
| 20,000 | 21.962 ± 0.347 | 41.198 ± 1.450 | 22.141 ± 0.379 | 66.976 ± 0.840 | 0.331× | 903,301 | 298,614 | 11,960 / 11,264 |

The 20,000-file case is a confirmed performance regression relative to fzf: cfzf takes 3.025× as long (202.5% more latency) and achieves 33.1% of fzf's throughput. The small case wins do not cancel this large-tree loss. RSS is similar and does not explain the latency gap by itself.

Raw walker samples (milliseconds unless the metric says `rss_kb`):

| Files | Metric | fzf samples | cfzf samples |
|---:|---|---|---|
| 1,000 | first path | 21.939, 21.926, 24.839, 21.748, 21.517 | 6.568, 6.278, 8.815, 6.637, 7.470 |
| 1,000 | full | 22.127, 22.099, 24.972, 21.933, 21.677 | 6.568, 6.278, 11.359, 6.637, 7.470 |
| 1,000 | peak RSS KiB | 10,740, 10,676, 10,676, 10,752, 10,752 | 10,740, 10,740, 7,100, 10,688, 10,688 |
| 5,000 | first path | 27.405, 22.442, 21.738, 22.760, 21.874 | 15.233, 14.199, 14.214, 14.038, 13.883 |
| 5,000 | full | 27.685, 22.614, 21.940, 22.939, 22.095 | 20.103, 19.194, 19.025, 20.584, 20.680 |
| 5,000 | peak RSS KiB | 8,836, 10,764, 10,700, 10,784, 10,720 | 7,124, 10,700, 10,764, 10,764, 10,720 |
| 20,000 | first path | 22.424, 21.962, 21.615, 22.015, 21.485 | 40.963, 39.722, 42.648, 43.547, 41.198 |
| 20,000 | full | 22.644, 22.141, 21.762, 22.206, 21.649 | 66.015, 67.816, 68.227, 66.976, 66.552 |
| 20,000 | peak RSS KiB | 11,916, 11,960, 12,008, 12,264, 11,900 | 11,112, 11,408, 11,232, 11,264, 11,312 |

The RSS samples include occasional low readings when a short-lived process exits between polling intervals; treat RSS medians as indicative, and improve the sampler before making fine-grained memory claims.

## Batch matching and startup results

Correctness was checked before timings: stdout bytes and exit status had to match for each workload. Speedup is `fzf median / cfzf median`.

| Lines | Query | fzf median (ms) ± MAD | cfzf median (ms) ± MAD | Speedup |
|---:|---|---:|---:|---:|
| 10,000 | `alpha` | 7.726 ± 0.202 | 5.041 ± 0.074 | 1.533× |
| 10,000 | `a` | 14.973 ± 0.141 | 6.635 ± 0.098 | 2.257× |
| 10,000 | `src` | 7.031 ± 0.029 | 4.350 ± 0.009 | 1.616× |
| 100,000 | `alpha` | 54.966 ± 0.688 | 43.632 ± 0.202 | 1.260× |
| 100,000 | `a` | 120.075 ± 1.619 | 62.589 ± 0.559 | 1.918× |
| 100,000 | `src` | 48.259 ± 0.970 | 36.389 ± 0.428 | 1.326× |
| 200,000 | `alpha` | 109.671 ± 3.757 | 86.102 ± 0.288 | 1.274× |
| 200,000 | `a` | 235.224 ± 4.783 | 125.542 ± 0.927 | 1.874× |
| 200,000 | `src` | 93.502 ± 1.450 | 72.333 ± 0.279 | 1.293× |
| Startup | `--version` | 2.653 | 0.878 | 3.021× |

Raw batch samples (milliseconds):

| Lines / query | fzf samples | cfzf samples |
|---|---|---|
| 10,000 / `alpha` | 7.928, 7.749, 7.365, 7.443, 7.726 | 5.334, 5.025, 5.041, 4.950, 5.114 |
| 10,000 / `a` | 14.973, 14.632, 15.074, 14.832, 15.423 | 6.635, 6.537, 6.808, 6.572, 6.909 |
| 10,000 / `src` | 7.033, 6.841, 7.031, 7.060, 6.965 | 4.350, 4.341, 4.307, 4.356, 4.366 |
| 100,000 / `alpha` | 56.565, 56.879, 54.278, 54.918, 54.966 | 43.632, 43.291, 43.430, 43.791, 44.121 |
| 100,000 / `a` | 121.489, 120.075, 118.456, 126.855, 117.752 | 63.148, 69.095, 62.589, 61.733, 62.307 |
| 100,000 / `src` | 46.044, 49.528, 48.259, 49.041, 47.288 | 35.961, 36.698, 36.389, 35.505, 38.894 |
| 200,000 / `alpha` | 105.421, 109.828, 109.671, 113.428, 104.255 | 86.102, 85.814, 86.701, 86.235, 85.425 |
| 200,000 / `a` | 230.440, 264.984, 235.215, 235.224, 244.160 | 126.973, 129.577, 125.542, 125.088, 124.615 |
| 200,000 / `src` | 88.352, 93.716, 89.618, 94.952, 93.502 | 72.333, 72.612, 72.270, 71.397, 73.195 |

## Interpretation and next action

- The current evidence does **not** support a general claim that cfzf is slower than fzf: all measured batch filters and startup favor cfzf.
- The measured gap is specific to large flat-directory traversal. Source inspection points to candidate causes—one serial traversal thread, per-entry `lstat`, per-entry name/path allocations, and two pipe writes per path—but these are hypotheses until syscall profiling and controlled changes establish causality.
- The first confirmed optimization and post-change syscall profile are recorded below. Next, buffer walker output to reduce per-path pipe syscalls, then compare before/after on the same runner and expand the workload matrix to nested trees.
- The current benchmark covers flat regular-file directories only. Nested directories, deep trees, symlinks, permission failures, and stress trees remain to be measured.


## Post-change results: avoid redundant lstat

The C source change is commit `66225533cb69522046bd20ec17d0a3534a041e3b`; the measurement and process/syscall tracing ran in [performance workflow](https://github.com/Vulto/cfzf/actions/runs/40192876310) at harness commit `25d00af04da3fd3e84032f06e3b4f97079648cc4`. This was a separate hosted-runner run, not a paired same-run A/B; use the syscall counts as direct causal evidence and the timings as indicative until a paired comparison is added.

| Files | fzf full (ms, median ± MAD) | cfzf after d_type (ms, median ± MAD) | Speedup | fzf paths/s | cfzf paths/s |
|---:|---:|---:|---:|---:|---:|
| 1,000 | 19.834 ± 0.076 | 3.560 ± 0.061 | 5.572× | 50,417 | 280,924 |
| 5,000 | 19.659 ± 0.083 | 8.391 ± 0.019 | 2.343× | 254,340 | 595,906 |
| 20,000 | 19.870 ± 0.101 | 30.245 ± 0.302 | 0.657× | 1,006,534 | 661,266 |

The 20,000-file median dropped from the earlier 66.976 ms baseline to 30.245 ms, a 2.214× improvement across separate runs. It still takes 1.522× as long as fzf in the post-change run.

Post-change raw samples (milliseconds):

| Files | Metric | fzf samples | cfzf samples |
|---:|---|---|---|
| 1,000 | first path | 20.034, 19.738, 19.658, 19.857, 19.679 | 3.560, 3.499, 3.578, 3.488, 4.820 |
| 1,000 | full | 20.135, 19.834, 19.758, 20.565, 19.778 | 3.560, 3.499, 3.578, 3.488, 6.075 |
| 5,000 | first path | 19.304, 19.568, 19.419, 19.650, 19.571 | 4.795, 4.917, 4.782, 4.802, 5.078 |
| 5,000 | full | 19.396, 19.659, 19.507, 19.742, 19.672 | 8.391, 8.428, 8.290, 8.389, 8.410 |
| 20,000 | first path | 19.790, 19.361, 19.101, 19.768, 19.810 | 11.263, 11.313, 10.038, 11.551, 11.377 |
| 20,000 | full | 19.893, 19.465, 19.194, 19.870, 19.971 | 29.943, 30.182, 30.245, 31.442, 30.578 |

### Syscall and process evidence

On the same 1,000-file flat tree:

| Metric | fzf | cfzf after d_type |
|---|---:|---:|
| `newfstatat` | 4 | 0 |
| `getdents64` | 6 | 3 |
| `write` | 5 | 2,033 |
| `read` | 34 | 1,764 |
| `openat` | 10 | 33 |
| default-path `execve` calls | 1 | 1 |
| child executable launches | 0 | 0 |

This confirms the metadata-syscall cause was removed and that default path discovery did not launch a shell or other external executable. The dominant remaining measured difference is the very high number of small pipe writes/reads. Issue #11 tracks bounded output buffering.
