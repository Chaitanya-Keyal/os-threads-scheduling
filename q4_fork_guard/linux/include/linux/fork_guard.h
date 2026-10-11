/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Fork storm containment (kernel/fork_guard.c) and tiered CPU scheduling
 * (kernel/sched_tier.c).
 */
#ifndef _LINUX_FORK_GUARD_H
#define _LINUX_FORK_GUARD_H

#include <linux/types.h>
#include <linux/uidgid.h>

struct kernel_clone_args;
struct task_struct;

/* Called from kernel_clone(): 0 to allow the fork, -EAGAIN to refuse it. */
int fg_check_fork(struct kernel_clone_args *args);

/* Called from commit_creds() when a task's real uid changes. */
void st_cred_changed(struct task_struct *task, kuid_t old_uid, kuid_t new_uid);

/* "GOLD", "SILVER" or "BRONZE", for /proc/fork_guard. */
const char *st_tier_name(kuid_t uid);

#endif /* _LINUX_FORK_GUARD_H */
