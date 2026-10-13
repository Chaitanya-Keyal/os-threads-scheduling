# Problem 2: Real-time scheduling, Round-Robin vs FIFO

## The task

Two programs each fork 8 children. Program A's children run the CPU-bound
`cpu_task()` from `workload_cpu.h`. Program B's children run the I/O-bound
`io_task()` from `workload_io.h` (2000 × write 4 KB + `fsync`). Both headers
are used exactly as given.

Each program is run pinned to CPU 0:

* under `SCHED_RR` with the RR quantum (`kernel.sched_rr_timeslice_ms`) set to
  1, 10, 50, 150, 300 and 500 ms,
* once under `SCHED_FIFO`, capped by `timeout 30`.

Each run reports involuntary/voluntary context switches, average/maximum
response time, average/maximum turnaround time, throughput and total elapsed
time. The results go in `q2_rt_sched/results.txt`.

## The code

`rt_bench.h` holds everything the two programs share. `cpu_bound.c` and
`io_bound.c` are each one line: `return run_bench(cpu_task);` /
`run_bench(io_task)`.

`run_bench()`:

1. Creates a pipe, then takes the **reference timestamp**
   (`clock_gettime(CLOCK_MONOTONIC)`), then forks 8 children.
2. **Child:** its very first action is to read its start time. It then runs
   the task, reads its end time, writes `{start, end}` to the pipe in one
   `write()` (smaller than `PIPE_BUF`, so it is atomic and reports never mix),
   and calls `_exit(0)`.
3. **Parent:** closes its write end and reads reports until `read()` returns 0,
   which happens when every child has exited. For each report it computes
   *response = start − reference* and *turnaround = end − reference* and
   keeps sums and maxima.
4. Reaps every child with `wait()`, takes the end timestamp, and calls
   `getrusage(RUSAGE_CHILDREN)`. The counters only include children that have
   been waited for, which is why the reaping comes first.
5. Prints the block in the sample format. The header comes from the kernel:
   `sched_getscheduler(0)` gives the policy, and `sched_rr_get_interval()`
   gives the quantum actually in force.

The scheduling policy is not set in C. `chrt` sets it on the parent, and
`fork()` copies the policy, priority and CPU affinity to every child.

**When `timeout 30` fires.** `timeout` sends SIGTERM to the whole process
group. The children die, as intended. The parent installs an empty SIGTERM
handler *after* forking, so only the parent has it. The signal makes its
blocked `read()` return, and it then reaps the children and prints the block
for the children that had finished, ending with a line such as
`(stopped early: 0 of 8 children finished)`. Without the handler a capped run
would print nothing. On a slow-disk day the I/O-bound FIFO run takes close to
30 s on the VM, so this can happen in the demo.

## Running it

```bash
make
cd q2_rt_sched && sudo ./run_all.sh     # about 10 minutes
```

`run_all.sh` sets each quantum with `sysctl -w`, runs
`taskset -c 0 chrt -r 50 ./prog`, then runs
`timeout 30 taskset -c 0 chrt -f 50 ./prog`, and does this for both
programs. It restores the old quantum on exit. It regenerates the data part
of `results.txt`; the observations at the end of the file were written by
hand.

Timings inside the VirtualBox VM vary by about ±20% between identical runs,
because the Windows host shares the CPU with the VM. The script therefore runs
every configuration 3 times and keeps the run with the median elapsed time.
`RUNS=1` gives single runs.

A single run, as in the problem statement:

```bash
sudo sysctl -w kernel.sched_rr_timeslice_ms=50
sudo taskset -c 0 chrt -r 50 ./cpu_bound
sudo timeout 30 taskset -c 0 chrt -f 50 ./cpu_bound
sudo sysctl -w kernel.sched_rr_timeslice_ms=100     # the default
```

## Reading the results

The numbers below are from `results.txt` (kernel 6.14, HZ=1000, 4 vCPUs, all
on CPU 0).

### Program A: CPU-bound

| quantum | invol. | resp. avg / max (ms) | turnaround avg (ms) | elapsed (s) |
|---------|--------|----------------------|---------------------|-------------|
| 1 ms    | 6949   | 30 / 50              | 8572                | 8.69 |
| 10 ms   | 723    | 44 / 82              | 8267                | 8.34 |
| 50 ms   | 158    | 190 / 383            | 8187                | 8.44 |
| 150 ms  | 61     | 607 / 1235           | 8770                | 9.47 |
| 300 ms  | 37     | 1189 / 2497          | 7873                | 8.85 |
| 500 ms  | 34     | 2570 / 5048          | 8241                | 10.05 |
| FIFO    | 30     | 4450 / 8692          | 5705                | 10.05 |

* **Context switches ≈ total CPU time / quantum.** The 8 children need about
  8 s of CPU in total. At 1 ms that is about 8000 slices (6949 measured), at
  10 ms about 800 (723), at 50 ms about 160 (158). Voluntary switches are
  always 8, because a child blocks only when it writes its report and exits.
* **The floor of about 30** at large quanta and under FIFO comes from **RT
  throttling**. Real-time tasks may use only 950 ms of every 1000 ms, so
  roughly once a second the running child is preempted for normal tasks. FIFO
  has no quantum at all, and these are its only involuntary switches.
* **Response time grows with the quantum.** The parent forks all 8 children
  before it blocks on the pipe, and they queue behind each other. Child i
  first runs after the i children ahead of it have each used one quantum, so
  the maximum is about 7 quanta and the average about 3.5 quanta. The measured
  values are a little higher because wall-clock time also includes RT
  throttling and host steal time.
* **Turnaround and throughput hardly change.** The CPU does the same total
  work whatever the quantum. With round-robin on one core every child is
  still running until the very end, so all of them finish at about 8 s.
  Tiny quanta add a little switching overhead. The quantum mostly trades
  responsiveness for switch count.
* **FIFO** runs the children one after another: the worst response time (the
  last child waits for 7 others, 8.7 s) but the best average turnaround
  (5.7 s), because the early children finish early instead of all finishing
  together. Throughput is the same.

### Program B: I/O-bound

| quantum | invol. | vol. | resp. avg (ms) | elapsed (s) |
|---------|--------|------|----------------|-------------|
| 1 ms    | 1015   | 57245 | 2.7 | 15.08 |
| 10 ms   | 120    | 56826 | 2.2 | 14.92 |
| 50 ms   | 29     | 55265 | 2.1 | 17.24 |
| 150-500 ms | 8-18 | ~55000 | 2-7 | 16.4-18.3 |
| FIFO    | 11     | 55304 | 6.5 | 18.15 |

* About **55,000 voluntary switches** for 16,000 write+fsync pairs: each
  `fsync` waits for the disk, sometimes more than once. The children block
  long before any quantum runs out, so **the quantum does not matter** and
  RR and FIFO behave the same.
* Involuntary switches are only noticeable at 1 ms, where a child is
  occasionally preempted between waking up from one fsync and blocking on the
  next.
* Response time is a few milliseconds everywhere, because each child blocks
  almost at once and lets the next one start.
* Elapsed time (15-18 s) is set by the virtual disk. The run-to-run
  differences are disk noise, not a scheduling effect.

### Summary

| | CPU-bound | I/O-bound |
|-|-----------|-----------|
| smaller quantum | many more involuntary switches, much better response time | almost no effect |
| larger quantum | fewer switches, response approaches FIFO's | almost no effect |
| FIFO | worst response, best average turnaround | same as RR |
| throughput | constant (CPU limited) | constant (disk limited) |

The quantum only matters when tasks actually use it up, which is the
CPU-bound case. A quantum that is short compared with the job length gives
interactive-feeling response at the cost of switching. A long quantum turns
RR into FIFO.
