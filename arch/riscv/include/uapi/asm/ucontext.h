/* SPDX-License-Identifier: GPL-2.0-only WITH Linux-syscall-note */
/*
 * Copyright (C) 2012 ARM Ltd.
 * Copyright (C) 2017 SiFive, Inc.
 *
 * This file was copied from arch/arm64/include/uapi/asm/ucontext.h
 */
#ifndef _UAPI_ASM_RISCV_UCONTEXT_H
#define _UAPI_ASM_RISCV_UCONTEXT_H

#include <linux/types.h>
/*
 * 用于保存程序在某个时刻的执行上下文，以便在信号处理程序中或在线程切换时能够保存和恢复程序状态。
 * 这个结构体在信号处理程序中尤为重要，它使得处理程序能够在执行完成后正确恢复到之前的执行状态。
 */
struct ucontext {
	unsigned long	  uc_flags;//上下文标志，用于指示上下文信息的状态或属性
	struct ucontext	 *uc_link;// 指向下一个 ucontext 结构的指针，表示信号处理完成后要恢复的上下文
	stack_t		  uc_stack;//保存信号处理程序使用的栈信息，包括栈指针、栈大小等
	sigset_t	  uc_sigmask;//信号屏蔽字，表示在信号处理程序中被阻塞的信号集合
	/*
	 * There's some padding here to allow sigset_t to be expanded in the
	 * future.  Though this is unlikely, other architectures put uc_sigmask
	 * at the end of this structure and explicitly state it can be
	 * expanded, so we didn't want to box ourselves in here.
	 */
	__u8		  __unused[1024 / 8 - sizeof(sigset_t)];//填充字段，用于在未来扩展 sigset_t 的大小。
	/*
	 * We can't put uc_sigmask at the end of this structure because we need
	 * to be able to expand sigcontext in the future.  For example, the
	 * vector ISA extension will almost certainly add ISA state.  We want
	 * to ensure all user-visible ISA state can be saved and restored via a
	 * ucontext, so we're putting this at the end in order to allow for
	 * infinite extensibility.  Since we know this will be extended and we
	 * assume sigset_t won't be extended an extreme amount, we're
	 * prioritizing this.
	 */
	struct sigcontext uc_mcontext;//保存了信号处理程序的寄存器上下文，包括处理器状态、寄存器值等。
};

#endif /* _UAPI_ASM_RISCV_UCONTEXT_H */
