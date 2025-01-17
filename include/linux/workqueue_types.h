/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_WORKQUEUE_TYPES_H
#define _LINUX_WORKQUEUE_TYPES_H

#include <linux/atomic.h>
#include <linux/lockdep_types.h>
#include <linux/timer_types.h>
#include <linux/types.h>

struct workqueue_struct;

struct work_struct;
typedef void (*work_func_t)(struct work_struct *work);
void delayed_work_timer_fn(struct timer_list *t);

struct work_struct {//工作项，通过链表连接到工作队列，并且在内核的工作队列机制中调度执行
	atomic_long_t data;//用于存储工作项的状态和其他信息
	struct list_head entry;//链表节点，工作项将通过它连接到工作队列
	work_func_t func;//指向执行工作项的函数的指针，定义了具体的工作任务
#ifdef CONFIG_LOCKDEP
	struct lockdep_map lockdep_map;//用于锁依赖性检查，只有在 CONFIG_LOCKDEP 配置开启时才会包含
#endif
};

#endif /* _LINUX_WORKQUEUE_TYPES_H */
