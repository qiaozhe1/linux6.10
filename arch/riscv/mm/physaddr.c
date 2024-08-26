// SPDX-License-Identifier: GPL-2.0

#include <linux/types.h>
#include <linux/mmdebug.h>
#include <linux/mm.h>
#include <asm/page.h>
#include <asm/sections.h>

phys_addr_t __virt_to_phys(unsigned long x)//将虚拟地址转换为物理地址
{
	/*
	 * Boundary checking aginst the kernel linear mapping space.
	 */
	WARN(!is_linear_mapping(x) && !is_kernel_mapping(x),//边界检查;检查地址 x 是否在内核的线性映射空间或内核映射空间内。
	     "virt_to_phys used for non-linear address: %pK (%pS)\n",
	     (void *)x, (void *)x);

	return __va_to_pa_nodebug(x);
}
EXPORT_SYMBOL(__virt_to_phys);

phys_addr_t __phys_addr_symbol(unsigned long x)//用于将内核符号地址（虚拟地址）转换为对应的物理地址
{
	unsigned long kernel_start = kernel_map.virt_addr;//保存内核映射区域的起始虚拟地址
	unsigned long kernel_end = kernel_start + kernel_map.size;//计算内核映射区域的结束虚拟地址。

	/*
	 * Boundary checking aginst the kernel image mapping.
	 * __pa_symbol should only be used on kernel symbol addresses.
	 */
	VIRTUAL_BUG_ON(x < kernel_start || x > kernel_end);//确保只能用于转换内核虚拟地址

	return __va_to_pa_nodebug(x);//将虚拟地址转换为物理地址。
}
EXPORT_SYMBOL(__phys_addr_symbol);

phys_addr_t linear_mapping_va_to_pa(unsigned long x)//将内核线性映射区域的虚拟地址 x 转换为相应的物理地址
{
	BUG_ON(!kernel_map.va_pa_offset);//检查内核映射的虚拟地址与物理地址的偏移量是否有效

	return ((unsigned long)(x) - kernel_map.va_pa_offset);//通过减去偏移量，将虚拟地址转换为物理地址
}
EXPORT_SYMBOL(linear_mapping_va_to_pa);

void *linear_mapping_pa_to_va(unsigned long x)//将内核线性映射区域的物理地址 x 转换为相应的虚拟地址
{
	BUG_ON(!kernel_map.va_pa_offset);//检查内核映射的虚拟地址与物理地址的偏移量是否有效

	return ((void *)((unsigned long)(x) + kernel_map.va_pa_offset));//通过加上偏移量，将物理地址转换为虚拟地址
}
EXPORT_SYMBOL(linear_mapping_pa_to_va);
