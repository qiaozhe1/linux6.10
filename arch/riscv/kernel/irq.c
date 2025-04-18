// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2018 Christoph Hellwig
 */

#include <linux/interrupt.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/scs.h>
#include <linux/seq_file.h>
#include <asm/sbi.h>
#include <asm/smp.h>
#include <asm/softirq_stack.h>
#include <asm/stacktrace.h>

static struct fwnode_handle *(*__get_intc_node)(void);

void riscv_set_intc_hwnode_fn(struct fwnode_handle *(*fn)(void))
{
	__get_intc_node = fn;
}

struct fwnode_handle *riscv_get_intc_hwnode(void)
{
	if (__get_intc_node)
		return __get_intc_node();

	return NULL;
}
EXPORT_SYMBOL_GPL(riscv_get_intc_hwnode);

#ifdef CONFIG_IRQ_STACKS
#include <asm/irq_stack.h>

DECLARE_PER_CPU(ulong *, irq_shadow_call_stack_ptr);

#ifdef CONFIG_SHADOW_CALL_STACK
DEFINE_PER_CPU(ulong *, irq_shadow_call_stack_ptr);
#endif

static void init_irq_scs(void)
{
	int cpu;//定义一个整数变量 cpu，用于循环遍历所有可能的 CPU。

	if (!scs_is_enabled())//如果 Shadow Call Stack (SCS) 功能未启用，直接返回，不做任何操作。
		return;

	for_each_possible_cpu(cpu)//遍历系统中所有可能存在的 CPU。
		per_cpu(irq_shadow_call_stack_ptr, cpu) =
			scs_alloc(cpu_to_node(cpu));//为每个 CPU 分配一个 Shadow Call Stack，并存储指针。
}

DEFINE_PER_CPU(ulong *, irq_stack_ptr);

#ifdef CONFIG_VMAP_STACK
static void init_irq_stacks(void)
{
	int cpu;
	ulong *p;

	for_each_possible_cpu(cpu) {//遍历系统中所有可能的 CPU
		p = arch_alloc_vmap_stack(IRQ_STACK_SIZE, cpu_to_node(cpu));//为当前 CPU 分配 IRQ 堆栈，使用 CPU 所在的 NUMA 节点
		per_cpu(irq_stack_ptr, cpu) = p;//将分配的堆栈指针存储到每个CPU的局部变量中
	}
}
#else
/* irq stack only needs to be 16 byte aligned - not IRQ_STACK_SIZE aligned. */
DEFINE_PER_CPU_ALIGNED(ulong [IRQ_STACK_SIZE/sizeof(ulong)], irq_stack);

static void init_irq_stacks(void)
{
	int cpu;

	for_each_possible_cpu(cpu)
		per_cpu(irq_stack_ptr, cpu) = per_cpu(irq_stack, cpu);
}
#endif /* CONFIG_VMAP_STACK */

#ifdef CONFIG_SOFTIRQ_ON_OWN_STACK
static void ___do_softirq(struct pt_regs *regs)
{
	__do_softirq();
}

void do_softirq_own_stack(void)
{
	if (on_thread_stack())
		call_on_irq_stack(NULL, ___do_softirq);
	else
		__do_softirq();
}
#endif /* CONFIG_SOFTIRQ_ON_OWN_STACK */

#else
static void init_irq_scs(void) {}
static void init_irq_stacks(void) {}
#endif /* CONFIG_IRQ_STACKS */

int arch_show_interrupts(struct seq_file *p, int prec)
{
	show_ipi_stats(p, prec);
	return 0;
}

void __init init_IRQ(void)
{
	init_irq_scs();//初始化每个 CPU 的中断上下文存储.为每个 CPU 配置专用的 IRQ 上下文。
	init_irq_stacks();//初始化中断堆栈，确保每个CPU拥有独立的中断处理堆栈。避免在中断处理过程中出现堆栈混乱。
	irqchip_init();//初始化中断控制器，设置中断处理器的基本功能和参数。为系统提供处理硬件中断的能力。
	if (!handle_arch_irq)//检查是否已设置 handle_arch_irq，它是一个指向架构特定中断处理函数的指针。
		panic("No interrupt controller found.");//如果未找到中断控制器,系统将无法处理中断，触发内核 panic 错误。
	sbi_ipi_init();//初始化 SBI IPI (Software Break Interrupt)，用于处理多核系统中的软件中断，支持多个核心之间的通信和同步。。
}
