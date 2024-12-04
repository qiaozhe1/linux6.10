// SPDX-License-Identifier: GPL-2.0-only
/*
 * HSM extension and cpu_ops implementation.
 *
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 */

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/sched/task_stack.h>
#include <asm/cpu_ops.h>
#include <asm/cpu_ops_sbi.h>
#include <asm/sbi.h>
#include <asm/smp.h>

extern char secondary_start_sbi[];
const struct cpu_operations cpu_ops_sbi;

/*
 * Ordered booting via HSM brings one cpu at a time. However, cpu hotplug can
 * be invoked from multiple threads in parallel. Define a per cpu data
 * to handle that.
 */
static DEFINE_PER_CPU(struct sbi_hart_boot_data, boot_data);
/*用于通过 SBI（Supervisor Binary Interface）启动指定的 hart（硬件线程）*/
static int sbi_hsm_hart_start(unsigned long hartid, unsigned long saddr,
			      unsigned long priv)
{
	struct sbiret ret;//定义一个 sbiret 结构体变量 ret，用于存储 SBI 调用的返回值

	ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_START,
			hartid, saddr, priv, 0, 0, 0);//使用 SBI 调用启动指定的 hart，传递 hart ID、启动地址和私有参数
	if (ret.error)//如果返回的错误码不为 0，表示启动 hart 失败
		return sbi_err_map_linux_errno(ret.error);//将 SBI 错误码映射为 Linux 错误码并返回
	else
		return 0;//启动成功，返回 0
}

#ifdef CONFIG_HOTPLUG_CPU
static int sbi_hsm_hart_stop(void)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STOP, 0, 0, 0, 0, 0, 0);

	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return 0;
}

static int sbi_hsm_hart_get_status(unsigned long hartid)
{
	struct sbiret ret;

	ret = sbi_ecall(SBI_EXT_HSM, SBI_EXT_HSM_HART_STATUS,
			hartid, 0, 0, 0, 0, 0);
	if (ret.error)
		return sbi_err_map_linux_errno(ret.error);
	else
		return ret.value;
}
#endif

static int sbi_cpu_start(unsigned int cpuid, struct task_struct *tidle)//启动指定 CPU 的 Hart 核心
{
	unsigned long boot_addr = __pa_symbol(secondary_start_sbi);//获取处理器启动代码的物理地址
	unsigned long hartid = cpuid_to_hartid_map(cpuid);//将 CPU ID 转换为 Hart ID
	unsigned long hsm_data;//Hart 启动管理数据指针
	struct sbi_hart_boot_data *bdata = &per_cpu(boot_data, cpuid);//获取该 CPU 的启动数据结构

	/* Make sure tidle is updated */
	smp_mb();//发出 SMP 内存屏障，确保 tidle 的更新对所有 CPU 可见
	bdata->task_ptr = tidle;//将启动数据中的任务指针设置为指定的空闲线程
	bdata->stack_ptr = task_pt_regs(tidle);//将启动数据中的栈指针设置为任务的寄存器上下文
	/* Make sure boot data is updated */
	smp_mb();//再次发出 SMP 内存屏障，确保启动数据的更新对所有 CPU 可见
	hsm_data = __pa(bdata);//获取启动数据结构的物理地址
	return sbi_hsm_hart_start(hartid, boot_addr, hsm_data);//调用 SBI HSM 接口启动指定的 Hart 核心
}

#ifdef CONFIG_HOTPLUG_CPU
static void sbi_cpu_stop(void)
{
	int ret;

	ret = sbi_hsm_hart_stop();
	pr_crit("Unable to stop the cpu %u (%d)\n", smp_processor_id(), ret);
}

static int sbi_cpu_is_stopped(unsigned int cpuid)
{
	int rc;
	unsigned long hartid = cpuid_to_hartid_map(cpuid);

	rc = sbi_hsm_hart_get_status(hartid);

	if (rc == SBI_HSM_STATE_STOPPED)
		return 0;
	return rc;
}
#endif

const struct cpu_operations cpu_ops_sbi = {
	.cpu_start	= sbi_cpu_start,
#ifdef CONFIG_HOTPLUG_CPU
	.cpu_stop	= sbi_cpu_stop,
	.cpu_is_stopped	= sbi_cpu_is_stopped,
#endif
};
