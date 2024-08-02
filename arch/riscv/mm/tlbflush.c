// SPDX-License-Identifier: GPL-2.0

#include <linux/mm.h>
#include <linux/smp.h>
#include <linux/sched.h>
#include <linux/hugetlb.h>
#include <asm/sbi.h>
#include <asm/mmu_context.h>

/*
 * Flush entire TLB if number of entries to be flushed is greater
 * than the threshold below.
 */
unsigned long tlb_flush_all_threshold __read_mostly = 64;

static void local_flush_tlb_range_threshold_asid(unsigned long start,
						 unsigned long size,
						 unsigned long stride,
						 unsigned long asid)
{
	unsigned long nr_ptes_in_range = DIV_ROUND_UP(size, stride);
	int i;

	if (nr_ptes_in_range > tlb_flush_all_threshold) {
		local_flush_tlb_all_asid(asid);
		return;
	}

	for (i = 0; i < nr_ptes_in_range; ++i) {
		local_flush_tlb_page_asid(start, asid);
		start += stride;
	}
}

static inline void local_flush_tlb_range_asid(unsigned long start,
		unsigned long size, unsigned long stride, unsigned long asid)
{
	if (size <= stride)
		local_flush_tlb_page_asid(start, asid);
	else if (size == FLUSH_TLB_MAX_SIZE)
		local_flush_tlb_all_asid(asid);
	else
		local_flush_tlb_range_threshold_asid(start, size, stride, asid);
}

/* Flush a range of kernel pages without broadcasting */
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	local_flush_tlb_range_asid(start, end - start, PAGE_SIZE, FLUSH_TLB_NO_ASID);
}

static void __ipi_flush_tlb_all(void *info)
{
	local_flush_tlb_all();
}

void flush_tlb_all(void)
{
	if (num_online_cpus() < 2)//检查当前在线 CPU 的数量是否少于 2
		local_flush_tlb_all();//如果只有一个 CPU 在线，则只需要在本地 CPU 上刷新 TLB
	else if (riscv_use_sbi_for_rfence())//如果系统支持通过SBI来进行远程 TLB刷新
		sbi_remote_sfence_vma_asid(NULL, 0, FLUSH_TLB_MAX_SIZE, FLUSH_TLB_NO_ASID);//执行进行远程TLB刷新
	else//如果有多个CPU在线，且不使用SBI进行TLB刷新
		on_each_cpu(__ipi_flush_tlb_all, NULL, 1);//在每个CPU上调用__ipi_flush_tlb_all函数进行TLB刷新
}

struct flush_tlb_range_data {
	unsigned long asid;
	unsigned long start;
	unsigned long size;
	unsigned long stride;
};
/*
 * 定义函数用于处理 IPI TLB 刷新请求
 * */
static void __ipi_flush_tlb_range_asid(void *info)
{
	struct flush_tlb_range_data *d = info;//将传入的参数转换为 flush_tlb_range_data 结构体指针

	local_flush_tlb_range_asid(d->start, d->size, d->stride, d->asid);//调用本地函数进行 TLB 刷新
}

static void __flush_tlb_range(const struct cpumask *cmask, unsigned long asid,
			      unsigned long start, unsigned long size,
			      unsigned long stride)
{
	unsigned int cpu;

	if (cpumask_empty(cmask))//如果 CPU 掩码为空，直接返回
		return;

	cpu = get_cpu();//获取当前 CPU 编号

	/* 检查是否需要将 TLB 刷新请求发送到其他 CPU */
	if (cpumask_any_but(cmask, cpu) >= nr_cpu_ids) {
		local_flush_tlb_range_asid(start, size, stride, asid);//如果当前CPU是唯一目标，进行本地刷新
	} else if (riscv_use_sbi_for_rfence()) {//如果支持SBI
		sbi_remote_sfence_vma_asid(cmask, start, size, asid);//使用SBI进行远程TLB刷新
	} else {
		struct flush_tlb_range_data ftd;//定义结构体保存刷新数据

		ftd.asid = asid;//设置地址空间标识符
		ftd.start = start;//设置起始地址
		ftd.size = size;//设置大小
		ftd.stride = stride;//设置步长
		on_each_cpu_mask(cmask, __ipi_flush_tlb_range_asid, &ftd, 1);//在掩码指定的每个 CPU 上调用刷新函数:__ipi_flush_tlb_range_asid
	}

	put_cpu();
}

static inline unsigned long get_mm_asid(struct mm_struct *mm)
{
	return cntx2asid(atomic_long_read(&mm->context.id));
}

void flush_tlb_mm(struct mm_struct *mm)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  0, FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
}

void flush_tlb_mm_range(struct mm_struct *mm,
			unsigned long start, unsigned long end,
			unsigned int page_size)
{
	__flush_tlb_range(mm_cpumask(mm), get_mm_asid(mm),
			  start, end - start, page_size);
}

void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  addr, PAGE_SIZE, PAGE_SIZE);
}
/*
 * 用于刷新特定虚拟内存区域的TLB范围
 * */
void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end)
{
	unsigned long stride_size;//定义变量 stride_size 用于保存步长大小

	if (!is_vm_hugetlb_page(vma)) {//检查vma是否是大页
		stride_size = PAGE_SIZE;//如果不是大页，设置步长为普通页大小
	} else {
		stride_size = huge_page_size(hstate_vma(vma));//如果是大页，获取大页大小

		/*
		 * 根据特权规范，每个 NAPOT 区域中的 PTE 必须被无效化，因此在这种情况下重置步长。
		 */
		if (has_svnapot()) {//检查是否支持 NAPOT
			if (stride_size >= PGDIR_SIZE)//如果大页大小大于等于 PGDIR_SIZE，设置步长为 PGDIR_SIZE
				stride_size = PGDIR_SIZE;
			else if (stride_size >= P4D_SIZE)//如果大页大小大于等于 P4D_SIZE，设置步长为 P4D_SIZE
				stride_size = P4D_SIZE;
			else if (stride_size >= PUD_SIZE)//如果大页大小大于等于 PUD_SIZE，设置步长为 PUD_SIZE
				stride_size = PUD_SIZE;
			else if (stride_size >= PMD_SIZE)//如果大页大小大于等于 PMD_SIZE，设置步长为 PMD_SIZE
				stride_size = PMD_SIZE;
			else
				stride_size = PAGE_SIZE;//否则，设置步长为普通页大小
		}
	}

	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),//调用内部函数刷新 TLB 范围
			  start, end - start, stride_size);
}

void flush_tlb_kernel_range(unsigned long start, unsigned long end)
{
	__flush_tlb_range(cpu_online_mask, FLUSH_TLB_NO_ASID,
			  start, end - start, PAGE_SIZE);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end)
{
	__flush_tlb_range(mm_cpumask(vma->vm_mm), get_mm_asid(vma->vm_mm),
			  start, end - start, PMD_SIZE);
}
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm)
{
	return true;
}

void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
			       struct mm_struct *mm,
			       unsigned long uaddr)
{
	cpumask_or(&batch->cpumask, &batch->cpumask, mm_cpumask(mm));
}

void arch_flush_tlb_batched_pending(struct mm_struct *mm)
{
	flush_tlb_mm(mm);
}

void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch)
{
	__flush_tlb_range(&batch->cpumask, FLUSH_TLB_NO_ASID, 0,
			  FLUSH_TLB_MAX_SIZE, PAGE_SIZE);
	cpumask_clear(&batch->cpumask);
}
