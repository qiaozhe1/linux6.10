// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) 2020 Western Digital Corporation or its affiliates.
 */

#include <linux/errno.h>
#include <linux/mm.h>
#include <linux/of.h>
#include <linux/string.h>
#include <linux/sched.h>
#include <asm/cpu_ops.h>
#include <asm/cpu_ops_sbi.h>
#include <asm/sbi.h>
#include <asm/smp.h>

const struct cpu_operations *cpu_ops __ro_after_init = &cpu_ops_spinwait;

extern const struct cpu_operations cpu_ops_sbi;
#ifndef CONFIG_RISCV_BOOT_SPINWAIT
const struct cpu_operations cpu_ops_spinwait = {
	.cpu_start	= NULL,
};
#endif

void __init cpu_set_ops(void)//用于设置 CPU 操作的实现方式
{
#if IS_ENABLED(CONFIG_RISCV_SBI)//如果启用了 CONFIG_RISCV_SBI 配置项
	if (sbi_probe_extension(SBI_EXT_HSM)) {//如果检测到 SBI（Supervisor Binary Interface）中的 HSM（Hart State Management）扩展
		pr_info("SBI HSM extension detected\n");//打印信息表示检测到了 SBI 的 HSM 扩展
		cpu_ops = &cpu_ops_sbi;//将全局变量 cpu_ops 指向 cpu_ops_sbi，这意味着使用 SBI 提供的操作集来管理 CPU
	}
#endif
}
