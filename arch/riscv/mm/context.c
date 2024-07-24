// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 * Copyright (C) 2021 Western Digital Corporation or its affiliates.
 */

#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/mm.h>
#include <linux/percpu.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/static_key.h>
#include <asm/tlbflush.h>
#include <asm/cacheflush.h>
#include <asm/mmu_context.h>
#include <asm/switch_to.h>

#ifdef CONFIG_MMU

DEFINE_STATIC_KEY_FALSE(use_asid_allocator);

static unsigned long num_asids;

static atomic_long_t current_version;

static DEFINE_RAW_SPINLOCK(context_lock);
static cpumask_t context_tlb_flush_pending;
static unsigned long *context_asid_map;

static DEFINE_PER_CPU(atomic_long_t, active_context);
static DEFINE_PER_CPU(unsigned long, reserved_context);

static bool check_update_reserved_context(unsigned long cntx,
					  unsigned long newcntx)
{
	int cpu;
	bool hit = false;

	/*
	 * Iterate over the set of reserved CONTEXT looking for a match.
	 * If we find one, then we can update our mm to use new CONTEXT
	 * (i.e. the same CONTEXT in the current_version) but we can't
	 * exit the loop early, since we need to ensure that all copies
	 * of the old CONTEXT are updated to reflect the mm. Failure to do
	 * so could result in us missing the reserved CONTEXT in a future
	 * version.
	 */
	for_each_possible_cpu(cpu) {//遍历所有可能的 CPU
		if (per_cpu(reserved_context, cpu) == cntx) {//检查当前CPU的保留上下文是否与 cntx 匹配
			hit = true;//如果找到匹配的上下文，设置hit标志为true。
			per_cpu(reserved_context, cpu) = newcntx;//更新当前CPU的保留上下文为新的上下文newcntx。
		}
	}

	return hit;
}

static void __flush_context(void)
{
	int i;
	unsigned long cntx;

	/* Must be called with context_lock held */
	lockdep_assert_held(&context_lock);//确保在调用此函数时持有context_lock锁，以防止数据竞争

	/* Update the list of reserved ASIDs and the ASID bitmap. */
	bitmap_zero(context_asid_map, num_asids);//将 ASID 位图 context_asid_map 清零，以更新保留的 ASID 列表

	/* Mark already active ASIDs as used */
	for_each_possible_cpu(i) {//遍历系统中的每个可能的 CPU
		cntx = atomic_long_xchg_relaxed(&per_cpu(active_context, i), 0);//将每个 CPU 的 active_context 原子地交换为 0，并返回原来的值。
		/*
		 * If this CPU has already been through a rollover, but
		 * hasn't run another task in the meantime, we must preserve
		 * its reserved CONTEXT, as this is the only trace we have of
		 * the process it is still running.
		 */
		if (cntx == 0)
			cntx = per_cpu(reserved_context, i);//如果 cntx 为 0，表示此 CPU 尚未运行另一个任务，则使用保留的 reserved_context

		__set_bit(cntx2asid(cntx), context_asid_map);//将 cntx 转换为 ASID 并在 context_asid_map 中设置相应的位。
		per_cpu(reserved_context, i) = cntx;//更新每个 CPU 的保留上下文 reserved_context。
	}

	/* Mark ASID #0 as used because it is used at boot-time */
	__set_bit(0, context_asid_map);//将 ASID 0 标记为已使用，因为它在系统启动时使用。

	/* Queue a TLB invalidation for each CPU on next context-switch */
	cpumask_setall(&context_tlb_flush_pending);//设置 context_tlb_flush_pending 位图的所有位，以在下次上下文切换时为每个 CPU 队列一个 TLB 无效请求。
}

static unsigned long __new_context(struct mm_struct *mm)
{
	static u32 cur_idx = 1;
	unsigned long cntx = atomic_long_read(&mm->context.id);//读取内存管理结构 mm 的ASID
	unsigned long asid, ver = atomic_long_read(&current_version);//读取当前代号存储在ver中。

	/* Must be called with context_lock held */
	lockdep_assert_held(&context_lock);//确保在调用此函数时持有context_lock锁，以防止数据竞争

	if (cntx != 0) {//如果ASID不为零
		unsigned long newcntx = ver | cntx2asid(cntx);//计算新的ASID。由旧ASID号与当前的代号组合在一起（一个ASID由编号和代号组成，这块主要是使用旧的编号结合当前代号组成新的ASID，避免通过查ASID位图来确定新的编号，从而节省时间）

		/*
		 * If our current CONTEXT was active during a rollover, we
		 * can continue to use it and this was just a false alarm.
		 */
		if (check_update_reserved_context(cntx, newcntx))//检查所有CPU，如果有保留旧的ASID就更新cpu的ASID为新的newcntx。
			return newcntx;//如果更新成功返回新的ASID

		/*
		 * 我们在之前有一个有效的上下文，所以尽可能尝试重新使用它。
		 */
		if (!__test_and_set_bit(cntx2asid(cntx), context_asid_map))//尝试重新使用旧的ASID，如果成功则返回新的上下文 ID
			return newcntx;
	}

	/*
	 * Allocate a free ASID. If we can't find one then increment
	 * current_version and flush all ASIDs.
	 * 分配一个空闲的ASID。如果找不到空闲的 ASID，则增加current_version来刷新所有ASID
	 */
	asid = find_next_zero_bit(context_asid_map, num_asids, cur_idx);//在ASID位图中查找下一个空闲的ASID
	if (asid != num_asids)//如果找到空闲的 ASID，则跳转到 set_asid 标签。
		goto set_asid;

	/* We're out of ASIDs, so increment current_version */
	ver = atomic_long_add_return_relaxed(BIT(SATP_ASID_BITS), &current_version);//如果没有空闲的 ASID，增加 current_version

	/* Flush everything  */
	__flush_context();//刷新所有上下文，清除TLB

	/* We have more ASIDs than CPUs, so this will always succeed */
	asid = find_next_zero_bit(context_asid_map, num_asids, 1);//在ASID位图中从头开始查找下一个空闲的ASID

set_asid://设置新的ASID。
	__set_bit(asid, context_asid_map);//在 ASID 位图中设置这个 ASID
	cur_idx = asid;//更新当前 ASID 的索引
	return asid | ver;//返回新的ASID，将 ASID 和版本号组合在一起
}

static void set_mm_asid(struct mm_struct *mm, unsigned int cpu)
{
	unsigned long flags;
	bool need_flush_tlb = false;
	unsigned long cntx, old_active_cntx;

	cntx = atomic_long_read(&mm->context.id);//读取内存管理结构的上下文ASID

	/*
	 * If our active_context is non-zero and the context matches the
	 * current_version, then we update the active_context entry with a
	 * relaxed cmpxchg.
	 *
	 * Following is how we handle racing with a concurrent rollover:
	 *
	 * - We get a zero back from the cmpxchg and end up waiting on the
	 *   lock. Taking the lock synchronises with the rollover and so
	 *   we are forced to see the updated verion.
	 *
	 * - We get a valid context back from the cmpxchg then we continue
	 *   using old ASID because __flush_context() would have marked ASID
	 *   of active_context as used and next context switch we will
	 *   allocate new context.
	 */
	old_active_cntx = atomic_long_read(&per_cpu(active_context, cpu));//读取当前CPU的active_context(ASID)
	if (old_active_cntx &&					
	    (cntx2version(cntx) == atomic_long_read(&current_version)) &&// 如果old_active_cntx非零并且cntx的代与current_version当前代匹配
	    atomic_long_cmpxchg_relaxed(&per_cpu(active_context, cpu),
					old_active_cntx, cntx))//并且使用放松的cmpxchg更新cpu的ASID成功
		goto switch_mm_fast;//跳转

	raw_spin_lock_irqsave(&context_lock, flags);//获取 context_lock 自旋锁并保存中断状态

	/* Check that our ASID belongs to the current_version. */
	cntx = atomic_long_read(&mm->context.id);//再次读取内存管理结构的上下文ASID
	if (cntx2version(cntx) != atomic_long_read(&current_version)) {//如果cntx的ASID代与current_version当前代不匹配
		cntx = __new_context(mm);//申请新的上下文ASID
		atomic_long_set(&mm->context.id, cntx);//将新的上下文ASID设置到mm->context.id
	}

	if (cpumask_test_and_clear_cpu(cpu, &context_tlb_flush_pending))//如果当前 CPU 的 TLB 刷新挂起标志被设置，则需要执行TLB刷新
		need_flush_tlb = true;   

	atomic_long_set(&per_cpu(active_context, cpu), cntx);//将当前 CPU 的 active_context 设置为新的ASID

	raw_spin_unlock_irqrestore(&context_lock, flags);//释放自旋锁并恢复中断状态

switch_mm_fast:
	csr_write(CSR_SATP, virt_to_pfn(mm->pgd) |
		  (cntx2asid(cntx) << SATP_ASID_SHIFT) |
		  satp_mode);// 设置 CSR_SATP 寄存器，用于切换页表基地址寄存器

	if (need_flush_tlb)//如果需要刷新，则刷新TLB
		local_flush_tlb_all();
}

static void set_mm_noasid(struct mm_struct *mm)
{
	/* Switch the page table and blindly nuke entire local TLB */
	csr_write(CSR_SATP, virt_to_pfn(mm->pgd) | satp_mode);//将新的页表基地址写入 CSR_SATP 寄存器，并设置页表为SATP模式。virt_to_pfn(mm->pgd)将页全局目录（mm->pgd）转换为物理帧号
	local_flush_tlb_all_asid(0);//将ASID设置为0，表示清除本地TLB的所有条目
}

static inline void set_mm(struct mm_struct *prev,
			  struct mm_struct *next, unsigned int cpu)
{
	/*
	 * The mm_cpumask indicates which harts' TLBs contain the virtual
	 * address mapping of the mm. Compared to noasid, using asid
	 * can't guarantee that stale TLB entries are invalidated because
	 * the asid mechanism wouldn't flush TLB for every switch_mm for
	 * performance. So when using asid, keep all CPUs footmarks in
	 * cpumask() until mm reset.
	 */
	cpumask_set_cpu(cpu, mm_cpumask(next));//将当前 CPU 的 ID 添加到 next 的 mm_cpumask 中。mm_cpumask 表示哪些 CPU 的 TLB 包含了 mm 的虚拟地址映射
	if (static_branch_unlikely(&use_asid_allocator)) {//检查是否使用 ASID 分配器
		set_mm_asid(next, cpu);//如果使用 ASID 分配器，调用该函数为next设置ASID。这意味着TLB刷新会利用ASID来避免完全的TLB刷新。
	} else {//如果不使用 ASID 分配器
		cpumask_clear_cpu(cpu, mm_cpumask(prev));//从prev的mm_cpumask中清除当前CPU的ID。这意味着当前CPU不再包含prev的虚拟地址映射。
		set_mm_noasid(next);//为 next 设置没有 ASID 的情况,切换内存管理mm。这意味着在没有 ASID 的情况下，必须确保 TLB 条目被正确无效化。
	}
}

static int __init asids_init(void)
{
	unsigned long asid_bits, old;

	/* Figure-out number of ASID bits in HW */
	old = csr_read(CSR_SATP);//读取当前的CSR_SATP寄存器值
	asid_bits = old | (SATP_ASID_MASK << SATP_ASID_SHIFT);//设置假定的最大ASID位数
	csr_write(CSR_SATP, asid_bits);//将假定的值写回CSR_SATP寄存器
	asid_bits = (csr_read(CSR_SATP) >> SATP_ASID_SHIFT)  & SATP_ASID_MASK;//读取 CSR_SATP 寄存器，并获取 ASID 部分的值。
	asid_bits = fls_long(asid_bits);//计算最高有效位（即实际的 ASID 位数）
	csr_write(CSR_SATP, old);//恢复原来的 CSR_SATP 值

	/*
	 * In the process of determining number of ASID bits (above)
	 * we polluted the TLB of current HART so let's do TLB flushed
	 * to remove unwanted TLB enteries.
	 */
	local_flush_tlb_all();//刷新 TLB，以移除在确定 ASID 位数过程中污染的 TLB 条目。

	/* Pre-compute ASID details */
	if (asid_bits) {
		num_asids = 1 << asid_bits;//如果 asid_bits 非零，计算 ASID 的数量
	}

	/*
	 * Use ASID allocator only if number of HW ASIDs are
	 * at-least twice more than CPUs
	 */
	if (num_asids > (2 * num_possible_cpus())) {//如果ASID 数量至少是CPU数量的两倍
		atomic_long_set(&current_version, BIT(SATP_ASID_BITS));//设置当前代数，使用SATP_ASID_BITS位数

		context_asid_map = bitmap_zalloc(num_asids, GFP_KERNEL);//分配ASID位图
		if (!context_asid_map)//如果位图分配失败，打印错误信息并崩溃。
			panic("Failed to allocate bitmap for %lu ASIDs\n",
			      num_asids);

		__set_bit(0, context_asid_map);//设置位图的第一个位，表示ASID 0已被使用。

		static_branch_enable(&use_asid_allocator);//启用ASID分配器

		pr_info("ASID allocator using %lu bits (%lu entries)\n",
			asid_bits, num_asids);//打印信息，表示启用了ASID分配器
	} else {
		pr_info("ASID allocator disabled (%lu bits)\n", asid_bits);//打印信息，表示未启用ASID分配器
	}

	return 0;
}
early_initcall(asids_init);
#else
static inline void set_mm(struct mm_struct *prev,
			  struct mm_struct *next, unsigned int cpu)
{
	/* Nothing to do here when there is no MMU */
}
#endif

/*
 * When necessary, performs a deferred icache flush for the given MM context,
 * on the local CPU.  RISC-V has no direct mechanism for instruction cache
 * shoot downs, so instead we send an IPI that informs the remote harts they
 * need to flush their local instruction caches.  To avoid pathologically slow
 * behavior in a common case (a bunch of single-hart processes on a many-hart
 * machine, ie 'make -j') we avoid the IPIs for harts that are not currently
 * executing a MM context and instead schedule a deferred local instruction
 * cache flush to be performed before execution resumes on each hart.  This
 * actually performs that local instruction cache flush, which implicitly only
 * refers to the current hart.
 *
 * The "cpu" argument must be the current local CPU number.
 */
static inline void flush_icache_deferred(struct mm_struct *mm, unsigned int cpu,
					 struct task_struct *task)
{
#ifdef CONFIG_SMP
	if (cpumask_test_and_clear_cpu(cpu, &mm->context.icache_stale_mask)) {
		/*
		 * Ensure the remote hart's writes are visible to this hart.
		 * This pairs with a barrier in flush_icache_mm.
		 */
		smp_mb();

		/*
		 * If cache will be flushed in switch_to, no need to flush here.
		 */
		if (!(task && switch_to_should_flush_icache(task)))
			local_flush_icache_all();
	}
#endif
}

void switch_mm(struct mm_struct *prev, struct mm_struct *next,
	struct task_struct *task)
{
	unsigned int cpu;

	if (unlikely(prev == next))// 如果前一个mm和下一个mm相同，则无需切换，直接返回
		return;

	membarrier_arch_switch_mm(prev, next, task);//处理特定架构的内存屏障

	/*
	 * 将当前 MM 上下文标记为非活动状态，并将下一个上下文标记为活动状态。
	 * 这至少用于指令缓存刷新例程，以确定谁应该被刷新。
	 */
	cpu = smp_processor_id();//获取当前处理器的 ID

	set_mm(prev, next, cpu);//将当前mm上下文从prev切换到next

	flush_icache_deferred(next, cpu, task);//延迟刷新指令缓存，以确保新的mm上下文的指令缓存一致性。
}
