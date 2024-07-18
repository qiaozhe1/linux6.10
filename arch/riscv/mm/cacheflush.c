// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2017 SiFive
 */

#include <linux/acpi.h>
#include <linux/of.h>
#include <linux/prctl.h>
#include <asm/acpi.h>
#include <asm/cacheflush.h>

#ifdef CONFIG_SMP

#include <asm/sbi.h>

static void ipi_remote_fence_i(void *info)
{
	return local_flush_icache_all();
}

void flush_icache_all(void)
{
	local_flush_icache_all();//在本地 CPU 上刷新指令缓存

	if (num_online_cpus() < 2)//如果系统中在线的 CPU 数量少于 2，则无需进一步处理，直接返回
		return;
	else if (riscv_use_sbi_for_rfence())//如果系统支持通过 SBI（Supervisor Binary Interface）进行远程 fence.i 指令
		sbi_remote_fence_i(NULL);//调用SBI接口在所有远程CPU上执行fence.i指令以刷新指令缓存
	else
		on_each_cpu(ipi_remote_fence_i, NULL, 1);// 如果不支持通过 SBI 进行远程 fence.i 指令,在每个 CPU 上调用 ipi_remote_fence_i 函数，以中断处理器间中断（IPI）方式执行 fence.i 指令
}
EXPORT_SYMBOL(flush_icache_all);

/*
 * Performs an icache flush for the given MM context.  RISC-V has no direct
 * mechanism for instruction cache shoot downs, so instead we send an IPI that
 * informs the remote harts they need to flush their local instruction caches.
 * To avoid pathologically slow behavior in a common case (a bunch of
 * single-hart processes on a many-hart machine, ie 'make -j') we avoid the
 * IPIs for harts that are not currently executing a MM context and instead
 * schedule a deferred local instruction cache flush to be performed before
 * execution resumes on each hart.
 */
void flush_icache_mm(struct mm_struct *mm, bool local)//刷新内存管理结构mm的指令缓存
{
	unsigned int cpu;//保存当前处理器的 CPU ID
	cpumask_t others, *mask;//用于保存其他处理器的掩码和当前处理器的掩码指针

	preempt_disable();//禁用抢占，确保当前代码不会被中断

	/* Mark every hart's icache as needing a flush for this MM. */
	mask = &mm->context.icache_stale_mask;//
	cpumask_setall(mask);//设置掩码中的所有位，表示所有hart的Icache都需要刷新
	/* Flush this hart's I$ now, and mark it as flushed. */
	cpu = smp_processor_id();//获取当前处理器的 CPU ID
	cpumask_clear_cpu(cpu, mask);//清除当前处理器在掩码中的位，表示它的 Icache 已经刷新
	local_flush_icache_all();//刷新当前处理器的所有 Icache

	/*
	 * Flush the I$ of other harts concurrently executing, and mark them as
	 * flushed.
	 */
	cpumask_andnot(&others, mm_cpumask(mm), cpumask_of(cpu));//获取除当前处理器外的所有处理器掩码
	local |= cpumask_empty(&others);//如果没有其他处理器则设置 local 标志
	if (mm == current->active_mm && local) {//检查当前的内存管理结构mm是否是当前进程的活动内存管理结构active_mm, 并且 local 标志是否为真,指示是否只有当前处理器需要刷新指令缓存。如果 local 为真，表示没有其他处理器需要刷新指令缓存。
		/*
		 * It's assumed that at least one strongly ordered operation is
		 * performed on this hart between setting a hart's cpumask bit
		 * and scheduling this MM context on that hart.  Sending an SBI
		 * remote message will do this, but in the case where no
		 * messages are sent we still need to order this hart's writes
		 * with flush_icache_deferred().
		 */
		smp_mb();//内存屏障
	} else if (riscv_use_sbi_for_rfence()) {//是否使用 RISC-V 的 SBI（Supervisor Binary Interface）机制来执行
		sbi_remote_fence_i(&others);//使用SB发送远程刷新指令缓存消息
	} else {
		on_each_cpu_mask(&others, ipi_remote_fence_i, NULL, 1);//使用IPI在其他处理器上执行远程刷新指令缓存
	}

	preempt_enable();//重新启用抢占
}

#endif /* CONFIG_SMP */

#ifdef CONFIG_MMU
void flush_icache_pte(struct mm_struct *mm, pte_t pte)//刷新与页表项关联的icache
{
	struct folio *folio = page_folio(pte_page(pte));//获取页表条目对应的页面folio

	if (!test_bit(PG_dcache_clean, &folio->flags)) {//如果folio的PG_dcache_clean标志没有设置
		flush_icache_mm(mm, false);//刷新指定内存管理结构mm的指令缓存
		set_bit(PG_dcache_clean, &folio->flags);//设置folio的PG_dcache_clean标志
	}
}
#endif /* CONFIG_MMU */

unsigned int riscv_cbom_block_size;
EXPORT_SYMBOL_GPL(riscv_cbom_block_size);

unsigned int riscv_cboz_block_size;
EXPORT_SYMBOL_GPL(riscv_cboz_block_size);

static void __init cbo_get_block_size(struct device_node *node,
				      const char *name, u32 *block_size,
				      unsigned long *first_hartid)
{
	unsigned long hartid;//用于存储处理器的 hart ID
	u32 val;

	if (riscv_of_processor_hartid(node, &hartid))//获取当前节点的处理器 hart ID，如果获取失败则返回。
		return;

	if (of_property_read_u32(node, name, &val))//从设备树节点读取名为name的属性值，如果读取失败则返回。
		return;

	if (!*block_size) {//如果block_size没有被设置
		*block_size = val;//将block_size设置为读取的val值。
		*first_hartid = hartid;//将first_hartid设置为当前处理器的 hart ID
	} else if (*block_size != val) {//如果读取的值 val 与已设置的 block_size 不一致，打印警告信息。
		pr_warn("%s mismatched between harts %lu and %lu\n",
			name, *first_hartid, hartid);
	}
}

void __init riscv_init_cbo_blocksizes(void)//初始化CBO操作的缓存行大小
{
	unsigned long cbom_hartid, cboz_hartid;//定义用于保存 cbom 和 cboz 缓存行大小的 hart ID。指的时进行cbo操作时一次能够处理缓存行大小
	u32 cbom_block_size = 0, cboz_block_size = 0;//初始化 cbom 和 cboz 的缓存行大小为 0。
	struct device_node *node;//定义设备节点指针，用于遍历设备树中的 CPU 节点。
	struct acpi_table_header *rhct;//定义 ACPI 表头指针，用于访问 ACPI 表。
	acpi_status status;//定义 ACPI 状态变量，用于保存 ACPI 函数的返回状态。

	if (acpi_disabled) {//如果ACPI被禁用，遍历每个 CPU 设备节点，并获取可用的 cbom 和/或 cboz 扩展缓存行大小。
		for_each_of_cpu_node(node) {
			/* set block-size for cbom and/or cboz extension if available */
			cbo_get_block_size(node, "riscv,cbom-block-size",
					   &cbom_block_size, &cbom_hartid);
			cbo_get_block_size(node, "riscv,cboz-block-size",
					   &cboz_block_size, &cboz_hartid);
		}
	} else {
		status = acpi_get_table(ACPI_SIG_RHCT, 0, &rhct);//如果 ACPI 启用，获取 ACPI RHCT 表
		if (ACPI_FAILURE(status))//如果获取表失败，则返回。
			return;

		acpi_get_cbo_block_size(rhct, &cbom_block_size, &cboz_block_size, NULL);//从 ACPI 表中获取 cbom 和 cboz 的缓存行大小
		acpi_put_table((struct acpi_table_header *)rhct);//释放 ACPI 表
	}

	if (cbom_block_size)//如果 cbom_block_size 不为 0，则设置全局变量 riscv_cbom_block_size
		riscv_cbom_block_size = cbom_block_size;

	if (cboz_block_size)//如果 cboz_block_size 不为 0，则设置全局变量 riscv_cboz_block_size
		riscv_cboz_block_size = cboz_block_size;
}

#ifdef CONFIG_SMP
static void set_icache_stale_mask(void)
{
	cpumask_t *mask;
	bool stale_cpu;

	/*
	 * Mark every other hart's icache as needing a flush for
	 * this MM. Maintain the previous value of the current
	 * cpu to handle the case when this function is called
	 * concurrently on different harts.
	 */
	mask = &current->mm->context.icache_stale_mask;//获取当前内存描述符（MM）上下文中的 I-cache 失效掩码
	stale_cpu = cpumask_test_cpu(smp_processor_id(), mask);//检查当前处理器的 I-cache 是否已经被标记为需要刷新

	cpumask_setall(mask);//将掩码设置为所有处理器，表示所有处理器的icache都需要刷新
	cpumask_assign_cpu(smp_processor_id(), mask, stale_cpu);//恢复当前处理器的之前状态
}
#endif

/**
 * riscv_set_icache_flush_ctx() - Enable/disable icache flushing instructions in
 * userspace.
 * @ctx: Set the type of icache flushing instructions permitted/prohibited in
 *	 userspace. Supported values described below.
 *
 * Supported values for ctx:
 *
 * * %PR_RISCV_CTX_SW_FENCEI_ON: Allow fence.i in user space.
 *
 * * %PR_RISCV_CTX_SW_FENCEI_OFF: Disallow fence.i in user space. All threads in
 *   a process will be affected when ``scope == PR_RISCV_SCOPE_PER_PROCESS``.
 *   Therefore, caution must be taken; use this flag only when you can guarantee
 *   that no thread in the process will emit fence.i from this point onward.
 *
 * @scope: Set scope of where icache flushing instructions are allowed to be
 *	   emitted. Supported values described below.
 *
 * Supported values for scope:
 *
 * * %PR_RISCV_SCOPE_PER_PROCESS: Ensure the icache of any thread in this process
 *                               is coherent with instruction storage upon
 *                               migration.
 *
 * * %PR_RISCV_SCOPE_PER_THREAD: Ensure the icache of the current thread is
 *                              coherent with instruction storage upon
 *                              migration.
 *
 * When ``scope == PR_RISCV_SCOPE_PER_PROCESS``, all threads in the process are
 * permitted to emit icache flushing instructions. Whenever any thread in the
 * process is migrated, the corresponding hart's icache will be guaranteed to be
 * consistent with instruction storage. This does not enforce any guarantees
 * outside of migration. If a thread modifies an instruction that another thread
 * may attempt to execute, the other thread must still emit an icache flushing
 * instruction before attempting to execute the potentially modified
 * instruction. This must be performed by the user-space program.
 *
 * In per-thread context (eg. ``scope == PR_RISCV_SCOPE_PER_THREAD``) only the
 * thread calling this function is permitted to emit icache flushing
 * instructions. When the thread is migrated, the corresponding hart's icache
 * will be guaranteed to be consistent with instruction storage.
 *
 * On kernels configured without SMP, this function is a nop as migrations
 * across harts will not occur.
 * 在用户空间中启用/禁用icache刷新指令。
 */
int riscv_set_icache_flush_ctx(unsigned long ctx, unsigned long scope)
{
#ifdef CONFIG_SMP//多处理器
	switch (ctx) {
	case PR_RISCV_CTX_SW_FENCEI_ON://开启强制icache刷新
		switch (scope) {//处理的范围
		case PR_RISCV_SCOPE_PER_PROCESS://对整个进程进行操作
			current->mm->context.force_icache_flush = true;//设置当前进程的icache刷新标志
			break;
		case PR_RISCV_SCOPE_PER_THREAD://对每个线程进行操作
			current->thread.force_icache_flush = true;//设置当前线程的icache刷新标志
			break;
		default:
			return -EINVAL;//无效范围，返回错误
		}
		break;
	case PR_RISCV_CTX_SW_FENCEI_OFF://强制关闭icache刷新
		switch (scope) {
		case PR_RISCV_SCOPE_PER_PROCESS://对整个进程进行操作
			current->mm->context.force_icache_flush = false;//清除当前进程的icache刷新标志

			set_icache_stale_mask();//设置icache失效掩码
			break;
		case PR_RISCV_SCOPE_PER_THREAD://对每个线程进行操作
			current->thread.force_icache_flush = false;//清除当前线程的icache刷新标志

			set_icache_stale_mask();//设置icache失效掩码
			break;
		default:
			return -EINVAL;//无效范围，返回错误
		}
		break;
	default:
		return -EINVAL;//无效上下文，返回错误
	}
	return 0;
#else
	switch (ctx) {//在单处理器（非 SMP）系统中，只处理上下文（ctx）
	case PR_RISCV_CTX_SW_FENCEI_ON:
	case PR_RISCV_CTX_SW_FENCEI_OFF:
		return 0;//对于单处理器系统，总是返回 0
	default:
		return -EINVAL;//无效上下文，返回错误
	}
#endif
}
