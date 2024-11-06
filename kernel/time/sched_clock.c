// SPDX-License-Identifier: GPL-2.0
/*
 * Generic sched_clock() support, to extend low level hardware time
 * counters to full 64-bit ns values.
 */
#include <linux/clocksource.h>
#include <linux/init.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/kernel.h>
#include <linux/math.h>
#include <linux/moduleparam.h>
#include <linux/sched.h>
#include <linux/sched/clock.h>
#include <linux/syscore_ops.h>
#include <linux/hrtimer.h>
#include <linux/sched_clock.h>
#include <linux/seqlock.h>
#include <linux/bitops.h>

#include "timekeeping.h"

/**
 * struct clock_data - all data needed for sched_clock() (including
 *                     registration of a new clock source)
 *
 * @seq:		Sequence counter for protecting updates. The lowest
 *			bit is the index for @read_data.
 * @read_data:		Data required to read from sched_clock.
 * @wrap_kt:		Duration for which clock can run before wrapping.
 * @rate:		Tick rate of the registered clock.
 * @actual_read_sched_clock: Registered hardware level clock read function.
 *
 * The ordering of this structure has been chosen to optimize cache
 * performance. In particular 'seq' and 'read_data[0]' (combined) should fit
 * into a single 64-byte cache line.
 */
struct clock_data {//用于存储与调度时钟相关的多个数据项
	seqcount_latch_t	seq;//用于保护对 read_data 的并发访问，确保在读取时钟数据时的一致性和有效性。
	struct clock_read_data	read_data[2];//实现双缓冲机制，允许在更新时钟数据时，旧数据可以继续被读取，从而避免数据冲突和不一致。
	ktime_t			wrap_kt;//当时钟计数达到其最大值后回绕时，记录此时的时间，以便于正确计算时间差。
	unsigned long		rate;//存储时钟频率，通常用于确定调度时钟的更新速率。

	u64 (*actual_read_sched_clock)(void);//指向实际的调度时钟读取函数，允许调用者获取当前的调度周期数。
};

static struct hrtimer sched_clock_timer;
static int irqtime = -1;

core_param(irqtime, irqtime, int, 0400);

static u64 notrace jiffy_sched_clock_read(void)
{
	/*
	 * We don't need to use get_jiffies_64 on 32-bit arches here
	 * because we register with BITS_PER_LONG
	 */
	return (u64)(jiffies - INITIAL_JIFFIES);
}

static struct clock_data cd ____cacheline_aligned = {
	.read_data[0] = { .mult = NSEC_PER_SEC / HZ,
			  .read_sched_clock = jiffy_sched_clock_read, },
	.actual_read_sched_clock = jiffy_sched_clock_read,
};

static __always_inline u64 cyc_to_ns(u64 cyc, u32 mult, u32 shift)
{
	return (cyc * mult) >> shift;
}

notrace struct clock_read_data *sched_clock_read_begin(unsigned int *seq)
{
	*seq = raw_read_seqcount_latch(&cd.seq);
	return cd.read_data + (*seq & 1);
}

notrace int sched_clock_read_retry(unsigned int seq)
{
	return raw_read_seqcount_latch_retry(&cd.seq, seq);
}

unsigned long long noinstr sched_clock_noinstr(void)
{
	struct clock_read_data *rd;
	unsigned int seq;
	u64 cyc, res;

	do {
		seq = raw_read_seqcount_latch(&cd.seq);
		rd = cd.read_data + (seq & 1);

		cyc = (rd->read_sched_clock() - rd->epoch_cyc) &
		      rd->sched_clock_mask;
		res = rd->epoch_ns + cyc_to_ns(cyc, rd->mult, rd->shift);
	} while (raw_read_seqcount_latch_retry(&cd.seq, seq));

	return res;
}

unsigned long long notrace sched_clock(void)
{
	unsigned long long ns;
	preempt_disable_notrace();
	ns = sched_clock_noinstr();
	preempt_enable_notrace();
	return ns;
}

/*
 * Updating the data required to read the clock.
 *
 * sched_clock() will never observe mis-matched data even if called from
 * an NMI. We do this by maintaining an odd/even copy of the data and
 * steering sched_clock() to one or the other using a sequence counter.
 * In order to preserve the data cache profile of sched_clock() as much
 * as possible the system reverts back to the even copy when the update
 * completes; the odd copy is used *only* during an update.
 */
static void update_clock_read_data(struct clock_read_data *rd)
{
	/* update the backup (odd) copy with the new data */
	cd.read_data[1] = *rd;

	/* steer readers towards the odd copy */
	raw_write_seqcount_latch(&cd.seq);

	/* now its safe for us to update the normal (even) copy */
	cd.read_data[0] = *rd;

	/* switch readers back to the even copy */
	raw_write_seqcount_latch(&cd.seq);
}

/*
 * Atomically update the sched_clock() epoch.
 * 用于更新系统的调度时钟，计算并存储新的时间戳（以纳秒为单位），
 */
static void update_sched_clock(void)
{
	u64 cyc;//用于存储当前的周期数
	u64 ns;//用于存储转换后的纳秒值
	struct clock_read_data rd;//用于读取时钟数据

	rd = cd.read_data[0];// 从调度时钟数据数组中获取当前的时钟读取数据

	cyc = cd.actual_read_sched_clock();//调用实际的调度时钟读取函数，获取当前的周期数

       /*
	* 将当前周期数转换为纳秒，并加上上一次的 epoch_ns，
	* 以得到更新后的纳秒值。
	* 具体步骤为：
	* 1. 计算周期数的变化量 (cyc - rd.epoch_cyc)，
	* 2. 通过与 sched_clock_mask 进行按位与操作确保在合理范围内，
	* 3. 使用 mult 和 shift 进行转换得到对应的纳秒数。
	*/				 
	ns = rd.epoch_ns + cyc_to_ns((cyc - rd.epoch_cyc) & rd.sched_clock_mask, rd.mult, rd.shift);//计算新的纳秒值

	rd.epoch_ns = ns;//更新 epoch_ns 为新的纳秒值
	rd.epoch_cyc = cyc;// 更新 epoch_cyc 为当前周期数

	update_clock_read_data(&rd);//更新调度时钟的读取数据
}

static enum hrtimer_restart sched_clock_poll(struct hrtimer *hrt)
{
	update_sched_clock();
	hrtimer_forward_now(hrt, cd.wrap_kt);

	return HRTIMER_RESTART;
}

void __init
sched_clock_register(u64 (*read)(void), int bits, unsigned long rate)
{
	u64 res, wrap, new_mask, new_epoch, cyc, ns;
	u32 new_mult, new_shift;
	unsigned long r, flags;
	char r_unit;
	struct clock_read_data rd;

	if (cd.rate > rate)
		return;

	/* Cannot register a sched_clock with interrupts on */
	local_irq_save(flags);

	/* Calculate the mult/shift to convert counter ticks to ns. */
	clocks_calc_mult_shift(&new_mult, &new_shift, rate, NSEC_PER_SEC, 3600);

	new_mask = CLOCKSOURCE_MASK(bits);
	cd.rate = rate;

	/* Calculate how many nanosecs until we risk wrapping */
	wrap = clocks_calc_max_nsecs(new_mult, new_shift, 0, new_mask, NULL);
	cd.wrap_kt = ns_to_ktime(wrap);

	rd = cd.read_data[0];

	/* Update epoch for new counter and update 'epoch_ns' from old counter*/
	new_epoch = read();
	cyc = cd.actual_read_sched_clock();
	ns = rd.epoch_ns + cyc_to_ns((cyc - rd.epoch_cyc) & rd.sched_clock_mask, rd.mult, rd.shift);
	cd.actual_read_sched_clock = read;

	rd.read_sched_clock	= read;
	rd.sched_clock_mask	= new_mask;
	rd.mult			= new_mult;
	rd.shift		= new_shift;
	rd.epoch_cyc		= new_epoch;
	rd.epoch_ns		= ns;

	update_clock_read_data(&rd);

	if (sched_clock_timer.function != NULL) {
		/* update timeout for clock wrap */
		hrtimer_start(&sched_clock_timer, cd.wrap_kt,
			      HRTIMER_MODE_REL_HARD);
	}

	r = rate;
	if (r >= 4000000) {
		r = DIV_ROUND_CLOSEST(r, 1000000);
		r_unit = 'M';
	} else if (r >= 4000) {
		r = DIV_ROUND_CLOSEST(r, 1000);
		r_unit = 'k';
	} else {
		r_unit = ' ';
	}

	/* Calculate the ns resolution of this counter */
	res = cyc_to_ns(1ULL, new_mult, new_shift);

	pr_info("sched_clock: %u bits at %lu%cHz, resolution %lluns, wraps every %lluns\n",
		bits, r, r_unit, res, wrap);

	/* Enable IRQ time accounting if we have a fast enough sched_clock() */
	if (irqtime > 0 || (irqtime == -1 && rate >= 1000000))
		enable_sched_clock_irqtime();

	local_irq_restore(flags);

	pr_debug("Registered %pS as sched_clock source\n", read);
}
/* 用于初始化调度时钟，确保系统中调度时钟的正确性和及时更新 */
void __init generic_sched_clock_init(void)
{
	/*
	 * If no sched_clock() function has been provided at that point,
	 * make it the final one.
	 */
	if (cd.actual_read_sched_clock == jiffy_sched_clock_read)//检查当前的调度时钟读取函数是否为 jiffy_sched_clock_read。如果是，表示尚未注册其他调度时钟。
		sched_clock_register(jiffy_sched_clock_read, BITS_PER_LONG, HZ);//注册jiffy_sched_clock_read函数为系统的调度时钟，指定其位数和时钟频率（HZ）。

	update_sched_clock();//更新调度时钟的状态，确保调度时钟的信息是最新的。

	/*
	 * Start the timer to keep sched_clock() properly updated and
	 * sets the initial epoch.
	 */
	hrtimer_init(&sched_clock_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL_HARD);//初始化一个高分辨率定时器，使用单调时钟（CLOCK_MONOTONIC）和相对硬实时模式
	sched_clock_timer.function = sched_clock_poll;//将定时器的回调函数设置为 sched_clock_poll，该函数将在定时器到期时被调用，负责更新调度时钟。
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL_HARD);//启动定时器，指定定时器的初始延迟（cd.wrap_kt），以相对硬实时模式启动。
}

/*
 * Clock read function for use when the clock is suspended.
 *
 * This function makes it appear to sched_clock() as if the clock
 * stopped counting at its last update.
 *
 * This function must only be called from the critical
 * section in sched_clock(). It relies on the read_seqcount_retry()
 * at the end of the critical section to be sure we observe the
 * correct copy of 'epoch_cyc'.
 */
static u64 notrace suspended_sched_clock_read(void)
{
	unsigned int seq = raw_read_seqcount_latch(&cd.seq);

	return cd.read_data[seq & 1].epoch_cyc;
}

int sched_clock_suspend(void)
{
	struct clock_read_data *rd = &cd.read_data[0];

	update_sched_clock();
	hrtimer_cancel(&sched_clock_timer);
	rd->read_sched_clock = suspended_sched_clock_read;

	return 0;
}

void sched_clock_resume(void)
{
	struct clock_read_data *rd = &cd.read_data[0];

	rd->epoch_cyc = cd.actual_read_sched_clock();
	hrtimer_start(&sched_clock_timer, cd.wrap_kt, HRTIMER_MODE_REL_HARD);
	rd->read_sched_clock = cd.actual_read_sched_clock;
}

static struct syscore_ops sched_clock_ops = {
	.suspend	= sched_clock_suspend,
	.resume		= sched_clock_resume,
};

static int __init sched_clock_syscore_init(void)
{
	register_syscore_ops(&sched_clock_ops);

	return 0;
}
device_initcall(sched_clock_syscore_init);
