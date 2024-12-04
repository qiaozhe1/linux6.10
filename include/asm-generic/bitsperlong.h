/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ASM_GENERIC_BITS_PER_LONG
#define __ASM_GENERIC_BITS_PER_LONG

#include <uapi/asm-generic/bitsperlong.h>


#ifdef CONFIG_64BIT
#define BITS_PER_LONG 64
#else
#define BITS_PER_LONG 32
#endif /* CONFIG_64BIT */

/*
 * FIXME: The check currently breaks x86-64 build, so it's
 * temporarily disabled. Please fix x86-64 and reenable
 */
#if 0 && BITS_PER_LONG != __BITS_PER_LONG
#error Inconsistent word size. Check asm/bitsperlong.h
#endif

#ifndef BITS_PER_LONG_LONG
#define BITS_PER_LONG_LONG 64
#endif

/*
 * small_const_nbits(n) is true precisely when it is known at compile-time
 * that BITMAP_SIZE(n) is 1, i.e. 1 <= n <= BITS_PER_LONG. This allows
 * various bit/bitmap APIs to provide a fast inline implementation. Bitmaps
 * of size 0 are very rare, and a compile-time-known-size 0 is most likely
 * a sign of error. They will be handled correctly by the bit/bitmap APIs,
 * but using the out-of-line functions, so that the inline implementations
 * can unconditionally dereference the pointer(s).
 *
 * small_const_nbits(nbits) 是一个用于检查 nbits 是否为一个小范围的编译期常量
 * 的宏，满足条件的 nbits 范围是 (0, BITS_PER_LONG]。
 */
#define small_const_nbits(nbits) \
	/*__builtin_constant_p() 是 GCC 编译器内置的函数，用于判断给定的表达式是否为编译期的常量。
	 * 如果 nbits 是一个编译期的常量（即在编译时可以确定值），则返回 true，否则返回 false
	 * (nbits) <= BITS_PER_LONG: 检查 nbits 是否小于或等于 BITS_PER_LONG。BITS_PER_LONG 通常
	 * 表示一个 unsigned long 类型中所包含的位数。
	 */
	(__builtin_constant_p(nbits) && (nbits) <= BITS_PER_LONG && (nbits) > 0)

#endif /* __ASM_GENERIC_BITS_PER_LONG */
