# Problem 4 - fork_guard and sched_tier (Linux 6.14)

Two kernel mechanisms that work independently of each other:

* **fork_guard**: graduated fork-rate containment per user. THROTTLE, then
  FREEZE (SIGSTOP the group), then REAP (SIGKILL the group). A user who calms
  down is forgiven after a cooldown.
* **sched_tier**: GOLD / SILVER / BRONZE CPU tiers per user, applied to
  running processes at once through their CFS weight.

Both are observable through `/proc/fork_guard` and `/proc/sched_tier`, and
all fork_guard thresholds are sysctls.

## Files

| File | Purpose |
|------|---------|
| `fork_guard.patch` | everything below as one patch against `v6.14` |
| `linux/kernel/fork_guard.c` | fork-rate ladder, sysctls, `/proc/fork_guard` |
| `linux/kernel/sched_tier.c` | tier table, re-nicing, `/proc/sched_tier` |
| `linux/include/linux/fork_guard.h` | the hooks called from `fork.c` and `cred.c` |
| `tier.c` | `tier set <user> <gold\|silver\|bronze>`, `tier reset`, `tier show` |
| `forkburst.c` | a bursty but legitimate workload: throttled, never frozen |
| `evidence/` | output captured on the VM: `01-04_tiers.txt`, `05-07_containment.txt`, `08_observability.txt`, `forkburst.txt` |

The patch also edits three existing files:

1. `kernel/fork.c`: `kernel_clone()` calls `fg_check_fork(args)` right after
   its argument checks and returns `-EAGAIN` if containment refuses the fork.
   Every fork, vfork, clone and clone3 passes through here.
2. `kernel/cred.c`: `commit_creds()` calls `st_cred_changed()` when the real
   uid changes, so `sudo -u alice cmd`, `su` and logins get the new user's
   tier.
3. `kernel/Makefile`: adds `fork_guard.o sched_tier.o` to `obj-y`.

## Interfaces

| sysctl (`/proc/sys/kernel/`) | default | meaning |
|-------------------------------|---------|---------|
| `fg_enabled` | 1 | 0 switches containment off |
| `fg_threshold` | 200 | forks allowed per window, per user |
| `fg_window_ms` | 1000 | sliding window length |
| `fg_throttle_ms` | 40 | penalty sleep for each refused fork |
| `fg_cooldown_s` | 10 | quiet time after which a user is forgiven |
| `fg_escalate_ms` | 200 | minimum time in THROTTLE before FREEZE, and in FREEZE before REAP |

```
$ cat /proc/fork_guard
enabled=1 threshold=200 window_ms=1000 throttle_ms=40 cooldown_s=10 escalate_ms=200

UID     TIER    FORKS   STAGE   THROTTLE  FREEZE  REAP
1001    GOLD    0       0       2         2       2

$ cat /proc/sched_tier
UID     TIER    NICE
1001    GOLD    -7
1002    BRONZE  5
```

`/proc/sched_tier` accepts `set <uid> gold|silver|bronze` and `reset` from
root (`CAP_SYS_ADMIN`). The `tier` tool is the front end.

## Build and install (Ubuntu 24.04 VM, 4 cores)

The steps match assignment 1's kernel build. Starting from the 6.14 source:

```bash
sudo apt install -y build-essential libncurses-dev bison flex libssl-dev \
                    libelf-dev bc dwarves zstd fakeroot
wget https://cdn.kernel.org/pub/linux/kernel/v6.x/linux-6.14.tar.xz
tar xf linux-6.14.tar.xz && cd linux-6.14
patch -p1 < /path/to/q4_fork_guard/fork_guard.patch

cp /boot/config-$(uname -r) .config
yes "" | make localmodconfig
scripts/config --disable SYSTEM_TRUSTED_KEYS --disable SYSTEM_REVOCATION_KEYS \
               --disable DEBUG_INFO --enable DEBUG_INFO_NONE --disable DEBUG_INFO_BTF \
               --set-str CONFIG_LOCALVERSION "-forkguard"
make olddefconfig
make -j$(nproc)
sudo make modules_install && sudo make install
```

On our VM the tree is `~/linux-6.14-fg`. GRUB is set to boot
`6.14.0-forkguard` by default through `GRUB_DEFAULT` in `/etc/default/grub`.
The assignment 1 kernel `6.14.0-setnice` is still under "Advanced options".

```bash
uname -r                              # 6.14.0-forkguard
sudo dmesg | grep -E "fork_guard|sched_tier"
make -C .. q4_fork_guard/tier q4_fork_guard/forkburst
sudo install tier forkburst /usr/local/bin/
sudo apt install -y forkstat
```

The test cases need two ordinary users. The VM has `alice` (uid 1001) and
`bob` (uid 1002), with passwords `alice` and `bob`.

## Test cases

The outputs captured for each step are in `evidence/`.

**Run the tier tests (1-4) from one root shell.** Both workloads must be
started with `sudo -u` from the same root shell, as the problem statement
does. Weights only compete inside one scheduling group. Separate logins
would put alice and bob in different systemd session cgroups, and each cgroup
gets an equal share no matter what the nice values inside it are.

1. **Baseline.** `tier show`, then
   `sudo -u alice taskset -c 0 yes > /dev/null &` and the same for bob.
   `top` shows about 50/50.
2. **Promote.** `tier set alice gold`. alice's `yes` goes to nice -7 and about
   83% within a second. `ps -u alice -o pid,ni,pcpu,etime,comm` shows the same
   PID and ELAPSED as before.
3. **Demote.** `tier set bob bronze`. The split becomes about 94/6, and bob's
   `TIME+` keeps increasing.
4. **Reset.** `tier reset`. Back to about 50/50. `kill %1 %2`.
5. **Containment, untiered.** Root runs `sudo dmesg -w | grep fork_guard`
   (and `sudo forkstat` if wanted). In alice's own terminal (`ssh alice@...`)
   run `:(){ :|:& };:`. dmesg shows THROTTLE, FREEZE and REAP within 0.5-2 s.
   The bomb dies, and alice's prompt keeps working without killing the
   terminal.
6. **Containment, GOLD.** `tier set alice gold` and repeat 5. The ladder is
   identical, because `fg_check_fork()` never looks at tiers.
7. **Retune at runtime.** `sysctl -w kernel.fg_threshold=50` and
   `sysctl -w kernel.fg_window_ms=500`, then repeat 5. THROTTLE now reports
   `>50 forks in 500 ms`. Restore with `sysctl -w kernel.fg_threshold=200
   kernel.fg_window_ms=1000`.
8. **Observability.** `cat /proc/fork_guard`, `cat /proc/sched_tier`,
   `tier show`, and `dmesg | grep -E "fork_guard|sched_tier"`.

To see graduated containment, run `forkburst` as alice
(`evidence/forkburst.txt`). It forks 300
children as fast as it can. Fork 201 fails with EAGAIN (THROTTLE), it backs
off for 1.1 s and finishes. It is never frozen, and it is forgiven after the
cooldown.
