/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2009 Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_TLBFLUSH_H
#define _ASM_RISCV_TLBFLUSH_H

#include <linux/mm_types.h>
#include <asm/smp.h>
#include <asm/errata_list.h>

#define FLUSH_TLB_MAX_SIZE      ((unsigned long)-1)
#define FLUSH_TLB_NO_ASID       ((unsigned long)-1)

#ifdef CONFIG_MMU
static inline void local_flush_tlb_all(void)
{
	__asm__ __volatile__ ("sfence.vma" : : : "memory");
}

static inline void local_flush_tlb_all_asid(unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)//如果 asid不等于 FLUSH_TLB_NO_ASID（表示使用ASID来清除TLB）
		ALT_SFENCE_VMA_ASID(asid);//通过指定的ASID来清除 TLB 条目。
	else
		local_flush_tlb_all();//清除所有TLB条目，而不考虑ASID
}

/* Flush one page from local TLB */
static inline void local_flush_tlb_page(unsigned long addr)
{
	ALT_SFENCE_VMA_ADDR(addr);
}

static inline void local_flush_tlb_page_asid(unsigned long addr,
					     unsigned long asid)
{
	if (asid != FLUSH_TLB_NO_ASID)
		ALT_SFENCE_VMA_ADDR_ASID(addr, asid);//如果提供了有效的 ASID，调用 SFENCE.VMA 指令刷新指定地址和与ASID关联的TLB条目
	else
		local_flush_tlb_page(addr);//如果没有提供 ASID，则仅刷新指定地址的TLB条目，而不考虑ASID
}

void flush_tlb_all(void);
void flush_tlb_mm(struct mm_struct *mm);
void flush_tlb_mm_range(struct mm_struct *mm, unsigned long start,
			unsigned long end, unsigned int page_size);
void flush_tlb_page(struct vm_area_struct *vma, unsigned long addr);
void flush_tlb_range(struct vm_area_struct *vma, unsigned long start,
		     unsigned long end);
void flush_tlb_kernel_range(unsigned long start, unsigned long end);
void local_flush_tlb_kernel_range(unsigned long start, unsigned long end);
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
#define __HAVE_ARCH_FLUSH_PMD_TLB_RANGE
void flush_pmd_tlb_range(struct vm_area_struct *vma, unsigned long start,
			unsigned long end);
#endif

bool arch_tlbbatch_should_defer(struct mm_struct *mm);
void arch_tlbbatch_add_pending(struct arch_tlbflush_unmap_batch *batch,
			       struct mm_struct *mm,
			       unsigned long uaddr);
void arch_flush_tlb_batched_pending(struct mm_struct *mm);
void arch_tlbbatch_flush(struct arch_tlbflush_unmap_batch *batch);

extern unsigned long tlb_flush_all_threshold;
#else /* CONFIG_MMU */
#define local_flush_tlb_all()			do { } while (0)
#endif /* CONFIG_MMU */

#endif /* _ASM_RISCV_TLBFLUSH_H */
