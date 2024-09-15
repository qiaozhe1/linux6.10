// SPDX-License-Identifier: GPL-2.0-or-later
/* bit search implementation
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * Copyright (C) 2008 IBM Corporation
 * 'find_last_bit' is written by Rusty Russell <rusty@rustcorp.com.au>
 * (Inspired by David Howell's find_next_bit implementation)
 *
 * Rewritten by Yury Norov <yury.norov@gmail.com> to decrease
 * size and improve performance, 2015.
 */

#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/swab.h>

/*
 * Common helper for find_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 */
#define FIND_FIRST_BIT(FETCH, MUNGE, size)					\
({										\
	unsigned long idx, val, sz = (size);					\
										\
	for (idx = 0; idx * BITS_PER_LONG < sz; idx++) {			\
		val = (FETCH);							\
		if (val) {							\
			sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(val)), sz);	\
			break;							\
		}								\
	}									\
										\
	sz;									\
})

/*
 * Common helper for find_next_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 * @start: The bitnumber to start searching at
 */
#define FIND_NEXT_BIT(FETCH, MUNGE, size, start)				\
({										\
	unsigned long mask, idx, tmp, sz = (size), __start = (start);		\
										\
	if (unlikely(__start >= sz))						\
		goto out;							\
										\
	mask = MUNGE(BITMAP_FIRST_WORD_MASK(__start));				\
	idx = __start / BITS_PER_LONG;						\
										\
	for (tmp = (FETCH) & mask; !tmp; tmp = (FETCH)) {			\
		if ((idx + 1) * BITS_PER_LONG >= sz)				\
			goto out;						\
		idx++;								\
	}									\
										\
	sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(tmp)), sz);			\
out:										\
	sz;									\
})

#define FIND_NTH_BIT(FETCH, size, num)						\
({										\
	unsigned long sz = (size), nr = (num), idx, w, tmp;			\
										\
	for (idx = 0; (idx + 1) * BITS_PER_LONG <= sz; idx++) {			\
		if (idx * BITS_PER_LONG + nr >= sz)				\
			goto out;						\
										\
		tmp = (FETCH);							\
		w = hweight_long(tmp);						\
		if (w > nr)							\
			goto found;						\
										\
		nr -= w;							\
	}									\
										\
	if (sz % BITS_PER_LONG)							\
		tmp = (FETCH) & BITMAP_LAST_WORD_MASK(sz);			\
found:										\
	sz = idx * BITS_PER_LONG + fns(tmp, nr);				\
out:										\
	sz;									\
})

#ifndef find_first_bit
/*
 * Find the first set bit in a memory region.
 */
unsigned long _find_first_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_bit);
#endif

#ifndef find_first_and_bit
/*
 * Find the first set bit in two memory regions.
 */
unsigned long _find_first_and_bit(const unsigned long *addr1,
				  const unsigned long *addr2,
				  unsigned long size)
{
	return FIND_FIRST_BIT(addr1[idx] & addr2[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_and_bit);
#endif

/*
 * Find the first set bit in three memory regions.
 */
unsigned long _find_first_and_and_bit(const unsigned long *addr1,
				      const unsigned long *addr2,
				      const unsigned long *addr3,
				      unsigned long size)
{
	return FIND_FIRST_BIT(addr1[idx] & addr2[idx] & addr3[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_and_and_bit);

#ifndef find_first_zero_bit
/*
 * Find the first cleared bit in a memory region.
 */
unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(~addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_zero_bit);
#endif

#ifndef find_next_bit
unsigned long _find_next_bit(const unsigned long *addr, unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_bit);
#endif

unsigned long __find_nth_bit(const unsigned long *addr, unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_bit);

unsigned long __find_nth_and_bit(const unsigned long *addr1, const unsigned long *addr2,
				 unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & addr2[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_and_bit);

unsigned long __find_nth_andnot_bit(const unsigned long *addr1, const unsigned long *addr2,
				 unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & ~addr2[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_andnot_bit);

unsigned long __find_nth_and_andnot_bit(const unsigned long *addr1,
					const unsigned long *addr2,
					const unsigned long *addr3,
					unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & addr2[idx] & ~addr3[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_and_andnot_bit);

#ifndef find_next_and_bit
unsigned long _find_next_and_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] & addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_and_bit);
#endif

#ifndef find_next_andnot_bit
unsigned long _find_next_andnot_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] & ~addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_andnot_bit);
#endif

#ifndef find_next_or_bit
unsigned long _find_next_or_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] | addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_or_bit);
#endif

#ifndef find_next_zero_bit
unsigned long _find_next_zero_bit(const unsigned long *addr, unsigned long nbits,
					 unsigned long start)
{
	return FIND_NEXT_BIT(~addr[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_zero_bit);
#endif

#ifndef find_last_bit
/*
 * 逐字向前查找位图中最后一个被设置的位。
 * 它从最后一个字开始，通过掩码屏蔽无效位，检查每一个字中的位。
 * 当找到设置的位时，计算并返回其位置；如果没有找到，则返回 size。
 * */
unsigned long _find_last_bit(const unsigned long *addr, unsigned long size)
{
	if (size) {//如果 size 为非零，表示有位需要检查
		unsigned long val = BITMAP_LAST_WORD_MASK(size);//计算位图最后一个字的掩码,确保仅检查有效的位
		unsigned long idx = (size-1) / BITS_PER_LONG;/// 计算位图中最后一个有效字的索引

		do {//从最后一个字开始，逐个向前查找，直到找到被设置的位
			val &= addr[idx];//过滤掉当前字中无效的位，仅保留有效位
			if (val)//如果当前字中有任何位被设置
				return idx * BITS_PER_LONG + __fls(val);//返回最高有效位的位号，该位号是 idx 乘以每个字的位数，再加上当前字的最高有效位位置
			/* 如果当前字没有 1，则继续检查前一个字*/
			val = ~0ul;//重置掩码为全 1，继续向前查找
		} while (idx--);//循环，直到 idx 递减到 0
	}
	return size;// 如果没有找到设置为 1 的位，或者 size 为 0，则返回 size
}
EXPORT_SYMBOL(_find_last_bit);
#endif

unsigned long find_next_clump8(unsigned long *clump, const unsigned long *addr,
			       unsigned long size, unsigned long offset)
{
	offset = find_next_bit(addr, size, offset);
	if (offset == size)
		return size;

	offset = round_down(offset, 8);
	*clump = bitmap_get_value8(addr, offset);

	return offset;
}
EXPORT_SYMBOL(find_next_clump8);

#ifdef __BIG_ENDIAN

#ifndef find_first_zero_bit_le
/*
 * Find the first cleared bit in an LE memory region.
 */
unsigned long _find_first_zero_bit_le(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(~addr[idx], swab, size);
}
EXPORT_SYMBOL(_find_first_zero_bit_le);

#endif

#ifndef find_next_zero_bit_le
unsigned long _find_next_zero_bit_le(const unsigned long *addr,
					unsigned long size, unsigned long offset)
{
	return FIND_NEXT_BIT(~addr[idx], swab, size, offset);
}
EXPORT_SYMBOL(_find_next_zero_bit_le);
#endif

#ifndef find_next_bit_le
unsigned long _find_next_bit_le(const unsigned long *addr,
				unsigned long size, unsigned long offset)
{
	return FIND_NEXT_BIT(addr[idx], swab, size, offset);
}
EXPORT_SYMBOL(_find_next_bit_le);

#endif

#endif /* __BIG_ENDIAN */
