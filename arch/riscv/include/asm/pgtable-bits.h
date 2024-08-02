/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#ifndef _ASM_RISCV_PGTABLE_BITS_H
#define _ASM_RISCV_PGTABLE_BITS_H

#define _PAGE_ACCESSED_OFFSET 6

#define _PAGE_PRESENT   (1 << 0)	//表示页表项存在，用于判断页表项是否有效。
#define _PAGE_READ      (1 << 1)    /* Readable 表示页表项可读，用于控制页面的读权限 */
#define _PAGE_WRITE     (1 << 2)    /* Writable 表示页表项可写，用于控制页面的写权限 */
#define _PAGE_EXEC      (1 << 3)    /* Executable 表示页表项可执行，用于控制页面的执行权限 */
#define _PAGE_USER      (1 << 4)    /* User 表示页表项为用户模式，用于区分用户模式和内核模式 */
#define _PAGE_GLOBAL    (1 << 5)    /* Global 表示页表项为全局标志，在所有地址空间中共享 */
#define _PAGE_ACCESSED  (1 << 6)    /* Set by hardware on any access 表示页表项被访问（由硬件设置），用于内存管理和页面替换算法 */
#define _PAGE_DIRTY     (1 << 7)    /* Set by hardware on any write 表示页表项被写（由硬件设置），用于内存管理和页面替换算法 */
#define _PAGE_SOFT      (3 << 8)    /* Reserved for software 表示页表项软件保留标志，预留给软件使用的位 */

#define _PAGE_SPECIAL   (1 << 8)    /* RSW: 0x1 表示页表项特殊标志，具体用途由实现决定 */
#define _PAGE_TABLE     _PAGE_PRESENT //表示页表项为页表标志，通常等同于页表项存在标志

/*
 * _PAGE_PROT_NONE is set on not-present pages (and ignored by the hardware) to
 * distinguish them from swapped out pages
 */
#define _PAGE_PROT_NONE _PAGE_GLOBAL

/* Used for swap PTEs only. */
#define _PAGE_SWP_EXCLUSIVE _PAGE_ACCESSED

#define _PAGE_PFN_SHIFT 10

/*
 * when all of R/W/X are zero, the PTE is a pointer to the next level
 * of the page table; otherwise, it is a leaf PTE.
 */
#define _PAGE_LEAF (_PAGE_READ | _PAGE_WRITE | _PAGE_EXEC)

#endif /* _ASM_RISCV_PGTABLE_BITS_H */
