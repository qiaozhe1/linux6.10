/* SPDX-License-Identifier: GPL-2.0-only */
/* -*- linux-c -*- ------------------------------------------------------- *
 *
 *   Copyright (C) 1991, 1992 Linus Torvalds
 *   Copyright 2007 rPath, Inc. - All Rights Reserved
 *
 * ----------------------------------------------------------------------- */

/*
 * Very simple bitops for the boot code.
 */

#ifndef BOOT_BITOPS_H
#define BOOT_BITOPS_H
#define _LINUX_BITOPS_H		/* Inhibit inclusion of <linux/bitops.h> */

#include <linux/types.h>
#include <asm/asm.h>

static inline bool constant_test_bit(int nr, const void *addr)
{
	const u32 *p = addr;
	return ((1UL << (nr & 31)) & (p[nr >> 5])) != 0;
}
static inline bool variable_test_bit(int nr, const void *addr)
{
	bool v;
	const u32 *p = addr;

	asm("btl %2,%1" CC_SET(c) : CC_OUT(c) (v) : "m" (*p), "Ir" (nr));
	return v;
}

#define test_bit(nr,addr) \
(__builtin_constant_p(nr) ? \		//GCC 提供的一个内建函数，用于判断 nr 是否是一个编译时常量。为常量返回true
 constant_test_bit((nr),(addr)) : \	//直接计算特定位是否被设置，适用于编译时已知的常量位。
 variable_test_bit((nr),(addr)))	//在运行时计算特定位是否被设置，适用于在编译时未知的变量位。

static inline void set_bit(int nr, void *addr)
{
	asm("btsl %1,%0" : "+m" (*(u32 *)addr) : "Ir" (nr));
}

#endif /* BOOT_BITOPS_H */
