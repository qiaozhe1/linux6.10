/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_TIMER_TYPES_H
#define _LINUX_TIMER_TYPES_H

#include <linux/lockdep_types.h>
#include <linux/types.h>

struct timer_list {//表示内核中的定时器
	/*
	 * All fields that change during normal runtime grouped to the
	 * same cacheline
	 */
	struct hlist_node	entry;//链表节点，用于将定时器添加到定时器链表中
	unsigned long		expires;//定时器的过期时间（以 jiffies 为单位）
	void			(*function)(struct timer_list *);//定时器到期时调用的回调函数
	u32			flags;//定时器的标志位

#ifdef CONFIG_LOCKDEP
	struct lockdep_map	lockdep_map;// 用于锁依赖检测的结构体（如果启用了 CONFIG_LOCKDEP）
#endif
};

#endif /* _LINUX_TIMER_TYPES_H */
