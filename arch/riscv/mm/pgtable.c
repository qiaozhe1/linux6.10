// SPDX-License-Identifier: GPL-2.0

#include <asm/pgalloc.h>
#include <linux/gfp.h>
#include <linux/kernel.h>
#include <linux/pgtable.h>
/*设置页表条目（PTE）的访问标志*/
int ptep_set_access_flags(struct vm_area_struct *vma,
			  unsigned long address, pte_t *ptep,
			  pte_t entry, int dirty)
{
	if (!pte_same(ptep_get(ptep), entry))//检查当前 PTE 是否与新的PTE条目相同.riscv体系在ptep_get(ptep)函数实现上是使用通用代码直接返回PTE的指针。
		__set_pte_at(vma->vm_mm, ptep, entry);//如果不同，更新 PTE
	/*
	 * update_mmu_cache will unconditionally execute, handling both
	 * the case that the PTE changed and the spurious fault case.
	 */
	return true;//返回成功
}
/*
 * 该函数用于检测并清除页表项 (PTE) 中的 "young" 标志位，"young" 标
 * 志位也称为 "accessed" 标志，用于指示页面是否最近被访问过。
 */
int ptep_test_and_clear_young(struct vm_area_struct *vma,
			      unsigned long address,
			      pte_t *ptep)
{
	if (!pte_young(ptep_get(ptep)))//用于检查给定的页表项 ptep 是否设置了 "young" 标志。
		return 0;
	return test_and_clear_bit(_PAGE_ACCESSED_OFFSET, &pte_val(*ptep));//尝试清除这个标志位。该操作不仅清除标志，还会返回标志的原始状态。
}
EXPORT_SYMBOL_GPL(ptep_test_and_clear_young);

#ifdef CONFIG_64BIT
pud_t *pud_offset(p4d_t *p4d, unsigned long address)//用于获取指定虚拟地址 address 在四级页表中的 pud_t 页表项指针。
{
	if (pgtable_l4_enabled)//一个全局变量，用于指示当前系统是否启用了四级页表结构
		return p4d_pgtable(p4dp_get(p4d)) + pud_index(address);//从 p4d 页表项中提取 PUD 页表的基地址

	return (pud_t *)p4d;
}

p4d_t *p4d_offset(pgd_t *pgd, unsigned long address)//用于获取指定虚拟地址 address 在五级页表中的 p4d_t 页表项指针
{
	if (pgtable_l5_enabled)//用于指示当前系统是否启用了五级页表结构
		return pgd_pgtable(pgdp_get(pgd)) + p4d_index(address);//从 pgd 页表项中提取 P4D 页表的基地址。

	return (p4d_t *)pgd;
}
#endif

#ifdef CONFIG_HAVE_ARCH_HUGE_VMAP
int p4d_set_huge(p4d_t *p4d, phys_addr_t addr, pgprot_t prot)
{
	return 0;
}

void p4d_clear_huge(p4d_t *p4d)
{
}

int pud_set_huge(pud_t *pud, phys_addr_t phys, pgprot_t prot)//用于在页上部目录PUD中设置一个大页（Huge Page）的映射
{
	pud_t new_pud = pfn_pud(__phys_to_pfn(phys), prot);//将物理地址转换为页帧号，并结合页表项保护标志创建一个新的 PUD 表项

	set_pud(pud, new_pud);//将新创建的PUD表项写入到指定的PUD位置，建立物理地址与虚拟地址的映射关系
	return 1;
}

int pud_clear_huge(pud_t *pud)//用于清除一个大页的PUD映射，将PUD表项置为无效，解除物理地址与虚拟地址的映射。
{
	if (!pud_leaf(pudp_get(pud)))//判断当前 PUD 表项是否为叶子节点。叶子节点表示该表项直接指向物理内存，而非下一级页表。
		return 0;
	pud_clear(pud);//清除该表项，将其置为无效。
	return 1;
}

int pud_free_pmd_page(pud_t *pud, unsigned long addr)//用于释放一个 PUD 条目及其下属的 PMD 页表和关联的 PTE 页表(或是巨页？)
{
	pmd_t *pmd = pud_pgtable(pudp_get(pud));//从 PUD 条目中提取与其关联的 PMD 页表的地址。
	int i;

	pud_clear(pud);//将 PUD 条目清除，使其无效。

	flush_tlb_kernel_range(addr, addr + PUD_SIZE);//刷新指定虚拟地址范围内的 TLB，确保该范围内的缓存条目失效，从而避免使用已清除的PUD条目。

	for (i = 0; i < PTRS_PER_PMD; i++) {//遍历 PMD 页表中的所有表项，逐一检查它们是否有效。
		if (!pmd_none(pmd[i])) {//如果 PMD 表项有效
			pte_t *pte = (pte_t *)pmd_page_vaddr(pmd[i]);//获取与其关联的 PTE 页表的虚拟地址

			pte_free_kernel(NULL, pte);//释放该 PTE 页表的内存。
		}
	}

	pmd_free(NULL, pmd);//释放 PMD 页表的内存

	return 1;
}

int pmd_set_huge(pmd_t *pmd, phys_addr_t phys, pgprot_t prot)//用于将一个 PMD 条目设置为指向一个 "Huge Page"（大页）
{
	pmd_t new_pmd = pfn_pmd(__phys_to_pfn(phys), prot);//首先，函数将物理地址 phys 转换为页帧号（PFN），然后结合指定的页表属性 prot，创建一个新的 PMD 条目。

	set_pmd(pmd, new_pmd);//将创建的新的 PMD 条目写入指定的 PMD 位置。
	return 1;
}

int pmd_clear_huge(pmd_t *pmd)//用于清除一个指向大页的 PMD 条目，将其设置为无效。
{
	if (!pmd_leaf(pmdp_get(pmd)))//检查给定的 PMD 条目是否为叶节点（即是否指向大页）。如果不是，则不需要清除。
		return 0;
	pmd_clear(pmd);//清除该条目，使其无效。
	return 1;
}

int pmd_free_pte_page(pmd_t *pmd, unsigned long addr)//释放与给定 PMD 条目相关联的 PTE 页(PTE页表占有的页,或是巨页？)，并清除相关的页表项
{
	pte_t *pte = (pte_t *)pmd_page_vaddr(pmdp_get(pmd));//获取与PMD条目对应的PTE页的虚拟地址.pmdp_get(pmd) 返回存储在 PMD 条目中的物理页框号

	pmd_clear(pmd);//清除 PMD 条目，将其设置为无效。

	flush_tlb_kernel_range(addr, addr + PMD_SIZE);//刷新 TLB 缓存，以确保任何涉及该地址范围的缓存条目被清除。
	pte_free_kernel(NULL, pte);//释放 PTE 页，将其内存归还给内核的内存管理系统。
	return 1;
}

#endif /* CONFIG_HAVE_ARCH_HUGE_VMAP */
#ifdef CONFIG_TRANSPARENT_HUGEPAGE
/*
 * 用于在透明巨页（Transparent HugePages）启用时，清除普通的页面目录中间条目 (PMD) ，以便于将其折叠成一个巨页 PMD
 */
pmd_t pmdp_collapse_flush(struct vm_area_struct *vma,
					unsigned long address, pmd_t *pmdp)
{
	pmd_t pmd = pmdp_huge_get_and_clear(vma->vm_mm, address, pmdp);//获取并清除与给定地址对应的 PMD 条目，返回原始的 PMD。主要是确保旧的 PMD 被清除，以便重新映射为巨页。

	VM_BUG_ON(address & ~HPAGE_PMD_MASK);//确保地址与巨页的对齐掩码一致
	VM_BUG_ON(pmd_trans_huge(pmdp_get(pmdp)));//确保在调用函数时 PMD 条目不再是巨页
	/*
	 * When leaf PTE entries (regular pages) are collapsed into a leaf
	 * PMD entry (huge page), a valid non-leaf PTE is converted into a
	 * valid leaf PTE at the level 1 page table.  Since the sfence.vma
	 * forms that specify an address only apply to leaf PTEs, we need a
	 * global flush here.  collapse_huge_page() assumes these flushes are
	 * eager, so just do the fence here.
	 * 当叶子 PTE 条目（常规页面）被折叠成叶子 PMD 条目（巨页）时
	 * 有效的非叶子 PTE 被转换为有效的叶子 PTE。由于 sfence.vma 指令
	 * 仅对叶子 PTE 生效，因此这里需要进行全局刷新操作。
	 * collapse_huge_page() 函数假设这些刷新是即时的，所以我们在这里执行刷新。
	 */
	flush_tlb_mm(vma->vm_mm);//刷新整个内存管理单元 (MMU) 的 TLB，确保所有缓存的旧条目被清除。
	return pmd;//返回原始的 PMD 条目。
}
#endif /* CONFIG_TRANSPARENT_HUGEPAGE */
