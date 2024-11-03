// SPDX-License-Identifier: GPL-2.0-only
/*
 * Multiplex several IPIs over a single HW IPI.
 *
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#define pr_fmt(fmt) "riscv: " fmt
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/irq.h>
#include <linux/irqchip/chained_irq.h>
#include <linux/irqdomain.h>
#include <asm/sbi.h>

DEFINE_STATIC_KEY_FALSE(riscv_sbi_for_rfence);
EXPORT_SYMBOL_GPL(riscv_sbi_for_rfence);

static int sbi_ipi_virq;

static void sbi_ipi_handle(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);

	chained_irq_enter(chip, desc);

	csr_clear(CSR_IP, IE_SIE);
	ipi_mux_process();

	chained_irq_exit(chip, desc);
}

static int sbi_ipi_starting_cpu(unsigned int cpu)
{
	enable_percpu_irq(sbi_ipi_virq, irq_get_trigger_type(sbi_ipi_virq));
	return 0;
}

void __init sbi_ipi_init(void)
{
	int virq;
	struct irq_domain *domain;

	if (riscv_ipi_have_virq_range())//如果 IPI（Inter-Processor Interrupt）已经有虚拟中断范围，直接返回
		return;

	domain = irq_find_matching_fwnode(riscv_get_intc_hwnode(),
					  DOMAIN_BUS_ANY);//查找与INTC硬件节点匹配的IRQ domain(这块直接返回的是intc_domain)
	if (!domain) {
		pr_err("unable to find INTC IRQ domain\n");//如果找不到中断域，打印错误信息
		return;//返回，表示初始化失败
	}

	sbi_ipi_virq = irq_create_mapping(domain, RV_IRQ_SOFT);//创建中断映射，将硬件中断号RV_IRQ_SOFT=3映射到逻辑中断号
	if (!sbi_ipi_virq) {
		pr_err("unable to create INTC IRQ mapping\n");//如果映射失败，打印错误信息
		return;
	}

	virq = ipi_mux_create(BITS_PER_BYTE, sbi_send_ipi);//创建多路复用 IPI 的虚拟中断号
	if (virq <= 0) {
		pr_err("unable to create muxed IPIs\n");//如果创建失败，打印错误信息
		irq_dispose_mapping(sbi_ipi_virq);// 释放之前创建的中断映射
		return;
	}

	irq_set_chained_handler(sbi_ipi_virq, sbi_ipi_handle);//设置链式中断处理程序，用于处理 IPI 中断

	/*
	 * Don't disable IPI when CPU goes offline because
	 * the masking/unmasking of virtual IPIs is done
	 * via generic IPI-Mux
	 * 在 CPU 下线时不要禁用 IPI，因为虚拟 IPI 的屏蔽/取消屏蔽是通过通用的 IPI-Mux 完成的。
	 */
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN,
			  "irqchip/sbi-ipi:starting",
			  sbi_ipi_starting_cpu, NULL);

	riscv_ipi_set_virq_range(virq, BITS_PER_BYTE);// 设置虚拟 IPI 中断号范围
	pr_info("providing IPIs using SBI IPI extension\n");

	/*
	 * Use the SBI remote fence extension to avoid
	 * the extra context switch needed to handle IPIs.
	 * 使用 SBI 的远程屏障扩展，以避免处理 IPI 所需的额外上下文切换。
	 */
	static_branch_enable(&riscv_sbi_for_rfence);
}
