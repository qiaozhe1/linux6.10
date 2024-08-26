// SPDX-License-Identifier: GPL-2.0-only
/*
 * SMP initialisation and IPI support
 * Based on arch/arm64/kernel/smp.c
 *
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2015 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */

#include <linux/acpi.h>
#include <linux/arch_topology.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include <linux/sched.h>
#include <linux/kernel_stat.h>
#include <linux/notifier.h>
#include <linux/cpu.h>
#include <linux/percpu.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/sched/task_stack.h>
#include <linux/sched/mm.h>

#include <asm/cacheflush.h>
#include <asm/cpu_ops.h>
#include <asm/irq.h>
#include <asm/mmu_context.h>
#include <asm/numa.h>
#include <asm/tlbflush.h>
#include <asm/sections.h>
#include <asm/smp.h>
#include <uapi/asm/hwcap.h>
#include <asm/vector.h>

#include "head.h"

static DECLARE_COMPLETION(cpu_running);

void __init smp_prepare_cpus(unsigned int max_cpus)
{
	int cpuid;
	unsigned int curr_cpuid;

	init_cpu_topology();

	curr_cpuid = smp_processor_id();
	store_cpu_topology(curr_cpuid);
	numa_store_cpu_info(curr_cpuid);
	numa_add_cpu(curr_cpuid);

	/* This covers non-smp usecase mandated by "nosmp" option */
	if (max_cpus == 0)
		return;

	for_each_possible_cpu(cpuid) {
		if (cpuid == curr_cpuid)
			continue;
		set_cpu_present(cpuid, true);
		numa_store_cpu_info(cpuid);
	}
}

#ifdef CONFIG_ACPI
static unsigned int cpu_count = 1;

static int __init acpi_parse_rintc(union acpi_subtable_headers *header, const unsigned long end)
{
	unsigned long hart;
	static bool found_boot_cpu;
	struct acpi_madt_rintc *processor = (struct acpi_madt_rintc *)header;

	/*
	 * Each RINTC structure in MADT will have a flag. If ACPI_MADT_ENABLED
	 * bit in the flag is not enabled, it means OS should not try to enable
	 * the cpu to which RINTC belongs.
	 */
	if (!(processor->flags & ACPI_MADT_ENABLED))
		return 0;

	if (BAD_MADT_ENTRY(processor, end))
		return -EINVAL;

	acpi_table_print_madt_entry(&header->common);

	hart = processor->hart_id;
	if (hart == INVALID_HARTID) {
		pr_warn("Invalid hartid\n");
		return 0;
	}

	if (hart == cpuid_to_hartid_map(0)) {
		BUG_ON(found_boot_cpu);
		found_boot_cpu = true;
		early_map_cpu_to_node(0, acpi_numa_get_nid(cpu_count));
		return 0;
	}

	if (cpu_count >= NR_CPUS) {
		pr_warn("NR_CPUS is too small for the number of ACPI tables.\n");
		return 0;
	}

	cpuid_to_hartid_map(cpu_count) = hart;
	early_map_cpu_to_node(cpu_count, acpi_numa_get_nid(cpu_count));
	cpu_count++;

	return 0;
}

static void __init acpi_parse_and_init_cpus(void)
{
	acpi_table_parse_madt(ACPI_MADT_TYPE_RINTC, acpi_parse_rintc, 0);
}
#else
#define acpi_parse_and_init_cpus(...)	do { } while (0)
#endif

static bool __init is_mpidr_duplicate(unsigned int cpu, u64 hwid)
{
	unsigned int i;

	for (i = 1; (i < cpu) && (i < NR_CPUS); i++)
		if (cpuid_to_hartid_map(i) == hwid)
			return true;
	return false;
}

/*解析设备树中的 CPU 节点并初始化多核 CPU*/
static void __init of_parse_and_init_cpus(void)
{
	struct device_node *dn;//声明指向设备节点的指针
	unsigned long hart;//声明 hart ID 变量
	bool found_boot_cpu = false;//标记是否找到启动CPU
	int cpuid = 1;//初始化CPU ID为1（CPU 0 是启动 CPU）
	int rc;

	for_each_of_cpu_node(dn) {//遍历每一个CPU节点
		rc = riscv_early_of_processor_hartid(dn, &hart);//获取设备节点的 hart ID
		if (rc < 0)//如果获取hart ID失败，跳过当前节点
			continue;

		if (is_mpidr_duplicate(cpuid, hart)) {
			pr_err("%pOF: duplicate cpu reg properties in the DT\n",
				dn);
			continue;
		}

		if (hart == cpuid_to_hartid_map(0)) {//检查是否为启动CPU
			BUG_ON(found_boot_cpu);//如果已经找到过启动 CPU，则触发内核错误
			found_boot_cpu = 1;//标记已经找到启动 CPU
			early_map_cpu_to_node(0, of_node_to_nid(dn));//将启动 CPU 映射到对应的 NUMA 节点
			continue;//跳过对启动 CPU 的进一步处理
		}
		if (cpuid >= NR_CPUS) {//如果 CPU ID 超过系统支持的最大 CPU 数量
			pr_warn("Invalid cpuid [%d] for hartid [%lu]\n",
				cpuid, hart);//输出警告信息
			continue;//跳过当前 CPU
		}

		cpuid_to_hartid_map(cpuid) = hart;//将 hart ID 映射到相应的 CPU ID
		early_map_cpu_to_node(cpuid, of_node_to_nid(dn));//将 CPU ID 映射到对应的 NUMA 节点
		cpuid++;//增加 CPU ID 以处理下一个 CPU
	}

	BUG_ON(!found_boot_cpu);//如果未找到启动 CPU，则触发内核错误

	if (cpuid > nr_cpu_ids)// 如果实际的 CPU 数量超过系统配置的 CPU 数量
		pr_warn("Total number of cpus [%d] is greater than nr_cpus option value [%d]\n",
			cpuid, nr_cpu_ids);//输出警告信息
}

void __init setup_smp(void)//用于设置对称多处理 (SMP) 系统的相关配置。
{
	int cpuid;// 声明一个整数变量 cpuid，用于存储 CPU 的 ID

	cpu_set_ops();//设置与 CPU 相关的操作函数指针

	if (acpi_disabled)//如果 ACPI 被禁用，则解析设备树并初始化 CPU
		of_parse_and_init_cpus();
	else
		acpi_parse_and_init_cpus();//如果 ACPI 启用，则解析 ACPI 表并初始化 CPU

	for (cpuid = 1; cpuid < nr_cpu_ids; cpuid++)//遍历从 1 到 nr_cpu_ids 的所有 CPU ID（跳过 CPU 0）
		if (cpuid_to_hartid_map(cpuid) != INVALID_HARTID)//检查给定 cpuid 对应的 hart ID 是否有效（不是 INVALID_HARTID）。
			set_cpu_possible(cpuid, true);//将该cpuid标记为系统中存在的CPU,这意味着这个 CPU 可以在系统中启用并参与工作负载
}

static int start_secondary_cpu(int cpu, struct task_struct *tidle)
{
	if (cpu_ops->cpu_start)
		return cpu_ops->cpu_start(cpu, tidle);

	return -EOPNOTSUPP;
}

int __cpu_up(unsigned int cpu, struct task_struct *tidle)
{
	int ret = 0;
	tidle->thread_info.cpu = cpu;

	ret = start_secondary_cpu(cpu, tidle);
	if (!ret) {
		wait_for_completion_timeout(&cpu_running,
					    msecs_to_jiffies(1000));

		if (!cpu_online(cpu)) {
			pr_crit("CPU%u: failed to come online\n", cpu);
			ret = -EIO;
		}
	} else {
		pr_crit("CPU%u: failed to start\n", cpu);
	}

	return ret;
}

void __init smp_cpus_done(unsigned int max_cpus)
{
}

/*
 * C entry point for a secondary processor.
 */
asmlinkage __visible void smp_callin(void)
{
	struct mm_struct *mm = &init_mm;
	unsigned int curr_cpuid = smp_processor_id();

	/* All kernel threads share the same mm context.  */
	mmgrab(mm);
	current->active_mm = mm;

	store_cpu_topology(curr_cpuid);
	notify_cpu_starting(curr_cpuid);

	riscv_ipi_enable();

	numa_add_cpu(curr_cpuid);
	set_cpu_online(curr_cpuid, true);

	if (has_vector()) {
		if (riscv_v_setup_vsize())
			elf_hwcap &= ~COMPAT_HWCAP_ISA_V;
	}

	riscv_user_isa_enable();

	/*
	 * Remote cache and TLB flushes are ignored while the CPU is offline,
	 * so flush them both right now just in case.
	 */
	local_flush_icache_all();
	local_flush_tlb_all();
	complete(&cpu_running);
	/*
	 * Disable preemption before enabling interrupts, so we don't try to
	 * schedule a CPU that hasn't actually started yet.
	 */
	local_irq_enable();
	cpu_startup_entry(CPUHP_AP_ONLINE_IDLE);
}
