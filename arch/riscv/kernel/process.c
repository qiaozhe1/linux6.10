// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Sunplus Core Technology Co., Ltd.
 *  Chen Liqin <liqin.chen@sunplusct.com>
 *  Lennox Wu <lennox.wu@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2017 SiFive
 */

#include <linux/cpu.h>
#include <linux/kernel.h>
#include <linux/sched.h>
#include <linux/sched/debug.h>
#include <linux/sched/task_stack.h>
#include <linux/tick.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>

#include <asm/unistd.h>
#include <asm/processor.h>
#include <asm/csr.h>
#include <asm/stacktrace.h>
#include <asm/string.h>
#include <asm/switch_to.h>
#include <asm/thread_info.h>
#include <asm/cpuidle.h>
#include <asm/vector.h>
#include <asm/cpufeature.h>

#if defined(CONFIG_STACKPROTECTOR) && !defined(CONFIG_STACKPROTECTOR_PER_TASK)
#include <linux/stackprotector.h>
unsigned long __stack_chk_guard __read_mostly;
EXPORT_SYMBOL(__stack_chk_guard);
#endif

extern asmlinkage void ret_from_fork(void);

void noinstr arch_cpu_idle(void)
{
	cpu_do_idle();
}

int set_unalign_ctl(struct task_struct *tsk, unsigned int val)
{
	if (!unaligned_ctl_available())
		return -EINVAL;

	tsk->thread.align_ctl = val;
	return 0;
}

int get_unalign_ctl(struct task_struct *tsk, unsigned long adr)
{
	if (!unaligned_ctl_available())
		return -EINVAL;

	return put_user(tsk->thread.align_ctl, (unsigned long __user *)adr);
}

void __show_regs(struct pt_regs *regs)
{
	show_regs_print_info(KERN_DEFAULT);

	if (!user_mode(regs)) {
		pr_cont("epc : %pS\n", (void *)regs->epc);
		pr_cont(" ra : %pS\n", (void *)regs->ra);
	}

	pr_cont("epc : " REG_FMT " ra : " REG_FMT " sp : " REG_FMT "\n",
		regs->epc, regs->ra, regs->sp);
	pr_cont(" gp : " REG_FMT " tp : " REG_FMT " t0 : " REG_FMT "\n",
		regs->gp, regs->tp, regs->t0);
	pr_cont(" t1 : " REG_FMT " t2 : " REG_FMT " s0 : " REG_FMT "\n",
		regs->t1, regs->t2, regs->s0);
	pr_cont(" s1 : " REG_FMT " a0 : " REG_FMT " a1 : " REG_FMT "\n",
		regs->s1, regs->a0, regs->a1);
	pr_cont(" a2 : " REG_FMT " a3 : " REG_FMT " a4 : " REG_FMT "\n",
		regs->a2, regs->a3, regs->a4);
	pr_cont(" a5 : " REG_FMT " a6 : " REG_FMT " a7 : " REG_FMT "\n",
		regs->a5, regs->a6, regs->a7);
	pr_cont(" s2 : " REG_FMT " s3 : " REG_FMT " s4 : " REG_FMT "\n",
		regs->s2, regs->s3, regs->s4);
	pr_cont(" s5 : " REG_FMT " s6 : " REG_FMT " s7 : " REG_FMT "\n",
		regs->s5, regs->s6, regs->s7);
	pr_cont(" s8 : " REG_FMT " s9 : " REG_FMT " s10: " REG_FMT "\n",
		regs->s8, regs->s9, regs->s10);
	pr_cont(" s11: " REG_FMT " t3 : " REG_FMT " t4 : " REG_FMT "\n",
		regs->s11, regs->t3, regs->t4);
	pr_cont(" t5 : " REG_FMT " t6 : " REG_FMT "\n",
		regs->t5, regs->t6);

	pr_cont("status: " REG_FMT " badaddr: " REG_FMT " cause: " REG_FMT "\n",
		regs->status, regs->badaddr, regs->cause);
}
void show_regs(struct pt_regs *regs)
{
	__show_regs(regs);
	if (!user_mode(regs))
		dump_backtrace(regs, NULL, KERN_DEFAULT);
}

#ifdef CONFIG_COMPAT
static bool compat_mode_supported __read_mostly;

bool compat_elf_check_arch(Elf32_Ehdr *hdr)
{
	return compat_mode_supported &&
	       hdr->e_machine == EM_RISCV &&
	       hdr->e_ident[EI_CLASS] == ELFCLASS32;
}

static int __init compat_mode_detect(void)//用于检测 RISC-V 平台是否支持 32 位 ELF 兼容模式
{
	unsigned long tmp = csr_read(CSR_STATUS);//读取 CSR_STATUS 寄存器的当前值

	csr_write(CSR_STATUS, (tmp & ~SR_UXL) | SR_UXL_32);//修改寄存器值以启用 32 位兼容模式
	compat_mode_supported =
			(csr_read(CSR_STATUS) & SR_UXL) == SR_UXL_32;//检查修改是否成功以确认兼容模式支持

	csr_write(CSR_STATUS, tmp);//恢复原始的 CSR_STATUS 值

	pr_info("riscv: ELF compat mode %s",
				compat_mode_supported ? "supported" : "unsupported");//打印兼容模式支持状态

	return 0;
}
early_initcall(compat_mode_detect);
#endif

void start_thread(struct pt_regs *regs, unsigned long pc,
	unsigned long sp)
{
	regs->status = SR_PIE;
	if (has_fpu()) {
		regs->status |= SR_FS_INITIAL;
		/*
		 * Restore the initial value to the FP register
		 * before starting the user program.
		 */
		fstate_restore(current, regs);
	}
	regs->epc = pc;
	regs->sp = sp;

#ifdef CONFIG_64BIT
	regs->status &= ~SR_UXL;

	if (is_compat_task())
		regs->status |= SR_UXL_32;
	else
		regs->status |= SR_UXL_64;
#endif
}

void flush_thread(void)
{
#ifdef CONFIG_FPU
	/*
	 * Reset FPU state and context
	 *	frm: round to nearest, ties to even (IEEE default)
	 *	fflags: accrued exceptions cleared
	 */
	fstate_off(current, task_pt_regs(current));
	memset(&current->thread.fstate, 0, sizeof(current->thread.fstate));
#endif
#ifdef CONFIG_RISCV_ISA_V
	/* Reset vector state */
	riscv_v_vstate_ctrl_init(current);
	riscv_v_vstate_off(task_pt_regs(current));
	kfree(current->thread.vstate.datap);
	memset(&current->thread.vstate, 0, sizeof(struct __riscv_v_ext_state));
	clear_tsk_thread_flag(current, TIF_RISCV_V_DEFER_RESTORE);
#endif
}

void arch_release_task_struct(struct task_struct *tsk)
{
	/* Free the vector context of datap. */
	if (has_vector())
		riscv_v_thread_free(tsk);
}

int arch_dup_task_struct(struct task_struct *dst, struct task_struct *src)
{
	fstate_save(src, task_pt_regs(src));
	*dst = *src;
	/* clear entire V context, including datap for a new task */
	memset(&dst->thread.vstate, 0, sizeof(struct __riscv_v_ext_state));
	memset(&dst->thread.kernel_vstate, 0, sizeof(struct __riscv_v_ext_state));
	clear_tsk_thread_flag(dst, TIF_RISCV_V_DEFER_RESTORE);

	return 0;
}

/* 用于创建新线程的上下文，并为新线程初始化相关的寄存器状态。*/
int copy_thread(struct task_struct *p, const struct kernel_clone_args *args)
{
	unsigned long clone_flags = args->flags;//克隆标志，决定新线程的属性
	unsigned long usp = args->stack;//用户栈指针地址
	unsigned long tls = args->tls;// 线程本地存储的地址
	struct pt_regs *childregs = task_pt_regs(p);//获取新线程的 pt_regs 结构体指针

	memset(&p->thread.s, 0, sizeof(p->thread.s)); //清零线程上下文结构体的寄存器状态部分

	/* p->thread 保存上下文信息，将由 __switch_to() 恢复 */
	if (unlikely(args->fn)) { //如果传入了函数指针，则为内核线程
		/* 内核线程 */
		memset(childregs, 0, sizeof(struct pt_regs));//清空寄存器内容
		/* 设置状态寄存器，启动时开启中断 */
		childregs->status = SR_PP | SR_PIE;

		p->thread.s[0] = (unsigned long)args->fn;//内核线程的入口函数
		p->thread.s[1] = (unsigned long)args->fn_arg;//传递给函数的参数
	} else {
		*childregs = *(current_pt_regs());//复制父进程的寄存器状态
		/* 关闭状态寄存器中的矢量扩展标志 */
		riscv_v_vstate_off(childregs);
		if (usp) /* 如果用户指定了新栈指针 */
			childregs->sp = usp;//设置用户栈指针
		if (clone_flags & CLONE_SETTLS)//如果设置了TLS标志
			childregs->tp = tls;//设置线程本地存储寄存器
		childregs->a0 = 0; //fork() 调用返回值，新线程返回 0
		p->thread.s[0] = 0;//清空线程上下文的第一个寄存器
	}
	p->thread.riscv_v_flags = 0;//重置 RISC-V 矢量扩展相关标志
	if (has_vector())//如果支持矢量扩展
		riscv_v_thread_alloc(p);//为新线程分配矢量寄存器上下文
	p->thread.ra = (unsigned long)ret_from_fork;//设置返回地址为 ret_from_fork
	p->thread.sp = (unsigned long)childregs; // 设置内核栈指针为 childregs 的地址
	return 0;//返回 0 表示线程复制成功
}

void __init arch_task_cache_init(void)
{
	riscv_v_setup_ctx_cache();
}
