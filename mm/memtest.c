// SPDX-License-Identifier: GPL-2.0
#include <linux/kernel.h>
#include <linux/types.h>
#include <linux/init.h>
#include <linux/memblock.h>
#include <linux/seq_file.h>

static bool early_memtest_done;
static phys_addr_t early_memtest_bad_size;

static u64 patterns[] __initdata = {
	/* The first entry has to be 0 to leave memtest with zeroed memory */
	0,
	0xffffffffffffffffULL,
	0x5555555555555555ULL,
	0xaaaaaaaaaaaaaaaaULL,
	0x1111111111111111ULL,
	0x2222222222222222ULL,
	0x4444444444444444ULL,
	0x8888888888888888ULL,
	0x3333333333333333ULL,
	0x6666666666666666ULL,
	0x9999999999999999ULL,
	0xccccccccccccccccULL,
	0x7777777777777777ULL,
	0xbbbbbbbbbbbbbbbbULL,
	0xddddddddddddddddULL,
	0xeeeeeeeeeeeeeeeeULL,
	0x7a6c7258554e494cULL, /* yeah ;-) */
};

static void __init reserve_bad_mem(u64 pattern, phys_addr_t start_bad, phys_addr_t end_bad)
{
	pr_info("  %016llx bad mem addr %pa - %pa reserved\n",
		cpu_to_be64(pattern), &start_bad, &end_bad);
	memblock_reserve(start_bad, end_bad - start_bad);
	early_memtest_bad_size += (end_bad - start_bad);
}

static void __init memtest(u64 pattern, phys_addr_t start_phys, phys_addr_t size)
{
	u64 *p, *start, *end;
	phys_addr_t start_bad, last_bad;
	phys_addr_t start_phys_aligned;
	const size_t incr = sizeof(pattern);

	start_phys_aligned = ALIGN(start_phys, incr);
	start = __va(start_phys_aligned);
	end = start + (size - (start_phys_aligned - start_phys)) / incr;
	start_bad = 0;
	last_bad = 0;

	for (p = start; p < end; p++)
		WRITE_ONCE(*p, pattern);

	for (p = start; p < end; p++, start_phys_aligned += incr) {
		if (READ_ONCE(*p) == pattern)
			continue;
		if (start_phys_aligned == last_bad + incr) {
			last_bad += incr;
			continue;
		}
		if (start_bad)
			reserve_bad_mem(pattern, start_bad, last_bad + incr);
		start_bad = last_bad = start_phys_aligned;
	}
	if (start_bad)
		reserve_bad_mem(pattern, start_bad, last_bad + incr);

	early_memtest_done = true;
}

static void __init do_one_pass(u64 pattern, phys_addr_t start, phys_addr_t end)
{
	u64 i;
	phys_addr_t this_start, this_end;

	for_each_free_mem_range(i, NUMA_NO_NODE, MEMBLOCK_NONE, &this_start,
				&this_end, NULL) {
		this_start = clamp(this_start, start, end);
		this_end = clamp(this_end, start, end);
		if (this_start < this_end) {
			pr_info("  %pa - %pa pattern %016llx\n",
				&this_start, &this_end, cpu_to_be64(pattern));
			memtest(pattern, this_start, this_end - this_start);
		}
	}
}

/* default is disabled */
static unsigned int memtest_pattern __initdata;

static int __init parse_memtest(char *arg)
{
	int ret = 0;

	if (arg)
		ret = kstrtouint(arg, 0, &memtest_pattern);
	else
		memtest_pattern = ARRAY_SIZE(patterns);

	return ret;
}

early_param("memtest", parse_memtest);
/*
 * 验证物理内存区域的可靠性和正确性。在系统启动的早期阶段，通过对指定
 * 的物理内存范围进行写入和读取操作，检测该内存区域是否存在潜在的硬
 * 件问题，如内存坏块或不稳定的内存单元
 */
void __init early_memtest(phys_addr_t start, phys_addr_t end)
{
	unsigned int i;
	unsigned int idx = 0;

	if (!memtest_pattern)// 如果没有设置内存测试模式（即 `memtest_pattern` 为 0），则直接返回。
		return;

	pr_info("early_memtest: # of tests: %u\n", memtest_pattern);//打印信息，显示要进行的测试次数
	for (i = memtest_pattern-1; i < UINT_MAX; --i) {//循环测试
		idx = i % ARRAY_SIZE(patterns);//计算要使用的模式索引，根据 `patterns` 数组的大小循环选择测试模式
		do_one_pass(patterns[idx], start, end);//对内存区域进行一次测试，使用 `patterns` 数组中的模式
	}
}

void memtest_report_meminfo(struct seq_file *m)
{
	unsigned long early_memtest_bad_size_kb;

	if (!IS_ENABLED(CONFIG_PROC_FS))
		return;

	if (!early_memtest_done)
		return;

	early_memtest_bad_size_kb = early_memtest_bad_size >> 10;
	if (early_memtest_bad_size && !early_memtest_bad_size_kb)
		early_memtest_bad_size_kb = 1;
	/* When 0 is reported, it means there actually was a successful test */
	seq_printf(m, "EarlyMemtestBad:   %5lu kB\n", early_memtest_bad_size_kb);
}
