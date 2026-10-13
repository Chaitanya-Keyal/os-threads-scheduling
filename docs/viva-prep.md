# Viva preparation

Every member may be asked about any problem and may be asked to change code
on the spot. Read the files in this order:

1. `concepts.md`: the background for everything below (30 min)
2. `01-report-card.md` and `03-sudoku.md`: the two threading problems
3. `02-rt-scheduling.md` and `q2_rt_sched/results.txt`
4. `04-fork-guard.md`, then skim `q4_fork_guard/linux/kernel/*.c`
5. `demo.md`: run through it once on the VM
6. `modifications.md`: try one change per problem

## What every member must be able to explain

* What `pthread_create`, `pthread_join` and `pthread_exit` do, and how a
  thread returns a value to the parent.
* Why Problems 1 and 3 need no locks (partitioning; see the tables below).
* The difference between SCHED_FIFO, SCHED_RR and normal scheduling, what the
  RR quantum is, and voluntary vs involuntary context switches.
* What the Problem 2 numbers show and why (the three trends in the summary
  below).
* Where Problem 4 hooks the kernel and why there, how the ladder escalates
  and how tiers change CPU share.

## Each problem in 60 seconds

**Problem 1.** n threads, one per student, each fill `students[i]` from
column i of the marks. The user picks a key and k. k threads each
insertion-sort one contiguous segment of `students[]` in place (descending,
ties to the lower id). The parent joins them and merges the k sorted segments
by repeatedly taking the best head. A menu then answers queries. The data is
partitioned, so there are no locks.

**Problem 2.** Two programs fork 8 children each, pinned to one core under
SCHED_RR or SCHED_FIFO. Children send start and end timestamps through a
pipe. The parent computes response and turnaround against its own reference
timestamp, and gets context switches from `getrusage(RUSAGE_CHILDREN)`. A
script sweeps the RR quantum from 1 to 500 ms. CPU-bound: switches fall and
response time rises with the quantum, turnaround stays flat, FIFO serialises.
I/O-bound: the quantum barely matters, because the children block on fsync
first.

**Problem 3.** 27 threads validate the rows, columns and boxes. Each counts
digits, writes `validity[own id]` and returns a `malloc`ed result with
missing and duplicate bitmasks. If the grid is invalid, the parent copies the
dup masks to a global, and 9 block threads score each cell (0-3 by
duplication in its row, column and box; a blank scores 3). The parent prints
the score, heatmap, hotspots and missing digits, plus a repair hint when a
unit has exactly one blank and one missing digit. Everything is freed:
valgrind is clean.

**Problem 4.** `kernel_clone()` calls `fg_check_fork()`, which counts each
non-root user's forks in a sliding window. Over the threshold: THROTTLE
(EAGAIN plus a delay), then FREEZE (SIGSTOP the group), then REAP (SIGKILL
the group) if the rate stays high. A user is forgiven after a cooldown.
Everything is a sysctl and is logged to dmesg. Tiers: root writes
`/proc/sched_tier` (through `tier`), and the kernel re-nices every task of
that user (GOLD -7, SILVER 0, BRONZE +5), which changes their CFS weight
immediately. `sched_fork()` gives new tasks the tier, and `commit_creds()`
applies it when a task switches user.
Containment never looks at tiers.

## Why each shared array is safe (asked in the viva)

| Array | Problem | Who writes | Who reads | Why safe |
|-------|---------|-----------|-----------|----------|
| `marks` | 1 | parent, before threads | Phase 1 threads | read-only while threads run |
| `students[i]` | 1 | Phase 1 thread i only | later phases | one writer per element |
| `students[seg]` | 1 | sort thread t, its own segment only | parent after join | segments are disjoint |
| `sort_key` | 1 | parent, before sort threads | sorters, merge | read-only while threads run |
| `sorted` | 1 | parent, after joins | menu | no threads alive |
| `grid` | 3 | parent, before threads | all threads | read-only |
| `validity[27]` | 3 | thread u writes index u | parent after join | one writer per element |
| `dup_masks[27]` | 3 | parent, before block threads | block threads | read-only while threads run |
| `heatmap[9][9]` | 3 | block thread b, its 9 cells | parent after join | the blocks partition the grid |

`pthread_create` guarantees the new thread sees everything the parent wrote
before it. `pthread_join` guarantees the parent sees everything the thread
wrote. Helgrind (`valgrind --tool=helgrind`) reports 0 errors for both
programs.

## Likely questions with answers

### Threads (Problems 1 and 3)

* **Why pass `&ids[i]` and not `&i`?** `i` keeps changing in the loop, and
  the thread could read it after it has moved on. Each thread needs its own
  memory that lives until it is joined.
* **Why must the result be `malloc`ed?** The thread's stack is gone after it
  exits. Heap memory survives until the parent `free`s it.
* **What if a thread is not joined?** Its resources (stack, exit status) are
  not released until the process ends, like a zombie process. And the parent
  could not know the thread had finished writing.
* **Could the k sort threads share one array without locks?** Yes, because
  each touches only its own contiguous range. The merge reads all ranges but
  only after every sorter is joined.
* **Why is the k-way merge O(n·k)?** For each of the n outputs it scans k
  heads. A min/max heap of the k heads would make it O(n log k). With
  n ≤ 128 the simple scan is clearer.
* **Why must k divide n?** The problem lets us assume it. The code checks
  and asks again. Supporting any k only needs the last segment to take the
  remainder (see `modifications.md`).
* **Why is the heatmap per block and not per row?** The problem asks for it.
  A block is a natural partition of the 81 cells, so each thread writes 9
  cells nobody else writes.
* **Why copy `dup_mask` into a global array?** The block threads need the
  masks of the row, column and box of each cell. Copying them before the
  threads start makes the array read-only for them, so no synchronisation is
  needed.

### Scheduling (Problem 2)

* **Why pin to one core?** On several cores the children would run in
  parallel and rarely preempt each other, so the quantum would have no
  visible effect.
* **Why does a 1 ms quantum give about 7000 involuntary switches?** About
  8 s of total CPU work divided by 1 ms slices. A larger quantum means fewer
  slices.
* **Why are voluntary switches 8 for the CPU-bound program?** Each child
  blocks once, on its final pipe write and exit. CPU-bound work never sleeps.
* **Why doesn't throughput change with the quantum?** The total CPU work is
  fixed and only one core runs it. The quantum only changes the order in
  which the work is done.
* **Why does FIFO have the best average turnaround?** Without time slicing,
  child 1 finishes after about 1 s, child 2 after about 2 s, and so on.
  Under RR all of them finish near the end.
* **Why are there still about 30 involuntary switches under FIFO?** RT
  throttling: real-time tasks get at most 950 ms per second
  (`sched_rt_runtime_us`), and the remaining 50 ms preempts them.
* **Why does the I/O-bound program ignore the quantum?** Each child blocks on
  `fsync` long before using up even 1 ms, so the quantum timer never fires.
  Elapsed time is set by the disk.
* **What happens when `timeout 30` fires?** `timeout` sends SIGTERM to the
  process group. The children die. The parent has an empty handler (installed
  after forking, so the children do not inherit it), which makes its `read()`
  return. It reaps the children and prints what it has, with
  `(stopped early: N of 8 children finished)`.
* **Why `CLOCK_MONOTONIC`?** It never jumps (no NTP or `date` changes), and
  it is the same clock in every process, so timestamps from different
  processes can be subtracted.
* **How does `getrusage(RUSAGE_CHILDREN)` know about the children?** The
  kernel adds a child's counters to the parent when the child is reaped by
  `wait`. That is why the parent waits for all children first.
* **Why the median of 3 runs?** VirtualBox shares the CPU with Windows, and
  identical runs differ by about 20%. The median removes outliers without
  inventing data.

### Kernel (Problem 4)

* **Why hook `kernel_clone()`?** Every fork, vfork, clone and clone3 passes
  through it, before the child is created. Refusing there costs almost
  nothing and gives user space the normal `EAGAIN`.
* **Why not count threads?** `CLONE_THREAD` adds a thread to an existing
  process rather than creating a process. The problem is about process
  creation.
* **Why per user and not per process?** A fork bomb is thousands of
  processes. Only the user (uid) ties them together.
* **How is the sliding window implemented?** 10 time slots per window, each
  with a counter and the slot number it belongs to. The count is the sum of
  the slots that are still inside the window. Old slots expire on their own.
* **Why FREEZE before REAP?** The problem wants a graduated response: a
  stopped group can be inspected (`ps` shows state T) before it is killed.
* **How does REAP happen if frozen processes cannot fork?** A delayed work
  item re-checks the user's rate `fg_escalate_ms` after the freeze. If the
  window is still over the threshold, it kills the group.
* **Why doesn't the shell die?** The bomb runs as a background job in its own
  process group. The shell is the leader of a different group, and group
  leaders never trigger the freeze. After REAP, the user's counters are reset
  so the shell's next command is not punished.
* **Why is root exempt?** So an administrator can always fork to recover the
  system (spec requirement).
* **Can a GOLD user escape containment?** No. `fg_check_fork()` never reads
  the tier table. Test 6 shows the identical ladder.
* **How is a running process re-tiered without a restart?** `set_user_nice()`
  changes the task's priority and reweights it in the run queue immediately.
  `st_apply()` calls it on every thread of the user.
* **How do children inherit the tier?** `sched_fork()` calls
  `st_fork_nice()`, so a new task of a tiered user starts at the tier's nice
  value and load weight, even if its parent was reniced. A process that
  switches to the user (`sudo -u alice`) gets the tier in `commit_creds()`.
* **`sched_fork()` only runs when a task is created. How do you affect
  running tasks?** `tier set` walks every thread of the user and calls
  `set_user_nice()`, which reweights each one in its run queue at once.
* **Why kill the process group and not one process?** A fork bomb is
  hundreds of processes that keep replacing each other. Killing one does
  nothing, but they all share one process group, so one `kill_pgrp()`
  reaches every copy, including those being forked at that moment.
* **In what order do you signal a stopped group?** Just SIGKILL. It is the
  one signal that wakes stopped tasks, so they die without resuming. SIGCONT
  first would let them fork again before dying. SIGCONT is only used to thaw
  a group that is forgiven.
* **Why 6.14 and not 6.8 like the help document?** The assignment asks for
  any 6.x kernel, and 6.14 is the tree from assignment 1. The only API
  difference is the sysctl table (const, and no empty sentinel in 6.14).
* **Why `spin_lock()` and not `spin_lock_irqsave()`?** The locks are never
  taken from interrupt context, only from the fork path, `/proc` and a
  workqueue item, so interrupts never need to be disabled.
* **Justify the clamp.** Nice -7..+5 means weights 4904..335. GOLD vs BRONZE
  is about 15:1, so BRONZE still gets about 6% of the core. Full -20..+19
  would be about 5900:1, which is effectively starvation.
* **Why might tiers show no effect?** If the two users' processes are in
  different cgroups (separate logins), the cgroups share the CPU equally and
  nice only matters inside a group. In the demo both are started with
  `sudo -u` from one root shell, so they are in the same group.
* **What locks are used, and why spinlocks?** `fg_lock` protects the user
  table and `st_lock` protects the tier table. The critical sections are
  short and never sleep. Signals and sleeps happen after unlocking.
* **How are sysctls protected from bad values?** `proc_dointvec_minmax` with
  `extra1`/`extra2` bounds rejects them.
* **What happens if 65 users fork at once?** The table holds 64 users. Users
  beyond that are not tracked. That is a documented limit and a single
  constant.
