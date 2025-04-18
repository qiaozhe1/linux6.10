/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2022-2024 Rivos, Inc
 */

#ifndef _ASM_CPUFEATURE_H
#define _ASM_CPUFEATURE_H

#include <linux/bitmap.h>
#include <linux/jump_label.h>
#include <asm/hwcap.h>
#include <asm/alternative-macros.h>
#include <asm/errno.h>

/*
 * These are probed via a device_initcall(), via either the SBI or directly
 * from the corresponding CSRs.
 */
struct riscv_cpuinfo {
	unsigned long mvendorid;
	unsigned long marchid;
	unsigned long mimpid;
};

struct riscv_isainfo {
	DECLARE_BITMAP(isa, RISCV_ISA_EXT_MAX);
};

DECLARE_PER_CPU(struct riscv_cpuinfo, riscv_cpuinfo);

/* Per-cpu ISA extensions. */
extern struct riscv_isainfo hart_isa[NR_CPUS];

void riscv_user_isa_enable(void);

#if defined(CONFIG_RISCV_MISALIGNED)
bool check_unaligned_access_emulated_all_cpus(void);
void unaligned_emulation_finish(void);
bool unaligned_ctl_available(void);
DECLARE_PER_CPU(long, misaligned_access_speed);
#else
static inline bool unaligned_ctl_available(void)
{
	return false;
}
#endif

#if defined(CONFIG_RISCV_PROBE_UNALIGNED_ACCESS)
DECLARE_STATIC_KEY_FALSE(fast_unaligned_access_speed_key);

static __always_inline bool has_fast_unaligned_accesses(void)
{
	return static_branch_likely(&fast_unaligned_access_speed_key);
}
#else
static __always_inline bool has_fast_unaligned_accesses(void)
{
	if (IS_ENABLED(CONFIG_HAVE_EFFICIENT_UNALIGNED_ACCESS))
		return true;
	else
		return false;
}
#endif

unsigned long riscv_get_elf_hwcap(void);

struct riscv_isa_ext_data {//用于描述 RISC-V ISA 的扩展信息。这个结构体通常用于管理和查询 RISC-V 处理器支持的特定 ISA 扩展。
	const unsigned int id;//扩展的唯一标识符 (ID)，用于标识特定的 RISC-V ISA 扩展
	const char *name;//扩展的名称，通常是一个描述性的字符串
	const char *property;//与设备树中属性名对应的字符串，用于匹配设备树中定义的 ISA 扩展属性
	const unsigned int *subset_ext_ids;// 指向子集扩展 ID 的数组，如果该扩展包含子集扩展，则指向这些子集扩展的 ID 列表
	const unsigned int subset_ext_size;//子集扩展的数量，即 `subset_ext_ids` 数组中的元素个数
};

extern const struct riscv_isa_ext_data riscv_isa_ext[];
extern const size_t riscv_isa_ext_count;
extern bool riscv_isa_fallback;

unsigned long riscv_isa_extension_base(const unsigned long *isa_bitmap);

bool __riscv_isa_extension_available(const unsigned long *isa_bitmap, unsigned int bit);
#define riscv_isa_extension_available(isa_bitmap, ext)	\
	__riscv_isa_extension_available(isa_bitmap, RISCV_ISA_EXT_##ext)

static __always_inline bool
riscv_has_extension_likely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX,
			   "ext must be < RISCV_ISA_EXT_MAX");//编译时断言(这个断言可以在编译阶段捕获错误)，确保传入的扩展标识符 ext 小于 RISCV_ISA_EXT_MAX

	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE)) {//如果启用了RISC-V ALTERNATIVE功能(这个配置项启用 RISC-V 的替代特性机制（如动态指令替换)。)
		/* 使用汇编指令检测是否支持特定扩展 */
		asm goto(
		ALTERNATIVE("j	%l[l_no]", "nop", 0, %[ext], 1)
		:							//无输出操作数
		: [ext] "i" (ext)					//输入操作数：立即数 ext
		:							//无破坏的寄存器
		: l_no);						//如果不支持扩展则跳转到 l_no 标签
	} else {
		if (!__riscv_isa_extension_available(NULL, ext))//如果没有启用 ALTERNATIVE 功能，使用 __riscv_isa_extension_available 检查
			goto l_no;//如果不支持扩展则跳转到 l_no 标签
	}

	return true;//如果支持扩展则返回 true
l_no:
	return false;//如果不支持扩展则返回 false
}

static __always_inline bool
riscv_has_extension_unlikely(const unsigned long ext)
{
	compiletime_assert(ext < RISCV_ISA_EXT_MAX,
			   "ext must be < RISCV_ISA_EXT_MAX");

	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE)) {
		asm goto(
		ALTERNATIVE("nop", "j	%l[l_yes]", 0, %[ext], 1)
		:
		: [ext] "i" (ext)
		:
		: l_yes);
	} else {
		if (__riscv_isa_extension_available(NULL, ext))
			goto l_yes;
	}

	return false;
l_yes:
	return true;
}

static __always_inline bool riscv_cpu_has_extension_likely(int cpu, const unsigned long ext)
{
	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE) && riscv_has_extension_likely(ext))
		return true;

	return __riscv_isa_extension_available(hart_isa[cpu].isa, ext);
}

static __always_inline bool riscv_cpu_has_extension_unlikely(int cpu, const unsigned long ext)
{
	if (IS_ENABLED(CONFIG_RISCV_ALTERNATIVE) && riscv_has_extension_unlikely(ext))
		return true;

	return __riscv_isa_extension_available(hart_isa[cpu].isa, ext);
}

#endif
