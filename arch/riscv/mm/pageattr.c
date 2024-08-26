// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (C) 2019 SiFive
 */

#include <linux/pagewalk.h>
#include <linux/pgtable.h>
#include <linux/vmalloc.h>
#include <asm/tlbflush.h>
#include <asm/bitops.h>
#include <asm/set_memory.h>

struct pageattr_masks {
	pgprot_t set_mask;
	pgprot_t clear_mask;
};
/*根据提供的掩码修改页表项的属性*/
static unsigned long set_pageattr_masks(unsigned long val, struct mm_walk *walk)
{
	struct pageattr_masks *masks = walk->private;//从 mm_walk 结构体中获取包含设置和清除掩码的 pageattr_masks 结构体。
	unsigned long new_val = val;//初始化一个新值，初始值为传入的 val

	new_val &= ~(pgprot_val(masks->clear_mask));// 清除指定的掩码位，通过按位与非操作符对 new_val 进行操作，将 clear_mask 中的位清零。
	new_val |= (pgprot_val(masks->set_mask));//设置指定的掩码位，通过按位或操作符对 new_val 进行操作，将 set_mask 中的位设置为 1。

	return new_val;//返回修改后的值
}

static int pageattr_p4d_entry(p4d_t *p4d, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	p4d_t val = p4dp_get(p4d);

	if (p4d_leaf(val)) {
		val = __p4d(set_pageattr_masks(p4d_val(val), walk));
		set_p4d(p4d, val);
	}

	return 0;
}

static int pageattr_pud_entry(pud_t *pud, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pud_t val = pudp_get(pud);

	if (pud_leaf(val)) {
		val = __pud(set_pageattr_masks(pud_val(val), walk));
		set_pud(pud, val);
	}

	return 0;
}

static int pageattr_pmd_entry(pmd_t *pmd, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pmd_t val = pmdp_get(pmd);

	if (pmd_leaf(val)) {
		val = __pmd(set_pageattr_masks(pmd_val(val), walk));
		set_pmd(pmd, val);
	}

	return 0;
}
/*根据特定的属性掩码修改 PTE，并将更新后的 PTE 写回页表。*/
static int pageattr_pte_entry(pte_t *pte, unsigned long addr,
			      unsigned long next, struct mm_walk *walk)
{
	pte_t val = ptep_get(pte);//从指定的页表项中获取当前的 PTE 值

	val = __pte(set_pageattr_masks(pte_val(val), walk));//使用 set_pageattr_masks 函数对当前 PTE 值进行修改，然后将其转换回 pte_t 类型。
	set_pte(pte, val);//将修改后的 PTE 值写回到页表中

	return 0;//返回 0 表示成功。
}

static int pageattr_pte_hole(unsigned long addr, unsigned long next,
			     int depth, struct mm_walk *walk)
{
	/* Nothing to do here */
	return 0;
}
/*修改页表属性的回调函数*/
static const struct mm_walk_ops pageattr_ops = {
	.p4d_entry = pageattr_p4d_entry,
	.pud_entry = pageattr_pud_entry,
	.pmd_entry = pageattr_pmd_entry,
	.pte_entry = pageattr_pte_entry,
	.pte_hole = pageattr_pte_hole,
	.walk_lock = PGWALK_RDLOCK,
};
/*拆分PMD页表*/
#ifdef CONFIG_64BIT
static int __split_linear_mapping_pmd(pud_t *pudp,
				      unsigned long vaddr, unsigned long end)
{
	pmd_t *pmdp;
	unsigned long next;

	pmdp = pmd_offset(pudp, vaddr);//获取 PMD 表项的指针

	do {
		next = pmd_addr_end(vaddr, end);//计算当前 PMD 表项的地址范围的结束地址。

		if (next - vaddr >= PMD_SIZE &&
		    vaddr <= (vaddr & PMD_MASK) && end >= next)
			continue;

		if (pmd_leaf(pmdp_get(pmdp))) {//如果 PMD 是一个叶表项（即指向一个物理页）。
			struct page *pte_page;
			unsigned long pfn = _pmd_pfn(pmdp_get(pmdp));// 获取 PMD 表项中存储的页框号。
			pgprot_t prot = __pgprot(pmd_val(pmdp_get(pmdp)) & ~_PAGE_PFN_MASK);//获取 PMD 表项中的权限标志。
			pte_t *ptep_new;
			int i;

			pte_page = alloc_page(GFP_KERNEL);//为新的 PTE 表分配一页内存。
			if (!pte_page)
				return -ENOMEM;

			ptep_new = (pte_t *)page_address(pte_page);//获取分配页的虚拟地址。
			for (i = 0; i < PTRS_PER_PTE; ++i, ++ptep_new)
				set_pte(ptep_new, pfn_pte(pfn + i, prot));//设置每个 PTE 表项，指向原始 PMD 指向的物理页。

			smp_wmb();//写屏障

			set_pmd(pmdp, pfn_pmd(page_to_pfn(pte_page), PAGE_TABLE));//更新 PMD 表项，使其指向新的 PTE 表
		}
	} while (pmdp++, vaddr = next, vaddr != end);//更新 PMD 指针和地址，继续处理下一个地址范围。

	return 0;
}
/*拆分PUD页表*/
static int __split_linear_mapping_pud(p4d_t *p4dp,
				      unsigned long vaddr, unsigned long end)
{
	pud_t *pudp;
	unsigned long next;
	int ret;

	pudp = pud_offset(p4dp, vaddr);//获取 PUD 表项的指针。

	do {
		next = pud_addr_end(vaddr, end); //计算当前 PUD 表项的地址范围的结束地址。

		if (next - vaddr >= PUD_SIZE &&
		    vaddr <= (vaddr & PUD_MASK) && end >= next)//检查当前范围是否涵盖整个PUD项，如果是，则不>      需要拆分。              

			continue;

		if (pud_leaf(pudp_get(pudp))) {//如果 PUD 是一个页表项（即指向一个物理页）。
			struct page *pmd_page;//定义 PMD 页表的页指针。
			unsigned long pfn = _pud_pfn(pudp_get(pudp));//获取 PUD 表项中存储的页框号。
			pgprot_t prot = __pgprot(pud_val(pudp_get(pudp)) & ~_PAGE_PFN_MASK);//获取 PUD 表项中的权限标志。
			pmd_t *pmdp_new;//定义新的 PMD 表项指针。
			int i;//用于遍历的计数器。

			pmd_page = alloc_page(GFP_KERNEL);//为新的 PMD 表分配一页内存
			if (!pmd_page)//如果分配失败，返回错误代码。
				return -ENOMEM;

			pmdp_new = (pmd_t *)page_address(pmd_page);//获取分配页的虚拟地址
			for (i = 0; i < PTRS_PER_PMD; ++i, ++pmdp_new)//填充 PMD 表项。
				set_pmd(pmdp_new,
					pfn_pmd(pfn + ((i * PMD_SIZE) >> PAGE_SHIFT), prot));//设置每个 PMD 表项，指向原始 PUD 指向的物理页

			smp_wmb();//写屏障

			set_pud(pudp, pfn_pud(page_to_pfn(pmd_page), PAGE_TABLE));//更新 PUD 表项，使其指向新的 PMD 表。
170                 }
		}

		ret = __split_linear_mapping_pmd(pudp, vaddr, next);//递归处理下一级页表（PMD）。
		if (ret)
			return ret;//如果递归处理返回错误，退出循环
	} while (pudp++, vaddr = next, vaddr != end);//更新 PUD 指针和地址，继续处理下一个地址范围。

	return 0;
}
/*拆分P4d页表*/
static int __split_linear_mapping_p4d(pgd_t *pgdp,
				      unsigned long vaddr, unsigned long end)
{
	p4d_t *p4dp;//定义指针p4dp，指向P4D表项
	unsigned long next;//用于保存下一个需要处理的虚拟地址范围的结束地址。
	int ret;//用于保存下层函数的返回值，以检测是否出现错误。

	p4dp = p4d_offset(pgdp, vaddr);//获取给定虚拟地址vaddr对应的P4D表项的指针p4dp

	do {
		next = p4d_addr_end(vaddr, end);//计算当前P4D项所覆盖的虚拟地址范围的结束地址next。

		/*
		 * If [vaddr; end] contains [vaddr & P4D_MASK; next], we don't
		 * need to split, we'll change the protections on the whole P4D.
		 * 如果[vaddr; end]包含[vaddr & P4D_MASK; next]，则不需要拆分，
		 * 我们将直接更改整个P4D的权限。
		 */
		if (next - vaddr >= P4D_SIZE &&
		    vaddr <= (vaddr & P4D_MASK) && end >= next)//检查当前范围是否涵盖整个P4D项，如果是，则不需要拆分。
			continue;//满足条件，直接继续处理下一个P4D项。

		if (p4d_leaf(p4dp_get(p4dp))) {//检查当前P4D项是否为叶子节点，即直接映射的物理页。
			struct page *pud_page;//定义指针pud_page，用于存储新分配的PUD（页中间目录）表页
			unsigned long pfn = _p4d_pfn(p4dp_get(p4dp));//获取当前P4D项中的页帧号（PFN）。
			pgprot_t prot = __pgprot(p4d_val(p4dp_get(p4dp)) & ~_PAGE_PFN_MASK);//获取当前P4D项的保护属性（权限）。
			pud_t *pudp_new;//定义指针pudp_new，指向新分配的PUD表项
			int i;

			pud_page = alloc_page(GFP_KERNEL);//分配一个新的页来存放PUD表。一个p4d映射的巨页大小为512G,一个PUD页表项可以管理1G，一个PUD页表有521个页表项。因此只需要分配一个PUD页表就可以映射由P4D管理的巨页
			if (!pud_page)
				return -ENOMEM;//如果分配失败，返回内存不足错误。

			/*
			 * Fill the pud level with leaf puds that have the same
			 * protections as the leaf p4d.
			 * 用与叶子P4D相同的保护属性填充PUD级别的叶子PUD
			 */
			pudp_new = (pud_t *)page_address(pud_page);//获取新分配的PUD表页的内存地址。
			for (i = 0; i < PTRS_PER_PUD; ++i, ++pudp_new)// 循环填充PUD表，设置与原P4D相同的映射。
				set_pud(pudp_new,
					pfn_pud(pfn + ((i * PUD_SIZE) >> PAGE_SHIFT), prot));// 设置每个PUD表项的页帧号和权限。

			/*
			 * Make sure the pud filling is not reordered with the
			 * p4d store which could result in seeing a partially
			 * filled pud level.
			 * 确保PUD填充操作不会与P4D存储操作重新排序，这可能导致
			 * 看到部分填充的PUD级别。
			 */
			smp_wmb();//使用写内存屏障确保前面的写操作不会被重新排序。

			set_p4d(p4dp, pfn_p4d(page_to_pfn(pud_page), PAGE_TABLE));//将P4D表项更新为新分配的PUD表
		}

		ret = __split_linear_mapping_pud(p4dp, vaddr, next);//处理下层PUD表项
		if (ret)
			return ret;//如果发生错误，则直接返回错误码
	} while (p4dp++, vaddr = next, vaddr != end);//循环处理下一个P4D表项，直到处理完所有的虚拟地址范围。

	return 0;
}
/*
 * 该函数用于拆分线性映射中的PGD（Page Global Directory，页全局目录）项。线性映射通
 * 常是内核将物理地址直接映射到虚拟地址的方式，以便内核能够直接访问物理内存。
 *
 * 线性映射的拆分是为了更精细地控制内存区域的映射和权限设置。这个函数逐个遍历PGD项，
 * 并调用 __split_linear_mapping_p4d 处理与这些PGD项关联的P4D项，从而递归地继续向下
 * 层处理。
 * */
static int __split_linear_mapping_pgd(pgd_t *pgdp,
				      unsigned long vaddr,
				      unsigned long end)
{
	unsigned long next;//用于保存下一个需要处理的虚拟地址范围的结束地址。
	int ret;//用于保存__split_linear_mapping_p4d函数的返回值

	do {
		next = pgd_addr_end(vaddr, end);//计算当前PGD项对应的虚拟地址范围的结束地址next
		/* We never use PGD mappings for the linear mapping */
		ret = __split_linear_mapping_p4d(pgdp, vaddr, next);//处理当前PGD项指向的P4D表项，并传递当前虚拟地址范围。
		if (ret)
			return ret;//如果出现错误（即ret不为0），则直接返回该错误码。
	} while (pgdp++, vaddr = next, vaddr != end);//循环更新pgdp指针和vaddr，继续处理下一个PGD项，直到处理完所有的虚拟地址范围。

	return 0;//成功完成所有PGD项的处理，返回0表示没有错误。
}

static int split_linear_mapping(unsigned long start, unsigned long end)
{
	return __split_linear_mapping_pgd(pgd_offset_k(start), start, end);
}
#endif	/* CONFIG_64BIT */
/*
 * 用于设置内存页面的属性（如可读、可写、可执行等）。该函数通过遍历指定的页面范围，
 * 应用给定的掩码来修改页面的属性。
 */
static int __set_memory(unsigned long addr, int numpages, pgprot_t set_mask,
			pgprot_t clear_mask)
{
	int ret;
	unsigned long start = addr;//记录起始地址
	unsigned long end = start + PAGE_SIZE * numpages;//计算结束地址，等于开始地址加上页面大小乘以页数
	unsigned long __maybe_unused lm_start;//用于保存线性映射开始地址
	unsigned long __maybe_unused lm_end;//用于保存线性映射结束地址
	struct pageattr_masks masks = {//初始化页面属性掩码结构体
		.set_mask = set_mask,//设置掩码
		.clear_mask = clear_mask//清除掩码
	};

	if (!numpages)//如果页面数量为0，直接返回0
		return 0;

	mmap_write_lock(&init_mm);//锁定内存映射，以防止并发修改

#ifdef CONFIG_64BIT
	/*
	 * We are about to change the permissions of a kernel mapping, we must
	 * apply the same changes to its linear mapping alias, which may imply
	 * splitting a huge mapping.
	 * 我们即将更改内核映射的权限，因此我们必须将相同的更改应用于其线性映
	 * 射别名，这可能意味着需要拆分大页面映射。
	 */

	if (is_vmalloc_or_module_addr((void *)start)) {//如果起始地址属于vmalloc或模块地址空间
		struct vm_struct *area = NULL;//用于保存虚拟内存区域的指针
		int i, page_start;// 用于保存页面索引

		area = find_vm_area((void *)start);//查找虚拟内存区域
		page_start = (start - (unsigned long)area->addr) >> PAGE_SHIFT;//计算起始页面索引

		for (i = page_start; i < page_start + numpages; ++i) {//遍历所有需要处理的页面
			lm_start = (unsigned long)page_address(area->pages[i]);//获取页面对应的线性地址
			lm_end = lm_start + PAGE_SIZE;//计算页面结束地址

			ret = split_linear_mapping(lm_start, lm_end);//如果需要，拆分线性映射
			if (ret)
				goto unlock;//如果拆分失败，跳转到解锁标签

			ret = walk_page_range_novma(&init_mm, lm_start, lm_end,
						    &pageattr_ops, NULL, &masks);//遍历页面范围，并应用属性
			if (ret)
				goto unlock;//如果遍历失败，跳转到解锁标签
		}
	} else if (is_kernel_mapping(start) || is_linear_mapping(start)) {//如果地址属于内核映射或线性映射
		if (is_kernel_mapping(start)) {//如果是内核映射
			lm_start = (unsigned long)lm_alias(start);//获取线性映射的别名开始地址
			lm_end = (unsigned long)lm_alias(end);//获取线性映射的别名结束地址
		} else {
			lm_start = start;//否则直接使用起始地址
			lm_end = end;//直接使用结束地址
		}

		ret = split_linear_mapping(lm_start, lm_end);//如果需要，拆分线性映射
		if (ret)
			goto unlock;//如果拆分失败，跳转到解锁标签

		ret = walk_page_range_novma(&init_mm, lm_start, lm_end,
					    &pageattr_ops, NULL, &masks);// 遍历页面范围，并应用属性
		if (ret)
			goto unlock;//如果遍历失败，跳转到解锁标签
	}

	ret =  walk_page_range_novma(&init_mm, start, end, &pageattr_ops, NULL,
				     &masks);//遍历页面范围，并应用属性

unlock:
	mmap_write_unlock(&init_mm);//解锁内存映射

	/*
	 * We can't use flush_tlb_kernel_range() here as we may have split a
	 * hugepage that is larger than that, so let's flush everything.
	 * 我们不能在这里使用flush_tlb_kernel_range()，因为我们可能已经拆分了
	 * 一个比这更大的大页面，所以让我们刷新所有TLB。
	 */
	flush_tlb_all();//刷新所有TLB
#else
	ret =  walk_page_range_novma(&init_mm, start, end, &pageattr_ops, NULL,
				     &masks);//在32位系统上直接遍历页面范围，并应用属性

	mmap_write_unlock(&init_mm);//解锁内存映射

	flush_tlb_kernel_range(start, end);//刷新指定范围内的TLB
#endif

	return ret;//返回函数执行结果
}

int set_memory_rw_nx(unsigned long addr, int numpages)
{
	return __set_memory(addr, numpages, __pgprot(_PAGE_READ | _PAGE_WRITE),
			    __pgprot(_PAGE_EXEC));
}

int set_memory_ro(unsigned long addr, int numpages)
{
	return __set_memory(addr, numpages, __pgprot(_PAGE_READ),
			    __pgprot(_PAGE_WRITE));
}

int set_memory_rw(unsigned long addr, int numpages)
{
	return __set_memory(addr, numpages, __pgprot(_PAGE_READ | _PAGE_WRITE),
			    __pgprot(0));
}

int set_memory_x(unsigned long addr, int numpages)
{
	return __set_memory(addr, numpages, __pgprot(_PAGE_EXEC), __pgprot(0));
}

int set_memory_nx(unsigned long addr, int numpages)
{
	return __set_memory(addr, numpages, __pgprot(0), __pgprot(_PAGE_EXEC));
}

int set_direct_map_invalid_noflush(struct page *page)
{
	return __set_memory((unsigned long)page_address(page), 1,
			    __pgprot(0), __pgprot(_PAGE_PRESENT));
}

int set_direct_map_default_noflush(struct page *page)
{
	return __set_memory((unsigned long)page_address(page), 1,
			    PAGE_KERNEL, __pgprot(_PAGE_EXEC));
}

#ifdef CONFIG_DEBUG_PAGEALLOC
static int debug_pagealloc_set_page(pte_t *pte, unsigned long addr, void *data)
{
	int enable = *(int *)data;

	unsigned long val = pte_val(ptep_get(pte));

	if (enable)
		val |= _PAGE_PRESENT;
	else
		val &= ~_PAGE_PRESENT;

	set_pte(pte, __pte(val));

	return 0;
}

void __kernel_map_pages(struct page *page, int numpages, int enable)
{
	if (!debug_pagealloc_enabled())
		return;

	unsigned long start = (unsigned long)page_address(page);
	unsigned long size = PAGE_SIZE * numpages;

	apply_to_existing_page_range(&init_mm, start, size, debug_pagealloc_set_page, &enable);

	flush_tlb_kernel_range(start, start + size);
}
#endif
/*
 * 检查给定的内核页面是否在内存中存在
 */
bool kernel_page_present(struct page *page)
{
	unsigned long addr = (unsigned long)page_address(page);//获取页面对应的虚拟地址。
	pgd_t *pgd;
	pud_t *pud;
	p4d_t *p4d;
	pmd_t *pmd;
	pte_t *pte;

	pgd = pgd_offset_k(addr);//获取虚拟地址对应的PGD项
	if (!pgd_present(pgdp_get(pgd)))//检查PGD项是否有效
		return false;//如果PGD无效，说明页面不在内存中
	if (pgd_leaf(pgdp_get(pgd)))//检查PGD项是否为叶子节点（映射大页）
		return true;//如果PGD是叶子节点，页面在内存中

	p4d = p4d_offset(pgd, addr);//获取虚拟地址对应的P4D项
	if (!p4d_present(p4dp_get(p4d)))//检查P4D项是否有效
		return false;//如果P4D无效，页面不在内存中
	if (p4d_leaf(p4dp_get(p4d)))//检查P4D项是否为叶子节点（映射大页）
		return true;//如果P4D是叶子节点，页面在内存中

	pud = pud_offset(p4d, addr);//获取虚拟地址对应的PUD项
	if (!pud_present(pudp_get(pud)))//检查PUD项是否有效
		return false;//如果PUD无效，页面不在内存中
	if (pud_leaf(pudp_get(pud)))//检查PUD项是否为叶子节点（映射大页）
		return true;//如果PUD是叶子节点，页面在内存中

	pmd = pmd_offset(pud, addr);//获取虚拟地址对应的PMD项
	if (!pmd_present(pmdp_get(pmd)))//检查PMD项是否有效
		return false;//如果PMD无效，页面不在内存中
	if (pmd_leaf(pmdp_get(pmd)))//检查PMD项是否为叶子节点（映射大页）
		return true;//如果PMD是叶子节点，页面在内存中

	pte = pte_offset_kernel(pmd, addr);//获取虚拟地址对应的PTE项
	return pte_present(ptep_get(pte));//检查PTE项是否有效，如果有效，则页面在内存中
}
