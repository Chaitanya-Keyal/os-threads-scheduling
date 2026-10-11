// SPDX-License-Identifier: GPL-2.0
/*
 * fork_guard: graduated fork-rate containment, per user.
 *
 * Every process creation (fork, vfork, clone without CLONE_THREAD) by a
 * non-root user is counted in a sliding window of fg_window_ms. When the
 * count goes over fg_threshold the user climbs a ladder:
 *
 *   stage 1 THROTTLE  over-threshold forks fail with -EAGAIN after a
 *                     penalty delay of fg_throttle_ms
 *   stage 2 FREEZE    still over the threshold fg_escalate_ms later:
 *                     SIGSTOP the process group of the forking task
 *   stage 3 REAP      still over the threshold fg_escalate_ms after the
 *                     freeze: SIGKILL that group, then start again at 0
 *
 * A user whose rate stays normal for fg_cooldown_s is forgiven (stage 0).
 * Root is never counted. The tier of a user is only shown in /proc and is
 * never looked at here, so tiers cannot weaken containment.
 */
#include <linux/cred.h>
#include <linux/delay.h>
#include <linux/fork_guard.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/pid.h>
#include <linux/proc_fs.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/sysctl.h>
#include <linux/workqueue.h>

#define FG_MAX_USERS 64
#define FG_SLOTS 10 /* the window is counted in 10 slots */

/* Tunables, under /proc/sys/kernel/. */
static int fg_enabled = 1;
static int fg_threshold = 200;
static int fg_window_ms = 1000;
static int fg_throttle_ms = 40;
static int fg_cooldown_s = 10;
static int fg_escalate_ms = 200;

struct fg_user {
	bool used;
	kuid_t uid;
	s64 slot_width;			/* ms per slot when the slots were filled */
	s64 slot_id[FG_SLOTS];		/* which time slot each counter holds */
	int slot_count[FG_SLOTS];
	int stage;			/* 0 normal, 1 throttled, 2 frozen */
	s64 stage_since;		/* ms */
	s64 last_violation;		/* ms */
	unsigned int throttles, freezes, reaps;
	struct pid *frozen;		/* the group that got SIGSTOP */
	struct delayed_work reap_work;	/* decides REAP or forgive after FREEZE */
};

static struct fg_user fg_users[FG_MAX_USERS];
static DEFINE_SPINLOCK(fg_lock);	/* protects fg_users[] */

static s64 now_ms(void)
{
	return ktime_to_ms(ktime_get());
}

static unsigned int uid_nr(const struct fg_user *u)
{
	return from_kuid(&init_user_ns, u->uid);
}

/* Forks in the last fg_window_ms, optionally counting one more first. */
static int fg_count(struct fg_user *u, s64 now, bool add)
{
	s64 width = max(fg_window_ms / FG_SLOTS, 1);
	s64 slot = div64_s64(now, width);
	int i, sum = 0;

	if (u->slot_width != width) {	/* window length changed: restart */
		memset(u->slot_id, 0, sizeof(u->slot_id));
		memset(u->slot_count, 0, sizeof(u->slot_count));
		u->slot_width = width;
	}
	if (add) {
		i = slot % FG_SLOTS;
		if (u->slot_id[i] != slot) {
			u->slot_id[i] = slot;
			u->slot_count[i] = 0;
		}
		u->slot_count[i]++;
	}
	for (i = 0; i < FG_SLOTS; i++)
		if (u->slot_id[i] > slot - FG_SLOTS)
			sum += u->slot_count[i];
	return sum;
}

static struct fg_user *fg_find(kuid_t uid, bool create)
{
	struct fg_user *free = NULL;
	int i;

	for (i = 0; i < FG_MAX_USERS; i++) {
		if (fg_users[i].used && uid_eq(fg_users[i].uid, uid))
			return &fg_users[i];
		if (!fg_users[i].used && !free)
			free = &fg_users[i];
	}
	if (!create || !free)
		return NULL;	/* table full: the user is not tracked */
	free->used = true;
	free->uid = uid;
	free->slot_width = 0;
	return free;
}

/* A throttled user who stayed under the threshold long enough. */
static void fg_try_forgive(struct fg_user *u, s64 now)
{
	if (u->stage == 1 &&
	    now - u->last_violation >= (s64)fg_cooldown_s * MSEC_PER_SEC) {
		u->stage = 0;
		pr_info("fork_guard: FORGIVE uid=%u (quiet for %d s, back to stage 0)\n",
			uid_nr(u), fg_cooldown_s);
	}
}

int fg_check_fork(struct kernel_clone_args *args)
{
	kuid_t uid = current_uid();
	struct pid *pgrp, *stop = NULL;
	struct fg_user *u;
	bool leader;
	s64 now;

	if (!fg_enabled || (args->flags & CLONE_THREAD) ||
	    (current->flags & PF_KTHREAD) || uid_eq(uid, GLOBAL_ROOT_UID))
		return 0;

	rcu_read_lock();
	pgrp = get_pid(task_pgrp(current));
	leader = task_pgrp(current) == task_pid(current);
	rcu_read_unlock();

	now = now_ms();
	spin_lock(&fg_lock);
	u = fg_find(uid, true);
	if (!u || fg_count(u, now, true) <= fg_threshold) {
		if (u)
			fg_try_forgive(u, now);
		spin_unlock(&fg_lock);
		put_pid(pgrp);
		return 0;
	}

	u->last_violation = now;
	if (u->stage == 0) {
		u->stage = 1;
		u->stage_since = now;
		u->throttles++;
		pr_info("fork_guard: THROTTLE uid=%u pgid=%d (>%d forks in %d ms)\n",
			uid_nr(u), pid_nr(pgrp), fg_threshold, fg_window_ms);
	} else if (u->stage == 1 && now - u->stage_since >= fg_escalate_ms &&
		   !leader) {
		/*
		 * A group leader (e.g. the user's shell starting a command) does
		 * not trigger the freeze; the bomb's many other members will.
		 */
		u->stage = 2;
		u->stage_since = now;
		u->freezes++;
		u->frozen = get_pid(pgrp);
		stop = pgrp;
		schedule_delayed_work(&u->reap_work,
				      msecs_to_jiffies(fg_escalate_ms));
		pr_info("fork_guard: FREEZE   uid=%u pgid=%d (SIGSTOP process group)\n",
			uid_nr(u), pid_nr(pgrp));
	}
	spin_unlock(&fg_lock);

	if (stop)
		kill_pgrp(stop, SIGSTOP, 1);
	put_pid(pgrp);
	msleep_interruptible(fg_throttle_ms);
	return -EAGAIN;
}

/* Runs fg_escalate_ms after a FREEZE (and again later if needed). */
static void fg_reap_work(struct work_struct *work)
{
	struct fg_user *u = container_of(to_delayed_work(work), struct fg_user,
					 reap_work);
	struct pid *grp = NULL;
	s64 now = now_ms();
	int sig = 0;

	spin_lock(&fg_lock);
	if (u->stage != 2)
		goto out;
	if (fg_count(u, now, false) > fg_threshold) {
		sig = SIGKILL;
		u->reaps++;
		pr_info("fork_guard: REAP     uid=%u pgid=%d (SIGKILL process group)\n",
			uid_nr(u), pid_nr(u->frozen));
		/* the offender is gone; start the user again from a clean slate */
		u->slot_width = 0;
		u->stage = 0;
	} else if (now - u->last_violation >=
		   (s64)fg_cooldown_s * MSEC_PER_SEC) {
		sig = SIGCONT;
		pr_info("fork_guard: FORGIVE uid=%u pgid=%d (SIGCONT, back to stage 0)\n",
			uid_nr(u), pid_nr(u->frozen));
		u->stage = 0;
	} else {
		schedule_delayed_work(&u->reap_work,
				      msecs_to_jiffies(fg_escalate_ms));
	}
	if (sig) {
		grp = u->frozen;
		u->frozen = NULL;
	}
out:
	spin_unlock(&fg_lock);
	if (grp) {
		kill_pgrp(grp, sig, 1);
		put_pid(grp);
	}
}

static int fg_show(struct seq_file *m, void *v)
{
	s64 now = now_ms();
	int i;

	seq_printf(m, "enabled=%d threshold=%d window_ms=%d throttle_ms=%d cooldown_s=%d escalate_ms=%d\n\n",
		   fg_enabled, fg_threshold, fg_window_ms, fg_throttle_ms,
		   fg_cooldown_s, fg_escalate_ms);
	seq_puts(m, "UID     TIER    FORKS   STAGE   THROTTLE  FREEZE  REAP\n");
	spin_lock(&fg_lock);
	for (i = 0; i < FG_MAX_USERS; i++) {
		struct fg_user *u = &fg_users[i];

		if (!u->used)
			continue;
		fg_try_forgive(u, now);
		seq_printf(m, "%-7u %-7s %-7d %-7d %-9u %-7u %u\n", uid_nr(u),
			   st_tier_name(u->uid), fg_count(u, now, false),
			   u->stage, u->throttles, u->freezes, u->reaps);
	}
	spin_unlock(&fg_lock);
	return 0;
}

static int fg_ten = 10, fg_minute_ms = 60000, fg_second_ms = 1000,
	   fg_hour_s = 3600;

static const struct ctl_table fg_sysctls[] = {
	{
		.procname	= "fg_enabled",
		.data		= &fg_enabled,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= SYSCTL_ONE,
	},
	{
		.procname	= "fg_threshold",
		.data		= &fg_threshold,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ONE,
		.extra2		= SYSCTL_INT_MAX,
	},
	{
		.procname	= "fg_window_ms",
		.data		= &fg_window_ms,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= &fg_ten,
		.extra2		= &fg_minute_ms,
	},
	{
		.procname	= "fg_throttle_ms",
		.data		= &fg_throttle_ms,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &fg_second_ms,
	},
	{
		.procname	= "fg_cooldown_s",
		.data		= &fg_cooldown_s,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &fg_hour_s,
	},
	{
		.procname	= "fg_escalate_ms",
		.data		= &fg_escalate_ms,
		.maxlen		= sizeof(int),
		.mode		= 0644,
		.proc_handler	= proc_dointvec_minmax,
		.extra1		= SYSCTL_ZERO,
		.extra2		= &fg_minute_ms,
	},
};

static int __init fork_guard_init(void)
{
	int i;

	for (i = 0; i < FG_MAX_USERS; i++)
		INIT_DELAYED_WORK(&fg_users[i].reap_work, fg_reap_work);
	register_sysctl("kernel", fg_sysctls);
	proc_create_single("fork_guard", 0444, NULL, fg_show);
	pr_info("fork_guard: initialised (threshold=%d/%dms, throttle=%dms, cooldown=%ds)\n",
		fg_threshold, fg_window_ms, fg_throttle_ms, fg_cooldown_s);
	return 0;
}
late_initcall(fork_guard_init);
