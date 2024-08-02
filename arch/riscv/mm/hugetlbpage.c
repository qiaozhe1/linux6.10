// SPDX-License-Identifier: GPL-2.0
#include <linux/hugetlb.h>
#include <linux/err.h>

#ifdef CONFIG_RISCV_ISA_SVNAPOT
pte_t huge_ptep_get(pte_t *ptep)//用于获取巨页表项
{
	unsigned long pte_num;//存储页表项数量
	int i;
	pte_t orig_pte = ptep_get(ptep);//获取原始的页表项

	if (!pte_present(orig_pte) || !pte_napot(orig_pte))//如果页表项不在内存中或不是NAPOT巨页
		return orig_pte;//直接返回原始页表项

	pte_num = napot_pte_num(napot_cont_order(orig_pte));//获取NAPOT巨页的页表项数量

	for (i = 0; i < pte_num; i++, ptep++) {//遍历所有页表项
		pte_t pte = ptep_get(ptep);//获取当前页表项

		if (pte_dirty(pte))//如果页表项为脏
			orig_pte = pte_mkdirty(orig_pte);//将原始页表项标记为脏

		if (pte_young(pte))//如果页表项为young
			orig_pte = pte_mkyoung(orig_pte);//将原始页表项标记为young
	}

	return orig_pte;//返回处理后的页表项
}
/*
 * 根据虚拟地址和巨页大小，分配并初始化相应的页表项，以支持巨页的内存映射。
 * mm: 内存描述符
 * vma: 虚拟内存区域
 * addr: 虚拟地址
 * sz: 巨页的大小
 * */
pte_t *huge_pte_alloc(struct mm_struct *mm,
		      struct vm_area_struct *vma,
		      unsigned long addr,
		      unsigned long sz)
{
	unsigned long order;//用于存储巨页的顺序
	pte_t *pte = NULL;//始化页表项指针pte为NULL
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);//获取PGD指针
	p4d = p4d_alloc(mm, pgd, addr);//分配P4D表项
	if (!p4d)//如果分配失败
		return NULL;//返回NULL

	pud = pud_alloc(mm, p4d, addr);//分配PUD表项
	if (!pud)//如果分配失败
		return NULL;//返回NULL

	if (sz == PUD_SIZE) {//如果巨页大小等于PUD_SIZE
		pte = (pte_t *)pud;//将PUD指针转换为PTE指针
		goto out;
	}

	if (sz == PMD_SIZE) {//如果巨页大小等于PMD_SIZE
		if (want_pmd_share(vma, addr) && pud_none(pudp_get(pud)))//如果需要共享PMD并且PUD为空
			pte = huge_pmd_share(mm, vma, addr, pud);//共享PMD.-----到这了
		else
			pte = (pte_t *)pmd_alloc(mm, pud, addr);//分配PMD表项
		goto out;
	}

	pmd = pmd_alloc(mm, pud, addr);//分配PMD表项
	if (!pmd)//如果分配失败
		return NULL;

	for_each_napot_order(order) {//遍历NAPOT巨页顺序
		if (napot_cont_size(order) == sz) {//如果找到匹配的巨页大小
			pte = pte_alloc_huge(mm, pmd, addr & napot_cont_mask(order));//分配巨页的PTE表项
			break;
		}
	}

out:
	if (pte) {//如果PTE不为空
		pte_t pteval = ptep_get_lockless(pte);//获取PTE的值

		WARN_ON_ONCE(pte_present(pteval) && !pte_huge(pteval));//如果PTE存在但不是巨页，发出警告
	}
	return pte;//返回PTE指针
}
/*
 * 获取巨页页表项
 * */
pte_t *huge_pte_offset(struct mm_struct *mm,
		       unsigned long addr,
		       unsigned long sz)
{
	unsigned long order;//页表项顺序
	pte_t *pte = NULL;//页表项指针初始化为NULL
	pgd_t *pgd;
	p4d_t *p4d;
	pud_t *pud;
	pmd_t *pmd;

	pgd = pgd_offset(mm, addr);//获取页全局目录项指针
	if (!pgd_present(pgdp_get(pgd)))//如果页全局目录项不存在
		return NULL;

	p4d = p4d_offset(pgd, addr);//获取页四级目录项指针
	if (!p4d_present(p4dp_get(p4d)))//如果页四级目录项不存在
		return NULL;

	pud = pud_offset(p4d, addr);//获取页上级目录项指针
	if (sz == PUD_SIZE)//如果巨页大小为PUD_SIZE
		/* must be pud huge, non-present or none */
		return (pte_t *)pud;//返回pud指针作为pte指针

	if (!pud_present(pudp_get(pud)))//如果页上级目录项不存在
		return NULL;//返回NULL

	pmd = pmd_offset(pud, addr);//获取页中级目录项指针
	if (sz == PMD_SIZE)//如果巨页大小为PMD_SIZE
		/* must be pmd huge, non-present or none */
		return (pte_t *)pmd;//返回pmd指针作为pte指针

	if (!pmd_present(pmdp_get(pmd)))//如果页中级目录项不存在
		return NULL;//返回NULL

	for_each_napot_order(order) {//遍历所有NAPOT顺序
		if (napot_cont_size(order) == sz) {//如果NAPOT连续大小等于sz
			pte = pte_offset_huge(pmd, addr & napot_cont_mask(order));//获取对应的pte偏移
			break;
		}
	}
	return pte;//返回找到的pte
}

unsigned long hugetlb_mask_last_page(struct hstate *h)
{
	unsigned long hp_size = huge_page_size(h);

	switch (hp_size) {
#ifndef __PAGETABLE_PMD_FOLDED
	case PUD_SIZE:
		return P4D_SIZE - PUD_SIZE;
#endif
	case PMD_SIZE:
		return PUD_SIZE - PMD_SIZE;
	case napot_cont_size(NAPOT_CONT64KB_ORDER):
		return PMD_SIZE - napot_cont_size(NAPOT_CONT64KB_ORDER);
	default:
		break;
	}

	return 0UL;
}
/*
 * 获取并清除连续的页表项
 * */
static pte_t get_clear_contig(struct mm_struct *mm,
			      unsigned long addr,
			      pte_t *ptep,
			      unsigned long pte_num)
{
	pte_t orig_pte = ptep_get(ptep);//获取第一个页表项
	unsigned long i;

	for (i = 0; i < pte_num; i++, addr += PAGE_SIZE, ptep++) {//遍历所有页表项
		pte_t pte = ptep_get_and_clear(mm, addr, ptep);//获取并清除当前页表项

		if (pte_dirty(pte))//如果页表项标记为脏
			orig_pte = pte_mkdirty(orig_pte);//将 orig_pte 标记为脏

		if (pte_young(pte))//如果页表项为young页
			orig_pte = pte_mkyoung(orig_pte);//将 orig_pte 标记young页
	}

	return orig_pte;//返回修改后的 orig_pte
}
/*
 * 获取并清除连续的页表项，并刷新TLB
 * */
static pte_t get_clear_contig_flush(struct mm_struct *mm,
				    unsigned long addr,
				    pte_t *ptep,//页表项指针
				    unsigned long pte_num)//页表项数量
{
	pte_t orig_pte = get_clear_contig(mm, addr, ptep, pte_num);//获取并清除连续页表项，保存原始页表项
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);//定义并初始化虚拟内存区域结构体，用于TLB刷新
	bool valid = !pte_none(orig_pte);//检查原始页表项是否有效

	if (valid)//如果原始页表项有效
		flush_tlb_range(&vma, addr, addr + (PAGE_SIZE * pte_num));//刷新TLB中的相应范围

	return orig_pte;//返回原始页表项
}

pte_t arch_make_huge_pte(pte_t entry, unsigned int shift, vm_flags_t flags)
{
	unsigned long order;

	for_each_napot_order(order) {
		if (shift == napot_cont_shift(order)) {
			entry = pte_mknapot(entry, order);
			break;
		}
	}
	if (order == NAPOT_ORDER_MAX)
		entry = pte_mkhuge(entry);

	return entry;
}

static void clear_flush(struct mm_struct *mm,
			unsigned long addr,
			pte_t *ptep,
			unsigned long pgsize,
			unsigned long ncontig)
{
	struct vm_area_struct vma = TLB_FLUSH_VMA(mm, 0);
	unsigned long i, saddr = addr;

	for (i = 0; i < ncontig; i++, addr += pgsize, ptep++)
		ptep_get_and_clear(mm, addr, ptep);

	flush_tlb_range(&vma, saddr, addr);
}

/*
 * When dealing with NAPOT mappings, the privileged specification indicates that
 * "if an update needs to be made, the OS generally should first mark all of the
 * PTEs invalid, then issue SFENCE.VMA instruction(s) covering all 4 KiB regions
 * within the range, [...] then update the PTE(s), as described in Section
 * 4.2.1.". That's the equivalent of the Break-Before-Make approach used by
 * arm64.
 * 设置指定地址处的巨页页表项
 * mm：内存管理结构指针
 * addr：目标地址
 * ptep：页表项指针
 * pte：要设置的页表项
 * sz：巨页大小
 */
void set_huge_pte_at(struct mm_struct *mm,
		     unsigned long addr,
		     pte_t *ptep,
		     pte_t pte,
		     unsigned long sz)
{
	unsigned long hugepage_shift, pgsize;//定义巨页的移位大小和页面大小
	int i, pte_num;

	if (sz >= PGDIR_SIZE)//如果巨页大小大于或等于PGDIR_SIZE
		hugepage_shift = PGDIR_SHIFT;//设置移位大小为PGDIR_SHIFT。
	else if (sz >= P4D_SIZE)//如果巨页大小大于或等于P4D_SIZE
		hugepage_shift = P4D_SHIFT;//设置移位大小为P4D_SHIFT。
	else if (sz >= PUD_SIZE)//如果巨页大小大于或等于PUD_SIZE
		hugepage_shift = PUD_SHIFT;//设置移位大小为PUD_SHIFT
	else if (sz >= PMD_SIZE)//如果巨页大小大于或等于PMD_SIZE
		hugepage_shift = PMD_SHIFT;//设置移位大小为PMD_SHIFT。
	else
		hugepage_shift = PAGE_SHIFT;//否则，设置移位大小为PAGE_SHIFT。

	pte_num = sz >> hugepage_shift;//计算页表项数量。
	pgsize = 1 << hugepage_shift;//计算页面大小

	if (!pte_present(pte)) {//如果页表项不可用
		for (i = 0; i < pte_num; i++, ptep++, addr += pgsize)//循环设置页表项
			set_ptes(mm, addr, ptep, pte, 1);//设置页表项
		return;
	}

	if (!pte_napot(pte)) {//如果页表项不是NAPOT类型
		set_ptes(mm, addr, ptep, pte, 1);//直接设置页表项
		return;
	}

	clear_flush(mm, addr, ptep, pgsize, pte_num);//清除并刷新页表项

	for (i = 0; i < pte_num; i++, ptep++, addr += pgsize)//循环设置页表项
		set_pte_at(mm, addr, ptep, pte);//设置页表项
}
/*
 * 设置巨页页表项的访问标志
 * */
int huge_ptep_set_access_flags(struct vm_area_struct *vma,
			       unsigned long addr,
			       pte_t *ptep,
			       pte_t pte,
			       int dirty)
{
	struct mm_struct *mm = vma->vm_mm;//获取虚拟内存区域所属的内存管理结构体
	unsigned long order;//页表项顺序
	pte_t orig_pte;//原始页表项
	int i, pte_num;//循环计数器和页表项数量

	if (!pte_napot(pte))//如果页表项不是NAPOT类型
		return ptep_set_access_flags(vma, addr, ptep, pte, dirty);//调用普通的设置访问标志函数

	order = napot_cont_order(pte);//获取NAPOT连续顺序
	pte_num = napot_pte_num(order);//计算NAPOT页表项数量
	ptep = huge_pte_offset(mm, addr, napot_cont_size(order));//获取巨页页表项
	orig_pte = get_clear_contig_flush(mm, addr, ptep, pte_num);//获取、清除连续的页表项，并刷新TLB

	if (pte_dirty(orig_pte))//如果原始页表项是脏页
		pte = pte_mkdirty(pte);//设置新的页表项为脏页

	if (pte_young(orig_pte))//如果原始页表项是young页
		pte = pte_mkyoung(pte);//设置新的页表项young页

	for (i = 0; i < pte_num; i++, addr += PAGE_SIZE, ptep++)//循环设置页表项
		set_pte_at(mm, addr, ptep, pte);//设置页表项

	return true;// 返回成功
}

pte_t huge_ptep_get_and_clear(struct mm_struct *mm,
			      unsigned long addr,
			      pte_t *ptep)
{
	pte_t orig_pte = ptep_get(ptep);
	int pte_num;

	if (!pte_napot(orig_pte))
		return ptep_get_and_clear(mm, addr, ptep);

	pte_num = napot_pte_num(napot_cont_order(orig_pte));

	return get_clear_contig(mm, addr, ptep, pte_num);
}

void huge_ptep_set_wrprotect(struct mm_struct *mm,
			     unsigned long addr,
			     pte_t *ptep)
{
	pte_t pte = ptep_get(ptep);
	unsigned long order;
	pte_t orig_pte;
	int i, pte_num;

	if (!pte_napot(pte)) {
		ptep_set_wrprotect(mm, addr, ptep);
		return;
	}

	order = napot_cont_order(pte);
	pte_num = napot_pte_num(order);
	ptep = huge_pte_offset(mm, addr, napot_cont_size(order));
	orig_pte = get_clear_contig_flush(mm, addr, ptep, pte_num);

	orig_pte = pte_wrprotect(orig_pte);

	for (i = 0; i < pte_num; i++, addr += PAGE_SIZE, ptep++)
		set_pte_at(mm, addr, ptep, orig_pte);
}

pte_t huge_ptep_clear_flush(struct vm_area_struct *vma,
			    unsigned long addr,
			    pte_t *ptep)
{
	pte_t pte = ptep_get(ptep);
	int pte_num;

	if (!pte_napot(pte))
		return ptep_clear_flush(vma, addr, ptep);

	pte_num = napot_pte_num(napot_cont_order(pte));

	return get_clear_contig_flush(vma->vm_mm, addr, ptep, pte_num);
}

void huge_pte_clear(struct mm_struct *mm,
		    unsigned long addr,
		    pte_t *ptep,
		    unsigned long sz)
{
	pte_t pte = ptep_get(ptep);
	int i, pte_num;

	if (!pte_napot(pte)) {
		pte_clear(mm, addr, ptep);
		return;
	}

	pte_num = napot_pte_num(napot_cont_order(pte));
	for (i = 0; i < pte_num; i++, addr += PAGE_SIZE, ptep++)
		pte_clear(mm, addr, ptep);
}

static bool is_napot_size(unsigned long size)
{
	unsigned long order;

	if (!has_svnapot())
		return false;

	for_each_napot_order(order) {
		if (size == napot_cont_size(order))
			return true;
	}
	return false;
}

static __init int napot_hugetlbpages_init(void)
{
	if (has_svnapot()) {
		unsigned long order;

		for_each_napot_order(order)
			hugetlb_add_hstate(order);
	}
	return 0;
}
arch_initcall(napot_hugetlbpages_init);

#else

static bool is_napot_size(unsigned long size)
{
	return false;
}

#endif /*CONFIG_RISCV_ISA_SVNAPOT*/

static bool __hugetlb_valid_size(unsigned long size)
{
	if (size == HPAGE_SIZE)
		return true;
	else if (IS_ENABLED(CONFIG_64BIT) && size == PUD_SIZE)
		return true;
	else if (is_napot_size(size))
		return true;
	else
		return false;
}

bool __init arch_hugetlb_valid_size(unsigned long size)
{
	return __hugetlb_valid_size(size);
}

#ifdef CONFIG_ARCH_ENABLE_HUGEPAGE_MIGRATION
bool arch_hugetlb_migration_supported(struct hstate *h)
{
	return __hugetlb_valid_size(huge_page_size(h));
}
#endif

#ifdef CONFIG_CONTIG_ALLOC
static __init int gigantic_pages_init(void)
{
	/* With CONTIG_ALLOC, we can allocate gigantic pages at runtime */
	if (IS_ENABLED(CONFIG_64BIT))
		hugetlb_add_hstate(PUD_SHIFT - PAGE_SHIFT);
	return 0;
}
arch_initcall(gigantic_pages_init);
#endif
