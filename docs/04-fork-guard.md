# Problem 4: Fork storm containment and tiered CPU scheduling

## The task

Modify Linux 6.x so that:

1. **Fork storms are contained gradually, per user.** When a user creates
   processes faster than a threshold, the kernel first refuses forks
   (THROTTLE), then stops the offending process group (FREEZE), then kills it
   (REAP). A user who calms down is forgiven after a cooldown. Root is exempt.
   Everything is logged to dmesg, and every threshold is a sysctl.
2. **Users get CPU tiers.** GOLD (at most one user), SILVER (the default) and
   BRONZE change a user's CPU share through the CFS weight. A change applies
   to running processes at once, children inherit it, and BRONZE must never
   be starved. Only root assigns tiers, with `tier set/reset/show`.
3. **Both are observable** through `/proc`, and a tier never weakens
   containment.

Source: `q4_fork_guard/linux/` (about 610 lines), `fork_guard.patch`, and the
`tier` and `forkburst` tools.

## Where the kernel is hooked

| Hook | File / function | Why there |
|------|-----------------|-----------|
| `fg_check_fork(args)` | `kernel/fork.c`, top of `kernel_clone()` | fork, vfork, clone and clone3 all pass through `kernel_clone()`, so this sees every process creation once, before any work is done. Returning `-EAGAIN` makes `fork()` fail exactly as `ulimit -u` would. |
| `st_fork_nice(p, &nice)` | `kernel/sched/core.c`, `sched_fork()` | every new task passes through here before it first runs, so a tiered user's child starts at the tier's nice value and load weight |
| `st_cred_changed(task, old, new)` | `kernel/cred.c`, end of `commit_creds()` | every uid change (sudo, su, login, setuid) installs new credentials here, so a task that becomes alice gets alice's tier |

The first two are the hook points named in the Q4 help document.
`sched_fork()` is called from `copy_process()`, which runs after the
`kernel_clone()` check, so containment always runs first and never depends on
the tier code. The `commit_creds()` hook is our addition. `sched_fork()` only
sees the uid the task has at fork time, and `sudo -u alice yes` is forked as
root and becomes alice afterwards.

Everything else lives in two new files, `kernel/fork_guard.c` and
`kernel/sched_tier.c`, which are always built in (`obj-y`).

## Mechanism 1: fork_guard

### Counting

Each tracked user has a slot in a fixed table of 64 (`struct fg_user`), under
one spinlock. Fork attempts are counted in a **sliding window** of
`fg_window_ms`, split into 10 time slots: a counter per slot, plus the slot
number it belongs to. Old slots fall out of the sum automatically as time
passes, so "forks in the last second" is always current and costs 10
additions.

The check returns 0 at once, without counting, for threads
(`CLONE_THREAD`), kernel threads (`PF_KTHREAD`), root (uid 0) and when
`fg_enabled` is 0. Attempts are counted, not successes. A bomb that keeps
retrying while throttled therefore stays over the threshold, and a program
that backs off drops below it.

### The ladder

An attempt that pushes the window count over `fg_threshold` is a
**violation**:

| From | Condition | Action | Log |
|------|-----------|--------|-----|
| stage 0 | violation | stage 1, refuse with `-EAGAIN` after sleeping `fg_throttle_ms` | `THROTTLE uid=1001 pgid=3412 (>200 forks in 1000 ms)` |
| stage 1 | violation, at least `fg_escalate_ms` after the throttle, by a task that is not its group's leader | stage 2, `kill_pgrp(group, SIGSTOP)`, start a delayed work item | `FREEZE uid=1001 pgid=3412 (SIGSTOP process group)` |
| stage 2 | `fg_escalate_ms` later, window still over threshold | `kill_pgrp(group, SIGKILL)`, clear the user's window, back to stage 0 | `REAP uid=1001 pgid=3412 (SIGKILL process group)` |
| stage 1 | no violation for `fg_cooldown_s` | back to stage 0 | `FORGIVE uid=...` |
| stage 2 | window dropped below threshold and no violation for `fg_cooldown_s` | `SIGCONT` the group, back to stage 0 | `FORGIVE ... (SIGCONT ...)` |

In any stage ≥ 1, every over-threshold attempt is refused. Attempts under the
threshold are allowed again, so a bursty workload that pauses keeps working.

**Why the REAP decision runs in a delayed work item.** Once a group is
stopped, its processes cannot fork, so no fork attempt will ever arrive to
trigger stage 3. The delayed work re-checks the user's window
`fg_escalate_ms` after the freeze. With the default 1000 ms window the
storm's attempts are still in it, so the group is reaped. If someone changed
the settings so the window had emptied, the group would instead be resumed
once the cooldown passes. Stopped processes are never left behind.

**Why the user goes back to stage 0 after REAP.** The offending group is gone.
If the user stayed at stage 2, the next ordinary command from alice's shell
(a fork while the old window is still full) would be refused or escalated,
which would break "restore shell control to the user".

**Why a group leader never triggers the freeze.** The fork bomb
`:(){ :|:& };:` typed in a shell runs as one background job, in one process
group (all its copies share one pgid). The shell itself is in a different
group, which it leads. If alice's shell happened to start a command during
the storm, freezing *its* group would stop the shell. Only non-leaders can
trigger the freeze, and a bomb has hundreds of them. The shell is safe, and
the bomb is still caught at the next attempt by one of its members.

**Why SIGKILL alone is enough for a stopped group.** SIGKILL is the one
signal that wakes a stopped task, so the group dies without a SIGCONT first.
Sending SIGCONT first would be wrong: the processes would resume and fork
again for a moment before the kill arrives. SIGCONT is only used when a
frozen group is forgiven instead of reaped.

**Why the signals are sent outside the spinlock.** `kill_pgrp()` walks the
group's task list and takes other locks, and `msleep_interruptible()` sleeps.
Neither may run while holding a spinlock. The code records what to do under
the lock and acts after `spin_unlock()`. It holds a reference
(`get_pid`/`put_pid`) to the group so the `struct pid` cannot disappear in
between.

**The penalty sleep** slows each refused attempt by `fg_throttle_ms`.
`msleep_interruptible` ends early if a signal arrives, so a SIGSTOP or SIGKILL
for the group takes effect at once.

### Tunables

`register_sysctl("kernel", fg_sysctls)` creates
`/proc/sys/kernel/fg_{enabled,threshold,window_ms,throttle_ms,cooldown_s,escalate_ms}`.
Each entry uses `proc_dointvec_minmax` with bounds: threshold ≥ 1,
window 10 ms to 60 s, throttle ≤ 1 s, cooldown ≤ 1 h. A nonsensical value is
rejected with `EINVAL`. A changed window length is detected per user and
restarts that user's count.

## Mechanism 2: sched_tier

### How the bias works

A tier is just a nice value applied to every task of the user:

| Tier | nice | CFS weight | vs one SILVER task | vs one GOLD task |
|------|------|-----------|-------------------|------------------|
| GOLD | -7 | 4904 | 83% / 17% | - |
| SILVER | 0 | 1024 | 50% / 50% | 17% |
| BRONZE | +5 | 335 | 25% / 75% | **6%** |

The fair scheduler gives each runnable task CPU time in proportion to its
weight, so changing the nice value changes the share. `set_user_nice()`
updates `static_prio` and reweights the task in its run queue immediately.
That is how **running** processes change share without a restart, and why
`top` shows NI -7 / 5 like the screenshots.

### The clamp

Tier bias is limited to nice **-7..+5**, not the full -20..+19:

* GOLD : BRONZE = 4904 : 335, about 15 : 1. BRONZE still gets about 6% of a
  shared core, which is clearly measurable progress.
* With -20 and +19 the ratio would be 88761 : 15, about 5900 : 1. BRONZE would
  get 0.02%, which is starvation in practice.
* GOLD at -7 against SILVER gives about 83/17. That is a visible privilege
  without letting one user take the whole machine.

The fair scheduler never fully starves a runnable task, because every weight
is greater than zero. The clamp keeps the ratio small enough that the progress
is meaningful.

### Assignment rules

* `/proc/sched_tier` is mode 0644, and its write handler checks
  `capable(CAP_SYS_ADMIN)`. Only root can assign tiers.
* Only non-SILVER users are stored, in a table of 64 under a spinlock.
  Setting SILVER removes the entry. `reset` empties the table and re-nices
  everyone back to 0.
* **At most one GOLD.** Setting GOLD first removes any other GOLD entry, logs
  `GOLD -> SILVER` and re-nices that user to 0.
* **Re-tiering running work.** `st_apply()` walks every thread
  (`for_each_process_thread` under `rcu_read_lock`) and calls
  `set_user_nice()` on those whose real uid matches.
* **Inheritance.** In `sched_fork()` a child of a tiered user is given the
  tier's nice value and load weight before it first runs, even if its parent
  had been reniced. Real-time children keep their RT priority. Tasks that
  *switch* user, such as `sudo -u alice yes`, start as root and become alice
  in `commit_creds()`, where `st_cred_changed()` applies alice's tier. A switch between two SILVER
  users keeps the inherited nice value, so `nice -n 10 sudo -u bob job` still
  works.
* Root cannot be tiered, so system daemons are never re-niced.

### Scheduling groups

Weights only compete within one group (`concepts.md`). In the tests, both
`yes` processes are started by `sudo -u` from one root shell and stay in that
shell's cgroup, so their nice values compete directly. If alice and bob each
logged in separately, systemd would place them in different user cgroups,
each with equal cgroup weight. That is per-user fairness above the task
level, and nice values could not change the split. That is not a bug in the
tier code; it is how group scheduling is designed. We measured it: with
two separate ssh logins, a GOLD `yes` at nice -7 still got only about 50%
(see `q4_fork_guard/evidence/01-04_tiers.txt`).

## Mechanism 3: observability

* `/proc/fork_guard` (0444, `proc_create_single` + `seq_file`): the live
  tunables, then per user the tier, forks in the current window, stage, and
  cumulative throttle/freeze/reap counts. Reading it also applies a pending
  cooldown, so STAGE is accurate.
* `/proc/sched_tier`: `UID TIER NICE` for every non-SILVER user.
* `tier show` prints both files. If nobody is tiered it prints
  `(no user is tiered; everyone is SILVER)`.
* dmesg: `fork_guard: initialised (...)` and `sched_tier: initialised (...)`
  at boot, then every THROTTLE (`pr_warn`), FREEZE and REAP (`pr_alert`),
  every FORGIVE and every tier change (`pr_info`). `dmesg` colours the
  warning and alert levels, as in the screenshots of the problem statement.

## Tiers never weaken containment

`fg_check_fork()` never calls into `sched_tier.c`. The only shared symbol it
uses is `st_tier_name()`, and only to print the TIER column in
`/proc/fork_guard`. The only exemption is uid 0. Test case 6 shows a GOLD
user's bomb taking exactly the same path as an untiered one.

Lock order is always `fg_lock` then `st_lock` (only in the `/proc` show).
`sched_tier.c` never takes `fg_lock`, so the two locks cannot deadlock.

## Spinlocks without irqsave

The help document suggests `spin_lock_irqsave()`. Both locks are only taken
in process context: the fork path, `/proc` reads and writes, and the
workqueue item. They are never taken from an interrupt handler, so plain
`spin_lock()` is enough and disabling interrupts would add nothing.

## Limits and known simplifications

* Up to 64 users are tracked by fork_guard, and 64 users can have a
  non-SILVER tier. Further users are simply not tracked or tiered. Both are
  constants.
* A user who renices their own process after being tiered keeps that value
  until the next re-tier.
* Thread creation is not counted. A thread bomb is limited by `ulimit` and
  cgroup `pids.max` instead.
