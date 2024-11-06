/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * sched_clock.h: support for extending counters to full 64-bit ns counter
 */
#ifndef LINUX_SCHED_CLOCK
#define LINUX_SCHED_CLOCK

#include <linux/types.h>

#ifdef CONFIG_GENERIC_SCHED_CLOCK
/**
 * struct clock_read_data - data required to read from sched_clock()
 *
 * @epoch_ns:		sched_clock() value at last update
 * @epoch_cyc:		Clock cycle value at last update.
 * @sched_clock_mask:   Bitmask for two's complement subtraction of non 64bit
 *			clocks.
 * @read_sched_clock:	Current clock source (or dummy source when suspended).
 * @mult:		Multiplier for scaled math conversion.
 * @shift:		Shift value for scaled math conversion.
 *
 * Care must be taken when updating this structure; it is read by
 * some very hot code paths. It occupies <=40 bytes and, when combined
 * with the seqcount used to synchronize access, comfortably fits into
 * a 64 byte cache line.
 */
struct clock_read_data {//用于存储与调度时钟相关的数据
	u64 epoch_ns;//存储自系统启动以来的纳秒时间，用于表示当前时间戳
	u64 epoch_cyc;//存储上一次读取的周期数，用于与当前周期数进行比较，以计算时间差
	u64 sched_clock_mask;//用于确保在进行周期数计算时，限制值在合理范围内，以避免溢出或错误的计算。
	u64 (*read_sched_clock)(void);//指向实际的调度时钟读取函数，调用该函数可以获取当前的周期数。
	u32 mult;//在周期数到纳秒的转换中用作乘数因子，帮助进行精确的时间计算。
	u32 shift;//在周期数到纳秒的转换中用作位移因子，以调整最终的计算结果。
};

extern struct clock_read_data *sched_clock_read_begin(unsigned int *seq);
extern int sched_clock_read_retry(unsigned int seq);

extern void generic_sched_clock_init(void);

extern void sched_clock_register(u64 (*read)(void), int bits,
				 unsigned long rate);
#else
static inline void generic_sched_clock_init(void) { }

static inline void sched_clock_register(u64 (*read)(void), int bits,
					unsigned long rate)
{
}
#endif

#endif
