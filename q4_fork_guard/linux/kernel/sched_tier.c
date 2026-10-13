// SPDX-License-Identifier: GPL-2.0
/*
 * sched_tier: GOLD / SILVER / BRONZE CPU service tiers, per user.
 *
 * A tier is applied by setting the nice value of every task of the user,
 * which selects the task's CFS load weight (sched_prio_to_weight[]):
 *
 *   GOLD   nice -7  weight 4904
 *   SILVER nice  0  weight 1024   (everyone's default)
 *   BRONZE nice +5  weight  335
 *
 * The bias is clamped to nice -7..+5 instead of the full -20..+19 so that
 * BRONZE still makes progress next to GOLD: 335 / (4904 + 335) = ~6% of a
 * shared core, where nice 19 against nice -20 would be 15 / 88776 = 0.02%.
 *
 * New tasks get their user's tier in sched_fork(), tasks that switch user
 * (sudo, su, login) in commit_creds(), and running tasks are re-niced when
 * the tier changes.
 *
 * Root writes commands to /proc/sched_tier:
 *   set <uid> gold|silver|bronze
 *   reset
 * At most one user is GOLD; making a new GOLD user demotes the old one.
 */
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/fork_guard.h>
#include <linux/proc_fs.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/sched/signal.h>
#include <linux/seq_file.h>
#include <linux/spinlock.h>
#include <linux/string.h>
#include <linux/uaccess.h>

#define ST_MAX_USERS 64

enum tier { SILVER, GOLD, BRONZE };

static const char *const tier_name[] = { "SILVER", "GOLD", "BRONZE" };
static const int tier_nice[] = { 0, -7, 5 };

/* Only users that are not SILVER are stored. */
static struct {
	kuid_t uid;
	enum tier tier;
} st_users[ST_MAX_USERS];
static int st_count;
static DEFINE_SPINLOCK(st_lock);	/* protects st_users[] and st_count */

static unsigned int uid_nr(kuid_t uid)
{
	return from_kuid(&init_user_ns, uid);
}

/* Caller holds st_lock. */
static int st_index(kuid_t uid)
{
	int i;

	for (i = 0; i < st_count; i++)
		if (uid_eq(st_users[i].uid, uid))
			return i;
	return -1;
}

static enum tier st_tier(kuid_t uid)
{
	enum tier t;
	int i;

	spin_lock(&st_lock);
	i = st_index(uid);
	t = i < 0 ? SILVER : st_users[i].tier;
	spin_unlock(&st_lock);
	return t;
}

const char *st_tier_name(kuid_t uid)
{
	return tier_name[st_tier(uid)];
}

/*
 * Re-nice every task (thread) of the user, so running jobs change share
 * at once without being restarted.
 */
static void st_apply(kuid_t uid, enum tier t)
{
	struct task_struct *g, *p;

	rcu_read_lock();
	for_each_process_thread(g, p)
		if (uid_eq(task_uid(p), uid))
			set_user_nice(p, tier_nice[t]);
	rcu_read_unlock();
}

/*
 * Called from sched_fork(): a new task of a tiered user starts with the
 * tier's nice value, whatever nice value its parent had.
 */
bool st_fork_nice(struct task_struct *p, int *nice)
{
	enum tier t;

	rcu_read_lock();
	t = st_tier(task_uid(p));
	rcu_read_unlock();
	*nice = tier_nice[t];
	return t != SILVER;
}

/*
 * A task switched user (e.g. sudo -u alice, su, login): give it the new
 * user's tier. Untiered to untiered keeps the inherited nice value.
 */
void st_cred_changed(struct task_struct *task, kuid_t old_uid, kuid_t new_uid)
{
	enum tier t_new = st_tier(new_uid);

	if (t_new != SILVER || st_tier(old_uid) != SILVER)
		set_user_nice(task, tier_nice[t_new]);
}

static int st_set(kuid_t uid, enum tier t)
{
	kuid_t demoted = INVALID_UID;
	int i, ret = 0;

	spin_lock(&st_lock);
	if (t == GOLD) {	/* at most one GOLD user */
		for (i = 0; i < st_count; i++) {
			if (st_users[i].tier == GOLD &&
			    !uid_eq(st_users[i].uid, uid)) {
				demoted = st_users[i].uid;
				st_users[i] = st_users[--st_count];
				break;
			}
		}
	}
	i = st_index(uid);
	if (t == SILVER) {
		if (i >= 0)
			st_users[i] = st_users[--st_count];
	} else if (i >= 0) {
		st_users[i].tier = t;
	} else if (st_count < ST_MAX_USERS) {
		st_users[st_count].uid = uid;
		st_users[st_count++].tier = t;
	} else {
		ret = -ENOSPC;
	}
	spin_unlock(&st_lock);
	if (ret)
		return ret;
	fg_track(uid);

	if (uid_valid(demoted)) {
		pr_info("sched_tier: uid=%u GOLD -> SILVER (only one GOLD user)\n",
			uid_nr(demoted));
		st_apply(demoted, SILVER);
	}
	pr_info("sched_tier: uid=%u -> %s (nice %d)\n", uid_nr(uid),
		tier_name[t], tier_nice[t]);
	st_apply(uid, t);
	return 0;
}

static void st_reset(void)
{
	kuid_t uids[ST_MAX_USERS];
	int i, n;

	spin_lock(&st_lock);
	n = st_count;
	for (i = 0; i < n; i++)
		uids[i] = st_users[i].uid;
	st_count = 0;
	spin_unlock(&st_lock);

	for (i = 0; i < n; i++)
		st_apply(uids[i], SILVER);
	pr_info("sched_tier: all users reset to SILVER\n");
}

static int st_show(struct seq_file *m, void *v)
{
	int i;

	seq_puts(m, "UID     TIER    NICE\n");
	spin_lock(&st_lock);
	for (i = 0; i < st_count; i++)
		seq_printf(m, "%-7u %-7s %d\n", uid_nr(st_users[i].uid),
			   tier_name[st_users[i].tier],
			   tier_nice[st_users[i].tier]);
	spin_unlock(&st_lock);
	return 0;
}

static int st_open(struct inode *inode, struct file *file)
{
	return single_open(file, st_show, NULL);
}

static ssize_t st_write(struct file *file, const char __user *ubuf,
			size_t len, loff_t *ppos)
{
	char buf[64], name[8];
	unsigned int id;
	kuid_t uid;
	int t, ret;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (len >= sizeof(buf))
		return -EINVAL;
	if (copy_from_user(buf, ubuf, len))
		return -EFAULT;
	buf[len] = '\0';

	if (sysfs_streq(buf, "reset")) {
		st_reset();
		return len;
	}
	if (sscanf(buf, "set %u %7s", &id, name) != 2)
		return -EINVAL;
	uid = make_kuid(current_user_ns(), id);
	if (!uid_valid(uid) || uid_eq(uid, GLOBAL_ROOT_UID))
		return -EINVAL;	/* root is never tiered */
	for (t = 0; t < ARRAY_SIZE(tier_name); t++)
		if (!strcasecmp(name, tier_name[t]))
			break;
	if (t == ARRAY_SIZE(tier_name))
		return -EINVAL;
	ret = st_set(uid, t);
	return ret ? ret : len;
}

static const struct proc_ops st_proc_ops = {
	.proc_open	= st_open,
	.proc_read	= seq_read,
	.proc_write	= st_write,
	.proc_lseek	= seq_lseek,
	.proc_release	= single_release,
};

static int __init sched_tier_init(void)
{
	proc_create("sched_tier", 0644, NULL, &st_proc_ops);
	pr_info("sched_tier: initialised (gold nice=%d, bronze nice=%d)\n",
		tier_nice[GOLD], tier_nice[BRONZE]);
	return 0;
}
late_initcall(sched_tier_init);
