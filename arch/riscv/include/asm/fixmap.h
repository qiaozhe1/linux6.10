/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2019 Western Digital Corporation or its affiliates.
 */

#ifndef _ASM_RISCV_FIXMAP_H
#define _ASM_RISCV_FIXMAP_H

#include <linux/kernel.h>
#include <linux/sizes.h>
#include <linux/pgtable.h>
#include <asm/page.h>

#ifdef CONFIG_MMU
/*
 * Here we define all the compile-time 'special' virtual addresses.
 * The point is to have a constant address at compile time, but to
 * set the physical address only in the boot process.
 *
 * These 'compile-time allocated' memory buffers are page-sized. Use
 * set_fixmap(idx,phys) to associate physical memory with fixmap indices.
 */
enum fixed_addresses {
	FIX_HOLE,//标记未使用部分，在实际使用中可能被跳过或作为保留区域。
	/*
	 * The fdt fixmap mapping must be PMD aligned and will be mapped
	 * using PMD entries in fixmap_pmd in 64-bit and a PGD entry in 32-bit.
	 * 与设备树相关的固定映射区索引
	 */
	FIX_FDT_END,//设备树映射区域的结束索引
	FIX_FDT = FIX_FDT_END + FIX_FDT_SIZE / PAGE_SIZE - 1,//指实际使用的设备树区域（实际的开始区域）

	/* Below fixmaps will be mapped using fixmap_pte */
	FIX_PTE,//映射页表不同层级的固定地址索引
	FIX_PMD,
	FIX_PUD,
	FIX_P4D,
	/*用于内核中修改代码段。通常在运行时需要修改内核代码（如动态打补丁）时会用到这些地址。*/
	FIX_TEXT_POKE1,
	FIX_TEXT_POKE0,
	FIX_EARLYCON_MEM_BASE,//这个固定地址索引用于早期控制台的内存映射，允许内核在早期启动阶段（如还没有完全初始化内存管理子系统时）输出调试信息。

	__end_of_permanent_fixed_addresses,//标记永久固定映射地址的结束位置。此后的固定映射地址用于临时用途，如早期 I/O 映射（early_ioremap）。
	/*
	 * Temporary boot-time mappings, used by early_ioremap(),
	 * before ioremap() is functional.
	 */
#define NR_FIX_BTMAPS		(SZ_256K / PAGE_SIZE)
#define FIX_BTMAPS_SLOTS	7
#define TOTAL_FIX_BTMAPS	(NR_FIX_BTMAPS * FIX_BTMAPS_SLOTS)
	/*这些地址用于早期启动阶段的临时映射，通常在内核启动过程中用 early_ioremap() 函数进行硬件资源的访问。定义了多个 FIX_BTMAP 槽位以支持多个并发的临时映射。 */
	FIX_BTMAP_END = __end_of_permanent_fixed_addresses,
	FIX_BTMAP_BEGIN = FIX_BTMAP_END + TOTAL_FIX_BTMAPS - 1,

	__end_of_fixed_addresses//标记整个固定地址范围的结束位置
};

#define __early_set_fixmap	__set_fixmap

#define __late_set_fixmap	__set_fixmap
#define __late_clear_fixmap(idx) __set_fixmap((idx), 0, FIXMAP_PAGE_CLEAR)

extern void __set_fixmap(enum fixed_addresses idx,
			 phys_addr_t phys, pgprot_t prot);

#include <asm-generic/fixmap.h>

#endif /* CONFIG_MMU */
#endif /* _ASM_RISCV_FIXMAP_H */
