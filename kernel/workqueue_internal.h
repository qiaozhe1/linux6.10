/* SPDX-License-Identifier: GPL-2.0 */
/*
 * kernel/workqueue_internal.h
 *
 * Workqueue internal header file.  Only to be included by workqueue and
 * core kernel subsystems.
 */
#ifndef _KERNEL_WORKQUEUE_INTERNAL_H
#define _KERNEL_WORKQUEUE_INTERNAL_H

#include <linux/workqueue.h>
#include <linux/kthread.h>
#include <linux/preempt.h>

struct worker_pool;

/*
 * The poor guys doing the actual heavy lifting.  All on-duty workers are
 * either serving the manager role, on idle list or on busy hash.  For
 * details on the locking annotation (L, I, X...), refer to workqueue.c.
 *
 * Only to be used in workqueue and async.
 */
struct worker {
	/* on idle list while idle, on busy hash table while busy */
	union {
		struct list_head	entry;	/* L: 工作者空闲时的链表节点 */
		struct hlist_node	hentry;	/* L: 工作者忙碌时的哈希表节点 */
	};

	struct work_struct	*current_work;	/* K: 当前正在处理的工作 */
	work_func_t		current_func;	/* K: 当前工作的函数指针 */
	struct pool_workqueue	*current_pwq;	/* K: 当前工作所在的工作队列 */
	u64			current_at;	/* K: 当前工作的开始运行时间或最后唤醒时间 */
	unsigned int		current_color;	/* K: 当前工作的标志颜色 */

	int			sleeping;	/* S: 工作者是否处于睡眠状态 */

	/* 用于调度器确定工作者的最后已知身份 */
	work_func_t		last_func;	/* K: 上一次工作的函数指针 */

	struct list_head	scheduled;	/* L: 已计划的工作列表 */

	struct task_struct	*task;		/* I: 关联的内核线程任务 */
	struct worker_pool	*pool;		/* A: 关联的工作池 */
						/* L: 对于救援线程的特殊标注 */
	struct list_head	node;		/* A: 锚定在 pool->workers 的节点 */
						/* A: 通过 worker->node 进行运行 */

	unsigned long		last_active;	/* K: 最后一次活跃的时间戳 */
	unsigned int		flags;		/* L: 工作者的标志 */
	int			id;		/* I: 工作者的唯一 ID */

	/*
	 * Opaque string set with work_set_desc().  Printed out with task
	 * dump for debugging - WARN, BUG, panic or sysrq.
	 * 使用 work_set_desc() 设置的描述性字符串。在调试（如 WARN、BUG、panic 或 sysrq）时打印。
	 */
	char			desc[WORKER_DESC_LEN];

	/* 仅用于救援工作者，指向目标工作队列  */
	struct workqueue_struct	*rescue_wq;	/* I: 需要救援的工作队列 */
};

/**
 * current_wq_worker - return struct worker if %current is a workqueue worker
 */
static inline struct worker *current_wq_worker(void)
{
	if (in_task() && (current->flags & PF_WQ_WORKER))
		return kthread_data(current);
	return NULL;
}

/*
 * Scheduler hooks for concurrency managed workqueue.  Only to be used from
 * sched/ and workqueue.c.
 */
void wq_worker_running(struct task_struct *task);
void wq_worker_sleeping(struct task_struct *task);
void wq_worker_tick(struct task_struct *task);
work_func_t wq_worker_last_func(struct task_struct *task);

#endif /* _KERNEL_WORKQUEUE_INTERNAL_H */
