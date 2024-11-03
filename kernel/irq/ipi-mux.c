// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Multiplex several virtual IPIs over a single HW IPI.
 *
 * Copyright The Asahi Linux Contributors
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "ipi-mux: " fmt
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <linux/jump_label.h>
#include <linux/percpu.h>
#include <linux/smp.h>

struct ipi_mux_cpu {//用于表示每个 CPU 在 IPI 多路复用器中的状态和配置
	atomic_t			enable;//表示该 CPU 的 IPI 发送是否被启用的原子变量。用于多核环境中，确保对该值的访问是线程安全的。
	atomic_t			bits;//存储与该 CPU 相关的 IPI 触发位的原子变量。可以用来表示该 CPU 收到了多少个 IPI 中断的请求，确保在并发访问时数据的正确性。
};

static struct ipi_mux_cpu __percpu *ipi_mux_pcpu;
static struct irq_domain *ipi_mux_domain;
static void (*ipi_mux_send)(unsigned int cpu);

static void ipi_mux_mask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);

	atomic_andnot(BIT(irqd_to_hwirq(d)), &icpu->enable);
}

static void ipi_mux_unmask(struct irq_data *d)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	u32 ibit = BIT(irqd_to_hwirq(d));

	atomic_or(ibit, &icpu->enable);

	/*
	 * The atomic_or() above must complete before the atomic_read()
	 * below to avoid racing ipi_mux_send_mask().
	 */
	smp_mb__after_atomic();

	/* If a pending IPI was unmasked, raise a parent IPI immediately. */
	if (atomic_read(&icpu->bits) & ibit)
		ipi_mux_send(smp_processor_id());
}

static void ipi_mux_send_mask(struct irq_data *d, const struct cpumask *mask)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	u32 ibit = BIT(irqd_to_hwirq(d));
	unsigned long pending;
	int cpu;

	for_each_cpu(cpu, mask) {
		icpu = per_cpu_ptr(ipi_mux_pcpu, cpu);

		/*
		 * This sequence is the mirror of the one in ipi_mux_unmask();
		 * see the comment there. Additionally, release semantics
		 * ensure that the vIPI flag set is ordered after any shared
		 * memory accesses that precede it. This therefore also pairs
		 * with the atomic_fetch_andnot in ipi_mux_process().
		 */
		pending = atomic_fetch_or_release(ibit, &icpu->bits);

		/*
		 * The atomic_fetch_or_release() above must complete
		 * before the atomic_read() below to avoid racing with
		 * ipi_mux_unmask().
		 */
		smp_mb__after_atomic();

		/*
		 * The flag writes must complete before the physical IPI is
		 * issued to another CPU. This is implied by the control
		 * dependency on the result of atomic_read() below, which is
		 * itself already ordered after the vIPI flag write.
		 */
		if (!(pending & ibit) && (atomic_read(&icpu->enable) & ibit))
			ipi_mux_send(cpu);
	}
}

static const struct irq_chip ipi_mux_chip = {
	.name		= "IPI Mux",
	.irq_mask	= ipi_mux_mask,
	.irq_unmask	= ipi_mux_unmask,
	.ipi_send_mask	= ipi_mux_send_mask,
};

static int ipi_mux_domain_alloc(struct irq_domain *d, unsigned int virq,
				unsigned int nr_irqs, void *arg)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_percpu_devid(virq + i);
		irq_domain_set_info(d, virq + i, i, &ipi_mux_chip, NULL,
				    handle_percpu_devid_irq, NULL, NULL);
	}

	return 0;
}

static const struct irq_domain_ops ipi_mux_domain_ops = {
	.alloc		= ipi_mux_domain_alloc,
	.free		= irq_domain_free_irqs_top,
};

/**
 * ipi_mux_process - Process multiplexed virtual IPIs
 */
void ipi_mux_process(void)
{
	struct ipi_mux_cpu *icpu = this_cpu_ptr(ipi_mux_pcpu);
	irq_hw_number_t hwirq;
	unsigned long ipis;
	unsigned int en;

	/*
	 * Reading enable mask does not need to be ordered as long as
	 * this function is called from interrupt handler because only
	 * the CPU itself can change it's own enable mask.
	 */
	en = atomic_read(&icpu->enable);

	/*
	 * Clear the IPIs we are about to handle. This pairs with the
	 * atomic_fetch_or_release() in ipi_mux_send_mask().
	 */
	ipis = atomic_fetch_andnot(en, &icpu->bits) & en;

	for_each_set_bit(hwirq, &ipis, BITS_PER_TYPE(int))
		generic_handle_domain_irq(ipi_mux_domain, hwirq);
}

/**
 * ipi_mux_create - Create virtual IPIs multiplexed on top of a single
 * parent IPI.
 * @nr_ipi:		number of virtual IPIs to create. This should
 *			be <= BITS_PER_TYPE(int)
 * @mux_send:		callback to trigger parent IPI for a particular CPU
 *
 * Returns first virq of the newly created virtual IPIs upon success
 * or <=0 upon failure
 */
int ipi_mux_create(unsigned int nr_ipi, void (*mux_send)(unsigned int cpu))
{
	struct fwnode_handle *fwnode;//设备节点句柄，用于描述 IPI Mux 的硬件特性
	struct irq_domain *domain;//中断域结构体，用于管理 IPI 中断的上下文和操作
	int rc;//返回状态码，用于指示函数执行的结果

	if (ipi_mux_domain)// 检查是否已经创建了IPI Mux域
		return -EEXIST;//如果已存在 IPI Mux 域，返回错误代码 EEXIST（已存在）

	if (BITS_PER_TYPE(int) < nr_ipi || !mux_send)//检查给定的 IPI 数量和发送函数指针的有效性
		return -EINVAL;//如果 nr_ipi 超过整数类型的位数，或者发送函数为空，返回错误代码 EINVAL（无效参数）

	ipi_mux_pcpu = alloc_percpu(typeof(*ipi_mux_pcpu));//为per_cpu分配 IPI Mux 数据结构
	if (!ipi_mux_pcpu)
		return -ENOMEM;//如果分配失败，返回错误代码 ENOMEM（内存不足）

	fwnode = irq_domain_alloc_named_fwnode("IPI-Mux");// 创建一个名为 "IPI-Mux" 的设备节点
	if (!fwnode) {
		pr_err("unable to create IPI Mux fwnode\n");//如果创建失败，输出错误信息
		rc = -ENOMEM;//设置返回代码为 ENOMEM
		goto fail_free_cpu;//跳转到释放 CPU 数据结构的代码
	}

	domain = irq_domain_create_linear(fwnode, nr_ipi,
					  &ipi_mux_domain_ops, NULL);// 创建线性的中断域，用于管理 IPI 中断
	if (!domain) {
		pr_err("unable to add IPI Mux domain\n");//创建失败，输出错误信息
		rc = -ENOMEM;// 设置返回代码为 ENOMEM
		goto fail_free_fwnode;// 跳转到释放设备节点的代码
	}

	domain->flags |= IRQ_DOMAIN_FLAG_IPI_SINGLE;//设置中断域标志，表明这是一个单一的 IPI 域
	irq_domain_update_bus_token(domain, DOMAIN_BUS_IPI);//更新总线令牌，表示该域处理 IPI 中断

	rc = irq_domain_alloc_irqs(domain, nr_ipi, NUMA_NO_NODE, NULL);//从 IPI Mux 域中分配指定数量的中断请求线
	if (rc <= 0) {
		pr_err("unable to alloc IRQs from IPI Mux domain\n");
		goto fail_free_domain;//跳转到释放中断域的代码
	}

	ipi_mux_domain = domain;// 将新创建的中断域指针赋值给全局变量
	ipi_mux_send = mux_send;//将用户提供的发送函数指针保存

	return rc;//返回成功分配的 IRQ 数量

fail_free_domain:
	irq_domain_remove(domain);//释放之前创建的中断域资源
fail_free_fwnode:
	irq_domain_free_fwnode(fwnode);// 释放创建的设备节点资源
fail_free_cpu:
	free_percpu(ipi_mux_pcpu);//释放per_CPU 的 IPI Mux 数据结构
	return rc;//返回错误码
}
