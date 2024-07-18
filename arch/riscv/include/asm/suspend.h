/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (c) 2021 Western Digital Corporation or its affiliates.
 * Copyright (c) 2022 Ventana Micro Systems Inc.
 */

#ifndef _ASM_RISCV_SUSPEND_H
#define _ASM_RISCV_SUSPEND_H

#include <asm/ptrace.h>

struct suspend_context {//用于保存系统进入休眠状态时的上下文信息
	/* Saved and restored by low-level functions */
	struct pt_regs regs;//保存 CPU 寄存器状态的结构体 pt_regs，用于在系统休眠和恢复时保存和恢复寄存器的状态。此成员由低级函数保存和恢复。
	/* Saved and restored by high-level functions */
	unsigned long envcfg;//保存环境配置的变量 envcfg，用于在系统休眠和恢复时保存和恢复环境配置。此成员由高级函数保存和恢复。
	unsigned long tvec;//保存中断向量的变量 tvec，用于在系统休眠和恢复时保存和恢复中断向量。
	unsigned long ie;//保存中断使能寄存器的变量 ie，用于在系统休眠和恢复时保存和恢复中断使能状态。
#ifdef CONFIG_MMU
	unsigned long satp;//保存地址空间标识符寄存器的变量 satp，用于在系统休眠和恢复时保存和恢复地址空间标识符。仅在启用 MMU 时包含此成员。
#endif
};

/*
 * Used by hibernation core and cleared during resume sequence
 */
extern int in_suspend;

/* Low-level CPU suspend entry function */
int __cpu_suspend_enter(struct suspend_context *context);

/* High-level CPU suspend which will save context and call finish() */
int cpu_suspend(unsigned long arg,
		int (*finish)(unsigned long arg,
			      unsigned long entry,
			      unsigned long context));

/* Low-level CPU resume entry function */
int __cpu_resume_enter(unsigned long hartid, unsigned long context);

/* Used to save and restore the CSRs */
void suspend_save_csrs(struct suspend_context *context);
void suspend_restore_csrs(struct suspend_context *context);

/* Low-level API to support hibernation */
int swsusp_arch_suspend(void);
int swsusp_arch_resume(void);
int arch_hibernation_header_save(void *addr, unsigned int max_size);
int arch_hibernation_header_restore(void *addr);
int __hibernate_cpu_resume(void);

/* Used to resume on the CPU we hibernated on */
int hibernate_resume_nonboot_cpu_disable(void);

asmlinkage void hibernate_restore_image(unsigned long resume_satp, unsigned long satp_temp,
					unsigned long cpu_resume);
asmlinkage int hibernate_core_restore_code(void);
bool riscv_sbi_hsm_is_supported(void);
bool riscv_sbi_suspend_state_is_valid(u32 state);
int riscv_sbi_hart_suspend(u32 state);
#endif
