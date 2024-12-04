// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/ctype.h>
#include <linux/init.h>
#include <linux/seq_file.h>
#include <linux/of.h>
#include <asm/acpi.h>
#include <asm/cpufeature.h>
#include <asm/csr.h>
#include <asm/hwcap.h>
#include <asm/sbi.h>
#include <asm/smp.h>
#include <asm/pgtable.h>

bool arch_match_cpu_phys_id(int cpu, u64 phys_id)
{
	return phys_id == cpuid_to_hartid_map(cpu);
}

/*
 * Returns the hart ID of the given device tree node, or -ENODEV if the node
 * isn't an enabled and valid RISC-V hart node.
 */
int riscv_of_processor_hartid(struct device_node *node, unsigned long *hart)
{
	int cpu;

	*hart = (unsigned long)of_get_cpu_hwid(node, 0);
	if (*hart == ~0UL) {
		pr_warn("Found CPU without hart ID\n");
		return -ENODEV;
	}

	cpu = riscv_hartid_to_cpuid(*hart);
	if (cpu < 0)
		return cpu;

	if (!cpu_possible(cpu))
		return -ENODEV;

	return 0;
}
/*RISC-V 架构的早期处理器 hart ID 初始化函数*/
int __init riscv_early_of_processor_hartid(struct device_node *node, unsigned long *hart)
{
	const char *isa;//指向指令集架构（ISA）字符串的指针

	if (!of_device_is_compatible(node, "riscv")) {//检查设备节点是否与 "riscv" 兼容，判断处理器是否符合 RISC-V 规范
		pr_warn("Found incompatible CPU\n");//如果设备节点不兼容 RISC-V，打印警告信息，表示找到不兼容的 CPU
		return -ENODEV;// 返回 -ENODEV，表示设备不可用
	}

	*hart = (unsigned long)of_get_cpu_hwid(node, 0);// 从设备树节点中获取 CPU 的硬件 ID（hart ID），并将其存储到 hart 指针所指的地址中
	if (*hart == ~0UL) {//检查获取的 hart ID 是否为无效值（~0UL 表示无效）
		pr_warn("Found CPU without hart ID\n");// 如果 hart ID 无效，打印警告信息，表示找到一个没有 hart ID 的 CPU
		return -ENODEV;//返回 -ENODEV，表示设备不可用
	}

	if (!of_device_is_available(node)) {//检查设备节点是否被标记为可用
		pr_info("CPU with hartid=%lu is not available\n", *hart);//如果设备节点不可用，打印信息，包含该处理器的 hart ID
		return -ENODEV;//返回 -ENODEV，表示设备不可用
	}

	if (of_property_read_string(node, "riscv,isa-base", &isa))//尝试从设备节点中读取 "riscv,isa-base" 属性，以获取处理器的指令集基本信息
		goto old_interface;//如果无法读取到该属性，跳转到旧接口处理逻辑

	if (IS_ENABLED(CONFIG_32BIT) && strncasecmp(isa, "rv32i", 5)) {//如果系统配置为 32 位（CONFIG_32BIT），但 ISA 不以 "rv32i" 开头
		pr_warn("CPU with hartid=%lu does not support rv32i", *hart);// 打印警告信息，表示该处理器不支持 rv32i 指令集
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_64BIT) && strncasecmp(isa, "rv64i", 5)) {//如果系统配置为 64 位（CONFIG_64BIT），但 ISA 不以 "rv64i" 开头
		pr_warn("CPU with hartid=%lu does not support rv64i", *hart);//打印警告信息，表示该处理器不支持 rv64i 指令集
		return -ENODEV;//返回 -ENODEV，表示设备不可用
	}

	if (!of_property_present(node, "riscv,isa-extensions"))//检查设备节点中是否存在 "riscv,isa-extensions" 属性，表示该处理器的扩展指令集
		return -ENODEV;// 如果属性不存在，返回 -ENODEV，表示设备不可用

	if (of_property_match_string(node, "riscv,isa-extensions", "i") < 0 ||//检查设备节点的 "riscv,isa-extensions" 属性中是否包含 "i" 扩展指令集
	    of_property_match_string(node, "riscv,isa-extensions", "m") < 0 ||//检查设备节点的 "riscv,isa-extensions" 属性中是否包含 "m" 扩展指令集
	    of_property_match_string(node, "riscv,isa-extensions", "a") < 0) {//检查设备节点的 "riscv,isa-extensions" 属性中是否包含 "a" 扩展指令集
		pr_warn("CPU with hartid=%lu does not support ima", *hart);//如果 "i", "m", "a" 其中之一缺失，打印警告信息，表示处理器不支持完整的 "ima" 指令集扩展
		return -ENODEV;//返回 -ENODEV，表示设备不可用
	}

	return 0;//所有检查通过，返回 0，表示处理器初始化成功

old_interface://旧接口处理逻辑标签
	if (!riscv_isa_fallback) {//如果没有启用旧接口解析功能（riscv_isa_fallback 为 false）
		pr_warn("CPU with hartid=%lu is invalid: this kernel does not parse \"riscv,isa\"",
			*hart);//打印警告信息，表示该处理器无效，因为内核不支持解析旧的 "riscv,isa" 属性
		return -ENODEV;//返回 -ENODEV，表示设备不可用
	}

	if (of_property_read_string(node, "riscv,isa", &isa)) {//尝试读取设备节点中的 "riscv,isa" 属性
		pr_warn("CPU with hartid=%lu has no \"riscv,isa-base\" or \"riscv,isa\" property\n",
			*hart);//如果未找到属性，打印警告信息，表示处理器没有 "riscv,isa-base" 或 "riscv,isa" 属性
		return -ENODEV;
	}

	if (IS_ENABLED(CONFIG_32BIT) && strncasecmp(isa, "rv32ima", 7)) {//如果系统配置为 32 位，但 ISA 不以 "rv32ima" 开头，则不支持 RV32IMA 指令集
		pr_warn("CPU with hartid=%lu does not support rv32ima", *hart);//打印警告信息，表示处理器不支持 rv32ima 指令集
		return -ENODEV;// 返回 -ENODEV，表示设备不可用
	}

	if (IS_ENABLED(CONFIG_64BIT) && strncasecmp(isa, "rv64ima", 7)) {//如果系统配置为 64 位，但 ISA 不以 "rv64ima" 开头，则不支持 RV64IMA 指令集
		pr_warn("CPU with hartid=%lu does not support rv64ima", *hart);//打印警告信息，表示处理器不支持 rv64ima 指令集
		return -ENODEV;
	}

	return 0;//所有检查通过，返回 0，表示处理器初始化成功
}

/*
 * Find hart ID of the CPU DT node under which given DT node falls.
 *
 * To achieve this, we walk up the DT tree until we find an active
 * RISC-V core (HART) node and extract the cpuid from it.
 */
int riscv_of_parent_hartid(struct device_node *node, unsigned long *hartid)
{
	for (; node; node = node->parent) {
		if (of_device_is_compatible(node, "riscv")) {
			*hartid = (unsigned long)of_get_cpu_hwid(node, 0);
			if (*hartid == ~0UL) {
				pr_warn("Found CPU without hart ID\n");
				return -ENODEV;
			}
			return 0;
		}
	}

	return -1;
}

unsigned long __init riscv_get_marchid(void)
{
	struct riscv_cpuinfo *ci = this_cpu_ptr(&riscv_cpuinfo);

#if IS_ENABLED(CONFIG_RISCV_SBI)
	ci->marchid = sbi_spec_is_0_1() ? 0 : sbi_get_marchid();
#elif IS_ENABLED(CONFIG_RISCV_M_MODE)
	ci->marchid = csr_read(CSR_MARCHID);
#else
	ci->marchid = 0;
#endif
	return ci->marchid;
}

unsigned long __init riscv_get_mvendorid(void)
{
	struct riscv_cpuinfo *ci = this_cpu_ptr(&riscv_cpuinfo);

#if IS_ENABLED(CONFIG_RISCV_SBI)
	ci->mvendorid = sbi_spec_is_0_1() ? 0 : sbi_get_mvendorid();
#elif IS_ENABLED(CONFIG_RISCV_M_MODE)
	ci->mvendorid = csr_read(CSR_MVENDORID);
#else
	ci->mvendorid = 0;
#endif
	return ci->mvendorid;
}

DEFINE_PER_CPU(struct riscv_cpuinfo, riscv_cpuinfo);

unsigned long riscv_cached_mvendorid(unsigned int cpu_id)
{
	struct riscv_cpuinfo *ci = per_cpu_ptr(&riscv_cpuinfo, cpu_id);

	return ci->mvendorid;
}
EXPORT_SYMBOL(riscv_cached_mvendorid);

unsigned long riscv_cached_marchid(unsigned int cpu_id)
{
	struct riscv_cpuinfo *ci = per_cpu_ptr(&riscv_cpuinfo, cpu_id);

	return ci->marchid;
}
EXPORT_SYMBOL(riscv_cached_marchid);

unsigned long riscv_cached_mimpid(unsigned int cpu_id)
{
	struct riscv_cpuinfo *ci = per_cpu_ptr(&riscv_cpuinfo, cpu_id);

	return ci->mimpid;
}
EXPORT_SYMBOL(riscv_cached_mimpid);

static int riscv_cpuinfo_starting(unsigned int cpu)
{
	struct riscv_cpuinfo *ci = this_cpu_ptr(&riscv_cpuinfo);

#if IS_ENABLED(CONFIG_RISCV_SBI)
	if (!ci->mvendorid)
		ci->mvendorid = sbi_spec_is_0_1() ? 0 : sbi_get_mvendorid();
	if (!ci->marchid)
		ci->marchid = sbi_spec_is_0_1() ? 0 : sbi_get_marchid();
	ci->mimpid = sbi_spec_is_0_1() ? 0 : sbi_get_mimpid();
#elif IS_ENABLED(CONFIG_RISCV_M_MODE)
	if (!ci->mvendorid)
		ci->mvendorid = csr_read(CSR_MVENDORID);
	if (!ci->marchid)
		ci->marchid = csr_read(CSR_MARCHID);
	ci->mimpid = csr_read(CSR_MIMPID);
#else
	ci->mvendorid = 0;
	ci->marchid = 0;
	ci->mimpid = 0;
#endif

	return 0;
}

static int __init riscv_cpuinfo_init(void)
{
	int ret;

	ret = cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "riscv/cpuinfo:starting",
				riscv_cpuinfo_starting, NULL);
	if (ret < 0) {
		pr_err("cpuinfo: failed to register hotplug callbacks.\n");
		return ret;
	}

	return 0;
}
arch_initcall(riscv_cpuinfo_init);

#ifdef CONFIG_PROC_FS

static void print_isa(struct seq_file *f, const unsigned long *isa_bitmap)
{

	if (IS_ENABLED(CONFIG_32BIT))
		seq_write(f, "rv32", 4);
	else
		seq_write(f, "rv64", 4);

	for (int i = 0; i < riscv_isa_ext_count; i++) {
		if (!__riscv_isa_extension_available(isa_bitmap, riscv_isa_ext[i].id))
			continue;

		/* Only multi-letter extensions are split by underscores */
		if (strnlen(riscv_isa_ext[i].name, 2) != 1)
			seq_puts(f, "_");

		seq_printf(f, "%s", riscv_isa_ext[i].name);
	}

	seq_puts(f, "\n");
}

static void print_mmu(struct seq_file *f)
{
	const char *sv_type;

#ifdef CONFIG_MMU
#if defined(CONFIG_32BIT)
	sv_type = "sv32";
#elif defined(CONFIG_64BIT)
	if (pgtable_l5_enabled)
		sv_type = "sv57";
	else if (pgtable_l4_enabled)
		sv_type = "sv48";
	else
		sv_type = "sv39";
#endif
#else
	sv_type = "none";
#endif /* CONFIG_MMU */
	seq_printf(f, "mmu\t\t: %s\n", sv_type);
}

static void *c_start(struct seq_file *m, loff_t *pos)
{
	if (*pos == nr_cpu_ids)
		return NULL;

	*pos = cpumask_next(*pos - 1, cpu_online_mask);
	if ((*pos) < nr_cpu_ids)
		return (void *)(uintptr_t)(1 + *pos);
	return NULL;
}

static void *c_next(struct seq_file *m, void *v, loff_t *pos)
{
	(*pos)++;
	return c_start(m, pos);
}

static void c_stop(struct seq_file *m, void *v)
{
}

static int c_show(struct seq_file *m, void *v)
{
	unsigned long cpu_id = (unsigned long)v - 1;
	struct riscv_cpuinfo *ci = per_cpu_ptr(&riscv_cpuinfo, cpu_id);
	struct device_node *node;
	const char *compat;

	seq_printf(m, "processor\t: %lu\n", cpu_id);
	seq_printf(m, "hart\t\t: %lu\n", cpuid_to_hartid_map(cpu_id));

	/*
	 * For historical raisins, the isa: line is limited to the lowest common
	 * denominator of extensions supported across all harts. A true list of
	 * extensions supported on this hart is printed later in the hart isa:
	 * line.
	 */
	seq_puts(m, "isa\t\t: ");
	print_isa(m, NULL);
	print_mmu(m);

	if (acpi_disabled) {
		node = of_get_cpu_node(cpu_id, NULL);

		if (!of_property_read_string(node, "compatible", &compat) &&
		    strcmp(compat, "riscv"))
			seq_printf(m, "uarch\t\t: %s\n", compat);

		of_node_put(node);
	}

	seq_printf(m, "mvendorid\t: 0x%lx\n", ci->mvendorid);
	seq_printf(m, "marchid\t\t: 0x%lx\n", ci->marchid);
	seq_printf(m, "mimpid\t\t: 0x%lx\n", ci->mimpid);

	/*
	 * Print the ISA extensions specific to this hart, which may show
	 * additional extensions not present across all harts.
	 */
	seq_puts(m, "hart isa\t: ");
	print_isa(m, hart_isa[cpu_id].isa);
	seq_puts(m, "\n");

	return 0;
}

const struct seq_operations cpuinfo_op = {
	.start	= c_start,
	.next	= c_next,
	.stop	= c_stop,
	.show	= c_show
};

#endif /* CONFIG_PROC_FS */
