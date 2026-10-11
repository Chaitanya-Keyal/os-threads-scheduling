# CS F372 Operating Systems - Assignment 2

## Group details

| Name | ID number |
|------|-----------|
| Chaitanya Keyal | 2023B4A70727H |
| Arpit Wallecha  | 2023B3A70482H |
| Vishesh Agarwal | 2023B4A70611H |
| Abhishek Verma  | 2023B4A71256H |

## Contents

Longer explanations of each problem, the concepts behind them, a demo script
and viva preparation notes are in `docs/` (start with `docs/viva-prep.md`).

| Directory         | Problem                                         | Program(s) |
|-------------------|-------------------------------------------------|------------|
| `q1_report_card/` | 1 - multithreaded student report cards          | `report_card` |
| `q2_rt_sched/`    | 2 - SCHED_RR vs SCHED_FIFO measurements         | `cpu_bound`, `io_bound`, `run_all.sh`, `results.txt` |
| `q3_sudoku/`      | 3 - Sudoku conflict analyzer and repair advisor | `sudoku_analyzer` |
| `q4_fork_guard/`  | 4 - fork storm containment and CPU tiers (kernel) | kernel patch + `tier`, `forkburst` |

Tested on Ubuntu 24.04 (VirtualBox VM, 4 cores, 4 GB) with gcc 13.3. The
kernel for Problem 4 is Linux 6.14.

## Build

```bash
make            # all user-space programs, gcc -Wall -Wextra -O2 -pthread, no warnings
make clean
make dist       # assignment2_submission.tar.gz with sources, executables and results
```

Each program can also be built by hand with the `gcc` line at the top of its
source file.

---

## Problem 1 - `report_card`

```bash
cd q1_report_card && ./report_card            # reads input.txt (or ./report_card <file>)
```

* Phase 1: one thread per student fills `students[i]` (average, min, max and
  a copy of the marks) and calls `pthread_exit`. The parent joins them all.
* Phase 2: the user enters the sort key (-3 max, -2 min, -1 avg, or a subject
  index) and k, which must divide n. k threads each insertion-sort one
  contiguous segment in place (descending, ties to the lower id). The parent
  joins them and k-way merges the segments into a new array.
* Phase 3: `1 <id>` shows a student, `2` shows the sorted order with the key
  values, `0` exits.
* No mutexes, semaphores or condition variables. Every thread writes only its
  own element or segment, and shared inputs are written before the threads
  start.

## Problem 2 - `cpu_bound`, `io_bound`

```bash
cd q2_rt_sched && sudo ./run_all.sh           # writes results.txt (about 10 min)
sudo taskset -c 0 chrt -r 50 ./cpu_bound      # one SCHED_RR run at the current quantum
sudo timeout 30 taskset -c 0 chrt -f 50 ./io_bound
```

* Both programs share `rt_bench.h` and use the given `workload_cpu.h` /
  `workload_io.h` unchanged. The parent takes a `CLOCK_MONOTONIC` reference,
  forks 8 children, and each child sends its start and completion timestamps
  through a pipe. Context switches come from `getrusage(RUSAGE_CHILDREN)`.
* `run_all.sh` sets `kernel.sched_rr_timeslice_ms` to 1, 10, 50, 150, 300 and
  500, runs both programs under SCHED_RR on CPU 0, runs them once under
  SCHED_FIFO, and restores the quantum. Each configuration runs 3 times and
  the median run is reported.
* `results.txt` holds the statistics and the observations.

## Problem 3 - `sudoku_analyzer`

```bash
cd q3_sudoku && ./sudoku_analyzer grid_invalid.txt
```

* The grid is read from `argv[1]`: digits, `0` or `.` for blanks, any
  spacing. Any count other than 81 cells is rejected.
* 27 threads validate the rows, columns and sub-grids. Each writes only
  `validity[unit]` and returns a `malloc`ed `unit_result` through
  `pthread_exit`, which the parent frees.
* For an invalid grid the program prints the validity score, a 9×9 conflict
  heatmap built by 9 block threads from the copied `dup_masks`, the hotspots
  (score 3), and a missing-digit report with repair hints.
* `valgrind --leak-check=full` is clean, and helgrind finds no races.

## Problem 4 - fork_guard and sched_tier (Linux 6.14)

See `q4_fork_guard/README.md` for the full build and test walkthrough. In
short: apply `q4_fork_guard/fork_guard.patch` to Linux 6.14. It adds
`kernel/fork_guard.c`, `kernel/sched_tier.c` and
`include/linux/fork_guard.h`, and hooks `kernel_clone()`, `sched_fork()`
and `commit_creds()`. Then build and boot the `6.14.0-forkguard` kernel and
install `tier`:

```bash
sudo dmesg -w | grep fork_guard          # root terminal
:(){ :|:& };:                            # as an ordinary user: THROTTLE, FREEZE, REAP, prompt returns
sudo tier set alice gold; tier show      # tiers: GOLD nice -7, SILVER 0, BRONZE +5
cat /proc/fork_guard /proc/sched_tier
sudo sysctl -w kernel.fg_threshold=50    # all thresholds are sysctls
```
