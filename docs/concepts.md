# Concepts used in this assignment

## Threads and pthreads

A thread is a separate flow of execution inside one process. All threads of a
process share its memory (globals, heap, open files). Each has its own stack
and registers. Creating a thread is much cheaper than `fork()` because nothing
is copied.

```c
pthread_t tid;
pthread_create(&tid, NULL, worker, arg);   /* start worker(arg) in a new thread */
pthread_join(tid, &ret);                   /* wait for it, collect its return value */
```

* `pthread_exit(p)` ends the calling thread. `p` is what `pthread_join`
  receives in `ret`. Returning `p` from the thread function does the same.
* The value must not point to the thread's stack, which disappears when the
  thread exits. Problem 3 therefore `malloc`s its `unit_result`, and the
  parent frees it after joining.
* Each thread's argument must stay valid while the thread runs. A pointer to a
  loop variable would change under the thread. Problems 1 and 3 give each
  thread its own array element (`&ids[i]`, `&units[u]`).
* Programs are built with `-pthread`.

## Sharing data without locks

Two threads race when both touch the same memory, at least one of them writes,
and nothing orders the accesses. Locks are one fix. Problems 1 and 3 forbid
them and use **partitioning** instead:

1. **Disjoint writes.** Each thread writes only its own slice: `students[i]`,
   one segment, `validity[u]`, or the 9 cells of one block. Two threads never
   write the same element.
2. **Read-only sharing.** Data many threads read (`marks`, `grid`,
   `dup_masks`, `sort_key`) is written by the parent *before* those threads
   are created and is never changed while they run.
3. **create/join order everything else.** Everything the parent wrote before
   `pthread_create` is visible to the new thread. Everything a thread wrote is
   visible to the parent after `pthread_join` returns. POSIX guarantees both.

`valgrind --tool=helgrind` checks for races at run time. It reports none for
either program.

## Scheduling classes and policies

Linux picks the next task from a stack of scheduling classes. A runnable task
of a higher class always runs before any task of a lower class:

| Class | Policies | Used by |
|-------|----------|---------|
| deadline | `SCHED_DEADLINE` | (not used) |
| real-time | `SCHED_FIFO`, `SCHED_RR`, priority 1-99 | Problem 2 |
| fair | `SCHED_OTHER` (normal), `SCHED_BATCH`, `SCHED_IDLE` | everything else, Problem 4 |

**SCHED_FIFO.** The highest-priority runnable task runs until it blocks,
exits or yields. Tasks of equal priority are not time-sliced: a CPU-bound
FIFO task keeps the core until it finishes.

**SCHED_RR.** Like FIFO, but a task that uses up its **time quantum** goes
to the back of the queue for its priority, so equal-priority tasks take turns.
The quantum is the sysctl `kernel.sched_rr_timeslice_ms` (default 100 ms). It
is counted in scheduler ticks, so with `CONFIG_HZ=1000` (the VM) one tick is
1 ms and every value from 1 ms upward is exact.

**RT throttling.** By default real-time tasks may use at most 950 ms of every
1000 ms (`kernel.sched_rt_runtime_us` / `sched_rt_period_us`). The rest is left
for normal tasks, so a runaway RT task cannot lock up the machine. It shows up
in Problem 2 as a few extra involuntary switches per second.

`chrt -r 50 prog` / `chrt -f 50 prog` start a program under RR/FIFO at
priority 50. `taskset -c 0` pins it to CPU 0. Both settings are inherited
across `fork()`, so all 8 children compete for the same core.

## The fair scheduler, nice and weights

Normal tasks are scheduled by the fair class. It was CFS until Linux 6.6 and
EEVDF since then, but the idea is the same: each task has a **weight**, and on
a busy CPU it gets CPU time in proportion to its weight. The weight comes from
the nice value through the table `sched_prio_to_weight[]`. Each nice step is
about 1.25× (10% of the CPU between two tasks):

| nice | -7 | 0 | 5 | 19 | -20 |
|------|----|---|---|----|-----|
| weight | 4904 | 1024 | 335 | 15 | 88761 |

Two tasks at nice 0 share 50/50. At nice -7 against nice 0 the split is
4904 : 1024 = 83% / 17%. `set_user_nice(task, n)` changes a running task's
nice value and reweights it at once. Problem 4's tiers are built on this.

**Groups.** Weights only compete between tasks in the same group. With cgroup
v2 and the `cpu` controller, each cgroup is a scheduling entity with its own
weight. With autogroup (`kernel.sched_autogroup_enabled`), each session gets
its own group when tasks are not in a cpu cgroup. So two tasks only compete by
nice value if they are in the same group. For Problem 4's demo both `yes`
processes are started with `sudo -u` from the same root shell, which keeps
them in the same group (see `04-fork-guard.md`).

## Context switches and getrusage

A context switch is the kernel saving one task's registers and loading
another's.

* **Voluntary:** the task gave up the CPU itself by blocking (on I/O, a pipe,
  `fsync`, sleep).
* **Involuntary:** the task was still runnable but was preempted, because its
  quantum ran out, a higher-priority task woke up or RT throttling kicked in.

`getrusage(RUSAGE_CHILDREN, &ru)` returns the totals over all children the
caller has **waited for**: `ru_nvcsw` (voluntary) and `ru_nivcsw`
(involuntary). Problem 2's parent therefore reaps all 8 children first.

## Timing: CLOCK_MONOTONIC

`clock_gettime(CLOCK_MONOTONIC, &ts)` reads a clock that only moves forward
and is not changed by NTP or `date`. It is shared by all processes, so a
child's timestamp can be subtracted from the parent's reference timestamp.
Problem 2 does exactly that:

* response time = first instruction of the child − parent's reference,
* turnaround time = child's last instruction − parent's reference.

## Process groups and signals

Every process belongs to a **process group** (pgid), and groups belong to a
**session**. An interactive shell is a session leader in its own group. Each
job it starts (a pipeline, a `&` background command) gets a new group. Every
process a job forks stays in that group unless it calls `setpgid`.

`kill(-pgid, sig)` in user space, or `kill_pgrp(pid, sig, 1)` in the kernel,
signals every process in a group at once. That makes a group the natural unit
for containing a fork bomb: the bomb `:(){ :|:& };:` runs as one background
job, so all its copies share one pgid, while the user's shell is in a
different group and survives.

* `SIGSTOP` suspends every process in the group. It cannot be caught or
  ignored. The processes stay in the process table and can be inspected
  (`ps -o pid,stat,cmd -g <pgid>` shows state `T`).
* `SIGKILL` ends them. It also works on stopped processes.
* `SIGCONT` resumes stopped processes.

## Inside fork(): kernel_clone

`fork()`, `vfork()`, `clone()` and `clone3()` all end up in `kernel_clone()`
in `kernel/fork.c`, which calls `copy_process()` to build the child. Kernel
threads are created through the same function. A check placed at the top of
`kernel_clone()` therefore sees every process creation exactly once and can
refuse it by returning a negative errno. `fork()` then returns -1 with
`errno = EAGAIN`, the same error as hitting `ulimit -u`.

Threads are `clone(CLONE_THREAD)`. They share the parent's process and are
not new processes, so Problem 4 does not count them.

## Credentials: commit_creds

A task's user and group ids live in a `struct cred`. Every id change
(`setuid`, `setresuid`, `su`, `sudo`, `login`, a setuid binary) builds a new
`cred` and installs it with `commit_creds()` in `kernel/cred.c`. That makes it
the single place to notice "this task now belongs to another user". Problem 4
uses it so that `sudo -u alice prog` gets alice's tier.

## procfs and sysctl

* **/proc files** are generated by the kernel when they are read. A
  `seq_file` show function prints the current state. `proc_create_single()`
  registers a read-only file. `proc_create()` with a `proc_ops` that has a
  `.proc_write` handler accepts commands written to the file.
* **sysctl** exposes kernel variables under `/proc/sys`. A `ctl_table` entry
  points at an `int` and uses `proc_dointvec_minmax` with `extra1`/`extra2`
  bounds, so out-of-range writes are rejected. `register_sysctl("kernel", ...)`
  puts them in `/proc/sys/kernel/`, readable and writable with
  `sysctl kernel.fg_threshold` and `sysctl -w kernel.fg_threshold=50`. The
  change takes effect at once, without rebuilding.

## Kernel logging

`pr_info()`, `pr_warn()` and `pr_alert()` write a line to the kernel ring
buffer at different log levels, and `dmesg` prints them (`dmesg -w` follows
it live, and colours warnings and alerts). Problem 4 logs every stage change
there.
