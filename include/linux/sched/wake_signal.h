/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_WAKE_SIGNAL_H
#define _LINUX_SCHED_WAKE_SIGNAL_H

#include <linux/wait.h>

/* Internal wake flags, never poll bits or userspace supplied priority. */
#define WF_SYNC			0x10
#define WF_SCX_INPUT		0x100
#define WF_SCX_FENCE		0x200
#define WF_SCX_SIGNAL_MASK	(WF_SCX_INPUT | WF_SCX_FENCE)
#define WF_SCX_SIGNAL_SHIFT	8

int wake_up_state_signal(struct task_struct *p, unsigned int state,
			 unsigned int signal);
int __wake_up_flags(struct wait_queue_head *head, unsigned int mode,
		    int nr_exclusive, int wake_flags, void *key);
void __wake_up_locked_key_flags(struct wait_queue_head *head, unsigned int mode,
				int wake_flags, void *key);

#endif
