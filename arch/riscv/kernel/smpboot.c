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
/*用于在多处理器系统中为每个 CPU 设置拓扑结构和 NUMA（非统一内存访问）信息。*/
void __init smp_prepare_cpus(unsigned int max_cpus)
{
	int cpuid;//定义变量 `cpuid` 用于遍历每个可能的 CPU ID
	unsigned int curr_cpuid;//定义变量 `curr_cpuid`，用于存储当前运行的 CPU ID

	init_cpu_topology();//初始化 CPU 拓扑结构，为系统设置各个 CPU 的硬件结构信息(主要为缓存信息)

	curr_cpuid = smp_processor_id();//获取当前运行 CPU 的 ID，并将其赋值给 `curr_cpuid`
	store_cpu_topology(curr_cpuid);//存储当前 CPU 的拓扑信息，以便后续多核处理的使用
	numa_store_cpu_info(curr_cpuid);//存储当前 CPU 的 NUMA（非统一内存访问）信息，确保多节点系统中内存访问的信息正确配置
	numa_add_cpu(curr_cpuid);//将当前 CPU 添加到 NUMA 拓扑中，设置它在 NUMA 结构中的位置

	/* This covers non-smp usecase mandated by "nosmp" option */
	if (max_cpus == 0)//如果系统最大 CPU 数设置为 0（通过 "nosmp" 选项禁用 SMP）
		return;//直接返回

	for_each_possible_cpu(cpuid) {// 遍历系统中每一个可能存在的 CPU
		if (cpuid == curr_cpuid)//如果当前遍历的 CPU ID 与当前 CPU 相同，跳过
			continue;
		set_cpu_present(cpuid, true);//设置当前遍历的 CPU 为 "present" 状态，表示该 CPU 存在于系统中，可以被使用
		numa_store_cpu_info(cpuid);//存储该 CPU 的 NUMA 信息，确保系统内所有 CPU 的内存访问信息都被正确配置
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

		if (is_mpidr_duplicate(cpuid, hart)) {//检查由 cpuid 和 hart 标识的当前 CPU 是否在设备树中存在重复的 MPIDR（多处理器 ID 寄存器）。
			pr_err("%pOF: duplicate cpu reg properties in the DT\n",
				dn);//如果发现重复的 MPIDR，调用 pr_err() 输出一条错误信息，格式化字符串中使用了 %pOF，它表示以设备树节点的格式打印指针 dn（指向当前处理器节点的指针）
			continue;
		}

		if (hart == cpuid_to_hartid_map(0)) {//检查是否为启动CPU
			BUG_ON(found_boot_cpu);//如果已经找到过启动CPU，则触发内核错误
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

static int start_secondary_cpu(int cpu, struct task_struct *tidle)//用于启动指定的 CPU
{
	if (cpu_ops->cpu_start)//检查 CPU 操作接口中是否存在启动函数
		return cpu_ops->cpu_start(cpu, tidle);//如果存在，则调用该启动函数(sbi_cpu_start)，传递 CPU ID 和空闲线程

	return -EOPNOTSUPP;//如果启动函数不存在，则返回 -EOPNOTSUPP，表示操作不被支持
}

int __cpu_up(unsigned int cpu, struct task_struct *tidle)//用于启动指定 CPU，分配相应的任务结构
{
	int rvet = 0;//
	tidle->thread_info.cpu = cpu;//将指定的 CPU 分配给任务结构中的 thread_info

	ret = start_secondary_cpu(cpu, tidle);//启动指定的 CPU，tidle 是空闲线程
	if (!ret) {//如果启动成功，继续等待 CPU 完全启动
		wait_for_completion_timeout(&cpu_running,
					    msecs_to_jiffies(1000));//等待启动完成的信号，超时时间为 1000 毫秒（转换为时钟节拍 jiffies）

		if (!cpu_online(cpu)) {//检查 CPU 是否已经成功上线（进入工作状态）
			pr_crit("CPU%u: failed to come online\n", cpu);//如果 CPU 没有上线，则打印错误信息并将返回值设为 -EIO，表示输入/输出错误
			ret = -EIO;
		}
	} else {
		pr_crit("CPU%u: failed to start\n", cpu);//如果启动过程发生错误，则打印启动失败的错误信息
	}

	return ret;//返回启动的结果，0 表示成功，负值表示失败
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
