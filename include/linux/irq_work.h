/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQ_WORK_H
#define _LINUX_IRQ_WORK_H

#include <linux/smp_types.h>
#include <linux/rcuwait.h>

/*
 * An entry can be in one of four states:
 *
 * free	     NULL, 0 -> {claimed}       : free to be used
 * claimed   NULL, 3 -> {pending}       : claimed to be enqueued
 * pending   next, 3 -> {busy}          : queued, pending callback
 * busy      NULL, 2 -> {free, claimed} : callback in progress, can be claimed
 */

struct irq_work {//表示中断工作（IRQ Work）任务的结构体，适用于需要在中断上下文中或在调度延迟执行的任务。
	struct __call_single_node node;//该字段是一个单节点结构，用于在多个 CPU 之间进行调用和调度操作。它帮助在 SMP（对称多处理器）系统中调度中断工作，使得特定的工作可以在特定的 CPU 上执行。
	void (*func)(struct irq_work *);//用于指向中断工作执行的函数。当中断工作被触发时，会调用该函数来执行具体的工作
	struct rcuwait irqwait;//用于等待中断工作完成的 RCU 同步等待结构。在中断工作系统中，irqwait 用于确保在一些工作完成之前，不会进行冲突的操作。

};

#define __IRQ_WORK_INIT(_func, _flags) (struct irq_work){	\
	.node = { .u_flags = (_flags), },			\
	.func = (_func),					\
	.irqwait = __RCUWAIT_INITIALIZER(irqwait),		\
}

#define IRQ_WORK_INIT(_func) __IRQ_WORK_INIT(_func, 0)
#define IRQ_WORK_INIT_LAZY(_func) __IRQ_WORK_INIT(_func, IRQ_WORK_LAZY)
#define IRQ_WORK_INIT_HARD(_func) __IRQ_WORK_INIT(_func, IRQ_WORK_HARD_IRQ)

#define DEFINE_IRQ_WORK(name, _f)				\
	struct irq_work name = IRQ_WORK_INIT(_f)

static inline
void init_irq_work(struct irq_work *work, void (*func)(struct irq_work *))
{
	*work = IRQ_WORK_INIT(func);
}

static inline bool irq_work_is_pending(struct irq_work *work)
{
	return atomic_read(&work->node.a_flags) & IRQ_WORK_PENDING;
}

static inline bool irq_work_is_busy(struct irq_work *work)
{
	return atomic_read(&work->node.a_flags) & IRQ_WORK_BUSY;
}

static inline bool irq_work_is_hard(struct irq_work *work)
{
	return atomic_read(&work->node.a_flags) & IRQ_WORK_HARD_IRQ;
}

bool irq_work_queue(struct irq_work *work);
bool irq_work_queue_on(struct irq_work *work, int cpu);

void irq_work_tick(void);
void irq_work_sync(struct irq_work *work);

#ifdef CONFIG_IRQ_WORK
#include <asm/irq_work.h>

void irq_work_run(void);
bool irq_work_needs_cpu(void);
void irq_work_single(void *arg);

void arch_irq_work_raise(void);

#else
static inline bool irq_work_needs_cpu(void) { return false; }
static inline void irq_work_run(void) { }
static inline void irq_work_single(void *arg) { }
#endif

#endif /* _LINUX_IRQ_WORK_H */
