# Demo script

Start the VirtualBox VM `cs-f372-ubuntu`. It boots `6.14.0-forkguard` by
default. Log in from WSL with `ssh osvm`, or at the VM console. Then:

```bash
cd ~/assignment-2 && make
```

## Problem 1

```bash
cd q1_report_card && ./report_card
```

Key `-1`, k `3`, then `1 2`, `2`, `0`. This is the sample output from the
problem statement. Run it again with key `-3` and k `6`, then show an invalid
k (`4` does not divide 6) and an invalid id (`1 9`).

Open the code at `aggregate()` (writes only `students[i]`), `sort_segment()`
(only its segment), `before()` (the shared tie rule) and `merge_segments()`.

## Problem 2

```bash
cd ../q2_rt_sched
cat results.txt                                   # the full measurements + observations
sudo sysctl -w kernel.sched_rr_timeslice_ms=10
sudo taskset -c 0 chrt -r 50 ./cpu_bound          # ~720 involuntary switches
sudo sysctl -w kernel.sched_rr_timeslice_ms=500
sudo taskset -c 0 chrt -r 50 ./cpu_bound          # ~35, much higher response time
sudo timeout 30 taskset -c 0 chrt -f 50 ./cpu_bound
sudo sysctl -w kernel.sched_rr_timeslice_ms=100   # back to the default
```

Each run takes about 10 s. Point out how involuntary switches and response
time move with the quantum, and that FIFO has the worst response time but
the best average turnaround. `sudo ./run_all.sh` redoes everything in about
10 minutes, so only run it if asked.

## Problem 3

```bash
cd ../q3_sudoku
./sudoku_analyzer grid_valid.txt
./sudoku_analyzer grid_invalid.txt      # the sample run
./sudoku_analyzer grid_blanks.txt       # '.' blanks, repair hints
./sudoku_analyzer grid_short.txt        # 80 cells -> diagnostic
valgrind --leak-check=full ./sudoku_analyzer grid_invalid.txt   # "All heap blocks were freed"
```

## Problem 4

Open three terminals:

* **root:** `ssh osvm`, then `sudo -i`
* **log:** `ssh osvm`, then `sudo dmesg -w | grep -E "fork_guard|sched_tier"`
* **alice:** `ssh alice@<vm>` (password `alice`), e.g.
  `ssh -p 2222 alice@172.20.96.1` from WSL

```bash
uname -r                                       # 6.14.0-forkguard
dmesg | grep -E "fork_guard|sched_tier" | head -2   # initialised lines
```

**Tiers (tests 1-4), in the root terminal:**

```bash
tier show                                      # (no user is tiered; everyone is SILVER)
sudo -u alice taskset -c 0 yes > /dev/null &
sudo -u bob   taskset -c 0 yes > /dev/null &
top                                            # ~50/50, press q
tier set alice gold                            # top: alice NI -7, ~83%
ps -u alice -o pid,ni,pcpu,etime,comm          # same PID, ELAPSED kept growing: not restarted
tier set bob bronze                            # ~94/6, bob's TIME+ still increasing
tier show
tier reset                                     # back to ~50/50
kill %1 %2
```

**Containment (tests 5-7):**

```bash
# root (optional, second view of the storm)
forkstat
# alice
:(){ :|:& };:
# log shows THROTTLE, FREEZE, REAP within 0.5-2 s;
# alice's prompt keeps working: type `echo alive`
cat /proc/fork_guard                           # counters went up by 1

tier set alice gold                            # test 6: repeat the bomb, same ladder
sysctl -w kernel.fg_threshold=50 kernel.fg_window_ms=500    # test 7
# repeat the bomb: THROTTLE says ">50 forks in 500 ms"
sysctl -w kernel.fg_threshold=200 kernel.fg_window_ms=1000
tier reset
```

**Graduated, not just a kill switch:** as alice run `forkburst`. One THROTTLE line appears,
no FREEZE. The program backs off and finishes all 300 forks. After 10 s,
`/proc/fork_guard` shows stage 0 again.

**Observability (test 8):** `cat /proc/fork_guard`, `cat /proc/sched_tier`,
`tier show`, `sysctl -a | grep fg_`.

Source on request: `~/linux-6.14-fg/kernel/fork_guard.c`, `sched_tier.c`,
and `grep -n fg_check_fork ~/linux-6.14-fg/kernel/fork.c`.

## If something goes wrong

* A frozen group that was not reaped (only if the settings were changed):
  `ps -eo pid,pgid,stat,user,cmd | grep " T"` shows it. `kill -CONT -<pgid>`
  or `kill -KILL -<pgid>`.
* The VM can still boot `6.14.0-setnice` or the stock Ubuntu kernel from GRUB
  under "Advanced options for Ubuntu". The VirtualBox snapshot `pre-forkguard`
  is the state just before this kernel was installed.
