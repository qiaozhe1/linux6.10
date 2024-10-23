// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017-2018 SiFive
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 */

#define pr_fmt(fmt) "riscv-intc: " fmt
#include <linux/acpi.h>
#include <linux/atomic.h>
#include <linux/bits.h>
#include <linux/cpu.h>
#include <linux/irq.h>
#include <linux/irqchip.h>
#include <linux/irqdomain.h>
#include <linux/interrupt.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/smp.h>
#include <linux/soc/andes/irq.h>

#include <asm/hwcap.h>

static struct irq_domain *intc_domain;
static unsigned int riscv_intc_nr_irqs __ro_after_init = BITS_PER_LONG;
static unsigned int riscv_intc_custom_base __ro_after_init = BITS_PER_LONG;
static unsigned int riscv_intc_custom_nr_irqs __ro_after_init;

static asmlinkage void riscv_intc_irq(struct pt_regs *regs)
{
	unsigned long cause = regs->cause & ~CAUSE_IRQ_FLAG;

	if (generic_handle_domain_irq(intc_domain, cause))
		pr_warn_ratelimited("Failed to handle interrupt (cause: %ld)\n", cause);
}

static asmlinkage void riscv_intc_aia_irq(struct pt_regs *regs)
{
	unsigned long topi;

	while ((topi = csr_read(CSR_TOPI)))
		generic_handle_domain_irq(intc_domain, topi >> TOPI_IID_SHIFT);
}

/*
 * On RISC-V systems local interrupts are masked or unmasked by writing
 * the SIE (Supervisor Interrupt Enable) CSR.  As CSRs can only be written
 * on the local hart, these functions can only be called on the hart that
 * corresponds to the IRQ chip.
 */

static void riscv_intc_irq_mask(struct irq_data *d)
{
	if (IS_ENABLED(CONFIG_32BIT) && d->hwirq >= BITS_PER_LONG)
		csr_clear(CSR_IEH, BIT(d->hwirq - BITS_PER_LONG));
	else
		csr_clear(CSR_IE, BIT(d->hwirq));
}

static void riscv_intc_irq_unmask(struct irq_data *d)
{
	if (IS_ENABLED(CONFIG_32BIT) && d->hwirq >= BITS_PER_LONG)
		csr_set(CSR_IEH, BIT(d->hwirq - BITS_PER_LONG));
	else
		csr_set(CSR_IE, BIT(d->hwirq));
}

static void andes_intc_irq_mask(struct irq_data *d)
{
	/*
	 * Andes specific S-mode local interrupt causes (hwirq)
	 * are defined as (256 + n) and controlled by n-th bit
	 * of SLIE.
	 */
	unsigned int mask = BIT(d->hwirq % BITS_PER_LONG);

	if (d->hwirq < ANDES_SLI_CAUSE_BASE)
		csr_clear(CSR_IE, mask);
	else
		csr_clear(ANDES_CSR_SLIE, mask);
}

static void andes_intc_irq_unmask(struct irq_data *d)
{
	unsigned int mask = BIT(d->hwirq % BITS_PER_LONG);

	if (d->hwirq < ANDES_SLI_CAUSE_BASE)
		csr_set(CSR_IE, mask);
	else
		csr_set(ANDES_CSR_SLIE, mask);
}

static void riscv_intc_irq_eoi(struct irq_data *d)
{
	/*
	 * The RISC-V INTC driver uses handle_percpu_devid_irq() flow
	 * for the per-HART local interrupts and child irqchip drivers
	 * (such as PLIC, SBI IPI, CLINT, APLIC, IMSIC, etc) implement
	 * chained handlers for the per-HART local interrupts.
	 *
	 * In the absence of irq_eoi(), the chained_irq_enter() and
	 * chained_irq_exit() functions (used by child irqchip drivers)
	 * will do unnecessary mask/unmask of per-HART local interrupts
	 * at the time of handling interrupts. To avoid this, we provide
	 * an empty irq_eoi() callback for RISC-V INTC irqchip.
	 */
}

static struct irq_chip riscv_intc_chip = {
	.name = "RISC-V INTC",
	.irq_mask = riscv_intc_irq_mask,
	.irq_unmask = riscv_intc_irq_unmask,
	.irq_eoi = riscv_intc_irq_eoi,
};

static struct irq_chip andes_intc_chip = {
	.name		= "RISC-V INTC",
	.irq_mask	= andes_intc_irq_mask,
	.irq_unmask	= andes_intc_irq_unmask,
	.irq_eoi	= riscv_intc_irq_eoi,
};

static int riscv_intc_domain_map(struct irq_domain *d, unsigned int irq,
				 irq_hw_number_t hwirq)
{
	struct irq_chip *chip = d->host_data;//获取与中断域关联的中断芯片数据

	irq_set_percpu_devid(irq);//设置该中断为per_CPU 唯一的设备 ID，表示每个 CPU 都会有一个独立的中断处理
	irq_domain_set_info(d, irq, hwirq, chip, NULL, handle_percpu_devid_irq,
			    NULL, NULL);//设置中断描述符的相关信息，包括中断芯片、处理函数等

	return 0;//返回 0 表示映射成功
}

static int riscv_intc_domain_alloc(struct irq_domain *domain,
				   unsigned int virq, unsigned int nr_irqs,
				   void *arg)
{
	int i, ret;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	struct irq_fwspec *fwspec = arg;

	ret = irq_domain_translate_onecell(domain, fwspec, &hwirq, &type);
	if (ret)
		return ret;

	/*
	 * Only allow hwirq for which we have corresponding standard or
	 * custom interrupt enable register.
	 */
	if (hwirq >= riscv_intc_nr_irqs &&
	    (hwirq < riscv_intc_custom_base ||
	     hwirq >= riscv_intc_custom_base + riscv_intc_custom_nr_irqs))
		return -EINVAL;

	for (i = 0; i < nr_irqs; i++) {
		ret = riscv_intc_domain_map(domain, virq + i, hwirq + i);
		if (ret)
			return ret;
	}

	return 0;
}

static const struct irq_domain_ops riscv_intc_domain_ops = {
	.map	= riscv_intc_domain_map,
	.xlate	= irq_domain_xlate_onecell,
	.alloc	= riscv_intc_domain_alloc
};

static struct fwnode_handle *riscv_intc_hwnode(void)
{
	return intc_domain->fwnode;
}

static int __init riscv_intc_init_common(struct fwnode_handle *fn, struct irq_chip *chip)
{
	int rc;

	intc_domain = irq_domain_create_tree(fn, &riscv_intc_domain_ops, chip);//创建 IRQ 域（irq_domain）来管理中断号的映射，使用提供的设备节点句柄 `fn` 和中断处理集合 `chip`
	if (!intc_domain) {
		pr_err("unable to add IRQ domain\n");
		return -ENXIO;//返回错误代码 -ENXIO 表示没有这样的设备或地址
	}
	/*检查 RISC-V ISA 扩展是否支持 AIA（Advanced Interrupt Architecture）*/
	if (riscv_isa_extension_available(NULL, SxAIA)) {
		riscv_intc_nr_irqs = 64;//如果支持 AIA，则中断数量为 64
		rc = set_handle_irq(&riscv_intc_aia_irq);// 设置 AIA 模式下的中断处理函数
	} else {
		rc = set_handle_irq(&riscv_intc_irq);//否则，设置常规模式的中断处理函数
	}
	if (rc) {
		pr_err("failed to set irq handler\n");//如果设置中断处理程序失败，打印错误信息
		return rc;
	}

	riscv_set_intc_hwnode_fn(riscv_intc_hwnode);//设置硬件中断节点的回调函数，用于指定 INTC 的硬件节点

	pr_info("%d local interrupts mapped%s\n",
		riscv_intc_nr_irqs,
		riscv_isa_extension_available(NULL, SxAIA) ? " using AIA" : "");//打印中断映射的信息，包括是否使用 AIA
	if (riscv_intc_custom_nr_irqs)
		pr_info("%d custom local interrupts mapped\n", riscv_intc_custom_nr_irqs);//如果定义了自定义的本地中断数量，则打印自定义中断的映射信息

	return 0;
}

static int __init riscv_intc_init(struct device_node *node,
				  struct device_node *parent)
{
	struct irq_chip *chip = &riscv_intc_chip;//初始化一个指向默认中断操作方法集合的指针
	unsigned long hartid;//用于存储硬件线程 ID
	int rc;

	rc = riscv_of_parent_hartid(node, &hartid);//获取与设备节点 `node` 关联的硬件线程 ID（HART ID）
	if (rc < 0) {
		pr_warn("unable to find hart id for %pOF\n", node);//如果无法找到 HART ID，输出警告信息
		return 0;//返回 0 表示初始化未能继续
	}

	/*
	 * The DT will have one INTC DT node under each CPU (or HART)
	 * DT node so riscv_intc_init() function will be called once
	 * for each INTC DT node. We only need to do INTC initialization
	 * for the INTC DT node belonging to boot CPU (or boot HART).
	 * 设备树（DT）中，每个 CPU（HART）节点下会有一个 INTC（中断控制器）节点，
	 *  因此 `riscv_intc_init()` 函数会为每个 INTC 节点调用一次.但我们只需要
	 *  对属于引导 CPU（引导 HART）的 INTC 节点进行初始化
	 */
	if (riscv_hartid_to_cpuid(hartid) != smp_processor_id()) {//通过 riscv_hartid_to_cpuid(hartid) 将 HART ID 转换为 CPU ID，并与当前处理器 ID (smp_processor_id()) 进行比较，如果两者不匹配，则表示该 INTC 节点不属于引导 CPU，不需要进一步初始化。
		/*
		 * The INTC nodes of each CPU are suppliers for downstream
		 * interrupt controllers (such as PLIC, IMSIC and APLIC
		 * direct-mode) so we should mark an INTC node as initialized
		 * if we are not creating IRQ domain for it.
		 * 每个 CPU 的 INTC 节点为下游的中断控制器（如 PLIC、IMSIC 和 APLIC direct-mode）提供中断信息，
		 * 因此即使不为它创建 IRQ 域，也应该标记 INTC 节点为已初始化，以便其他中断控制器能够使用它。
		 */
		fwnode_dev_initialized(of_fwnode_handle(node), true);//标记此设备节点为已初始化
		return 0;//返回 0，表示不需要进一步初始化
	}
	/*如果设备节点与 "andestech,cpu-intc" 兼容，则选择 Andes 特定的中断控制器芯片*/
	if (of_device_is_compatible(node, "andestech,cpu-intc")) {
		riscv_intc_custom_base = ANDES_SLI_CAUSE_BASE;//设置 Andes 特定的中断原因寄存器基地址
		riscv_intc_custom_nr_irqs = ANDES_RV_IRQ_LAST;//设置 Andes 特定的中断数量
		chip = &andes_intc_chip;// 使用 Andes 特定的中断方法集合代替默认的RISC-V 中断方法集合
	}

	return riscv_intc_init_common(of_node_to_fwnode(node), chip);//使用提供的中断chip调用 `riscv_intc_init_common()`，完成通用的中断控制器初始化
}

IRQCHIP_DECLARE(riscv, "riscv,cpu-intc", riscv_intc_init);
IRQCHIP_DECLARE(andes, "andestech,cpu-intc", riscv_intc_init);

#ifdef CONFIG_ACPI

static int __init riscv_intc_acpi_init(union acpi_subtable_headers *header,
				       const unsigned long end)
{
	struct acpi_madt_rintc *rintc;
	struct fwnode_handle *fn;
	int rc;

	rintc = (struct acpi_madt_rintc *)header;

	/*
	 * The ACPI MADT will have one INTC for each CPU (or HART)
	 * so riscv_intc_acpi_init() function will be called once
	 * for each INTC. We only do INTC initialization
	 * for the INTC belonging to the boot CPU (or boot HART).
	 */
	if (riscv_hartid_to_cpuid(rintc->hart_id) != smp_processor_id())
		return 0;

	fn = irq_domain_alloc_named_fwnode("RISCV-INTC");
	if (!fn) {
		pr_err("unable to allocate INTC FW node\n");
		return -ENOMEM;
	}

	rc = riscv_intc_init_common(fn, &riscv_intc_chip);
	if (rc)
		irq_domain_free_fwnode(fn);

	return rc;
}

IRQCHIP_ACPI_DECLARE(riscv_intc, ACPI_MADT_TYPE_RINTC, NULL,
		     ACPI_MADT_RINTC_VERSION_V1, riscv_intc_acpi_init);
#endif
