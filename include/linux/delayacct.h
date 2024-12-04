/* SPDX-License-Identifier: GPL-2.0-or-later */
/* delayacct.h - per-task delay accounting
 *
 * Copyright (C) Shailabh Nagar, IBM Corp. 2006
 */

#ifndef _LINUX_DELAYACCT_H
#define _LINUX_DELAYACCT_H

#include <uapi/linux/taskstats.h>

#ifdef CONFIG_TASK_DELAY_ACCT
struct task_delay_info {//任务延迟信息结构，用于跟踪与任务相关的各种延迟情况
	raw_spinlock_t	lock;//保护延迟统计数据的自旋锁，确保在多核环境中对延迟信息的原子访问

	/* For each stat XXX, add following, aligned appropriately
	 *
	 * struct timespec XXX_start, XXX_end;
	 * u64 XXX_delay;
	 * u32 XXX_count;
	 *
	 * Atomicity of updates to XXX_delay, XXX_count protected by
	 * single lock above (split into XXX_lock if contention is an issue).
	 */

	/*
	 * XXX_count is incremented on every XXX operation, the delay
	 * associated with the operation is added to XXX_delay.
	 * XXX_delay contains the accumulated delay time in nanoseconds.
	 */
	u64 blkio_start;	//块设备 I/O 操作的开始时间，单位为纳秒
	u64 blkio_delay;	//累计块设备 I/O 操作的延迟时间
	u64 swapin_start;	//换入（swapin）操作的开始时间，单位为纳秒
	u64 swapin_delay;	// 累计换入操作的延迟时间
	u32 blkio_count;	//块设备 I/O 操作的总次数
				//同步块设备 I/O 操作的次数统计
	u32 swapin_count;	//换入操作的总次数

	u64 freepages_start;	//内存回收操作的开始时间
	u64 freepages_delay;	//累计内存回收操作的延迟时间

	u64 thrashing_start;	//处理页抖动（thrashing）操作的开始时间
	u64 thrashing_delay;	//累计页抖动操作的延迟时间

	u64 compact_start;	//内存压缩（compaction）操作的开始时间
	u64 compact_delay;	//累计内存压缩操作的延迟时间

	u64 wpcopy_start;	//写保护复制（write-protect copy）操作的开始时间
	u64 wpcopy_delay;	//累计写保护复制操作的延迟时间

	u64 irq_delay;		//中断（IRQ/SOFTIRQ）处理的延迟时间

	u32 freepages_count;	//内存回收操作的总次数
	u32 thrashing_count;	//页抖动等待操作的总次数
	u32 compact_count;	//内存压缩操作的总次数
	u32 wpcopy_count;	//写保护复制操作的总次数
	u32 irq_count;		//中断（IRQ/SOFTIRQ）处理的总次数
};
#endif

#include <linux/sched.h>
#include <linux/slab.h>
#include <linux/jump_label.h>

#ifdef CONFIG_TASK_DELAY_ACCT
DECLARE_STATIC_KEY_FALSE(delayacct_key);
extern int delayacct_on;	/* Delay accounting turned on/off */
extern struct kmem_cache *delayacct_cache;
extern void delayacct_init(void);

extern void __delayacct_tsk_init(struct task_struct *);
extern void __delayacct_tsk_exit(struct task_struct *);
extern void __delayacct_blkio_start(void);
extern void __delayacct_blkio_end(struct task_struct *);
extern int delayacct_add_tsk(struct taskstats *, struct task_struct *);
extern __u64 __delayacct_blkio_ticks(struct task_struct *);
extern void __delayacct_freepages_start(void);
extern void __delayacct_freepages_end(void);
extern void __delayacct_thrashing_start(bool *in_thrashing);
extern void __delayacct_thrashing_end(bool *in_thrashing);
extern void __delayacct_swapin_start(void);
extern void __delayacct_swapin_end(void);
extern void __delayacct_compact_start(void);
extern void __delayacct_compact_end(void);
extern void __delayacct_wpcopy_start(void);
extern void __delayacct_wpcopy_end(void);
extern void __delayacct_irq(struct task_struct *task, u32 delta);

static inline void delayacct_tsk_init(struct task_struct *tsk)
{
	/* reinitialize in case parent's non-null pointer was dup'ed*/
	tsk->delays = NULL;
	if (delayacct_on)
		__delayacct_tsk_init(tsk);
}

/* Free tsk->delays. Called from bad fork and __put_task_struct
 * where there's no risk of tsk->delays being accessed elsewhere
 */
static inline void delayacct_tsk_free(struct task_struct *tsk)
{
	if (tsk->delays)
		kmem_cache_free(delayacct_cache, tsk->delays);
	tsk->delays = NULL;
}

static inline void delayacct_blkio_start(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_blkio_start();
}

static inline void delayacct_blkio_end(struct task_struct *p)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (p->delays)
		__delayacct_blkio_end(p);
}

static inline __u64 delayacct_blkio_ticks(struct task_struct *tsk)
{
	if (tsk->delays)
		return __delayacct_blkio_ticks(tsk);
	return 0;
}

static inline void delayacct_freepages_start(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_freepages_start();
}

static inline void delayacct_freepages_end(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_freepages_end();
}

static inline void delayacct_thrashing_start(bool *in_thrashing)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_thrashing_start(in_thrashing);
}

static inline void delayacct_thrashing_end(bool *in_thrashing)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_thrashing_end(in_thrashing);
}

static inline void delayacct_swapin_start(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_swapin_start();
}

static inline void delayacct_swapin_end(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_swapin_end();
}

static inline void delayacct_compact_start(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_compact_start();
}

static inline void delayacct_compact_end(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_compact_end();
}

static inline void delayacct_wpcopy_start(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_wpcopy_start();
}

static inline void delayacct_wpcopy_end(void)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (current->delays)
		__delayacct_wpcopy_end();
}

static inline void delayacct_irq(struct task_struct *task, u32 delta)
{
	if (!static_branch_unlikely(&delayacct_key))
		return;

	if (task->delays)
		__delayacct_irq(task, delta);
}

#else
static inline void delayacct_init(void)
{}
static inline void delayacct_tsk_init(struct task_struct *tsk)
{}
static inline void delayacct_tsk_free(struct task_struct *tsk)
{}
static inline void delayacct_blkio_start(void)
{}
static inline void delayacct_blkio_end(struct task_struct *p)
{}
static inline int delayacct_add_tsk(struct taskstats *d,
					struct task_struct *tsk)
{ return 0; }
static inline __u64 delayacct_blkio_ticks(struct task_struct *tsk)
{ return 0; }
static inline int delayacct_is_task_waiting_on_io(struct task_struct *p)
{ return 0; }
static inline void delayacct_freepages_start(void)
{}
static inline void delayacct_freepages_end(void)
{}
static inline void delayacct_thrashing_start(bool *in_thrashing)
{}
static inline void delayacct_thrashing_end(bool *in_thrashing)
{}
static inline void delayacct_swapin_start(void)
{}
static inline void delayacct_swapin_end(void)
{}
static inline void delayacct_compact_start(void)
{}
static inline void delayacct_compact_end(void)
{}
static inline void delayacct_wpcopy_start(void)
{}
static inline void delayacct_wpcopy_end(void)
{}
static inline void delayacct_irq(struct task_struct *task, u32 delta)
{}

#endif /* CONFIG_TASK_DELAY_ACCT */

#endif
