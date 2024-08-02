// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Sunplus Core Technology Co., Ltd.
 *  Lennox Wu <lennox.wu@sunplusct.com>
 *  Chen Liqin <liqin.chen@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 */


#include <linux/mm.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/perf_event.h>
#include <linux/signal.h>
#include <linux/uaccess.h>
#include <linux/kprobes.h>
#include <linux/kfence.h>
#include <linux/entry-common.h>

#include <asm/ptrace.h>
#include <asm/tlbflush.h>

#include "../kernel/head.h"

static void die_kernel_fault(const char *msg, unsigned long addr,
		struct pt_regs *regs)
{
	bust_spinlocks(1);//允许打印紧急消息时忽略自旋锁

	pr_alert("Unable to handle kernel %s at virtual address " REG_FMT "\n", msg,
		addr);//打印警告信息，提示无法处理内核错误，并显示虚拟地址和错误信息

	bust_spinlocks(0);//恢复自旋锁的正常操作。确保在输出紧急消息后，自旋锁的行为恢复正常
	die(regs, "Oops");//输出寄存器状态和 "Oops" 信息
	make_task_dead(SIGKILL);//发送SIGKILL信号，终止当前任务。
}

static inline void no_context(struct pt_regs *regs, unsigned long addr)
{
	const char *msg;

	/* Are we prepared to handle this kernel fault? */
	if (fixup_exception(regs))//如果能修复这个内核异常，直接返回
		return;

	/*
	 * Oops. The kernel tried to access some bad page. We'll have to
	 * terminate things with extreme prejudice.
	 */
	if (addr < PAGE_SIZE)//检查地址addr是否小于页面大小PAGE_SIZE
		msg = "NULL pointer dereference";//如果是，则说明发生了NULL指针解引用错误，将错误信息设置为 "NULL pointer dereference"。
	else {
		if (kfence_handle_page_fault(addr, regs->cause == EXC_STORE_PAGE_FAULT, regs))//尝试使用 KFENCE 处理页面错误
			return;//如果 KFENCE 成功处理了页面错误，则直接返回

		msg = "paging request";//如果 KFENCE 没有处理页面错误，则将错误信息设置为 "paging request
	}

	die_kernel_fault(msg, addr, regs);//打印错误信息并终止内核
}

static inline void mm_fault_error(struct pt_regs *regs, unsigned long addr, vm_fault_t fault)
{
	struct mm_struct *mm = current->mm;
	
	if (fault & VM_FAULT_OOM) {
		if (!user_mode(regs)) {
			no_context(regs, addr);
			return;
		}
		pagefault_out_of_memory();
		return;
	} else if (fault & (VM_FAULT_SIGBUS | VM_FAULT_HWPOISON | VM_FAULT_HWPOISON_LARGE)) {
		if (!user_mode(regs)) {
			no_context(regs, addr);
			return;
		}
		do_trap(regs, SIGBUS, BUS_ADRERR, addr);
		return;
	BUG();
}

static inline void
bad_area_nosemaphore(struct pt_regs *regs, int code, unsigned long addr)
{
	/*
	 * Something tried to access memory that isn't in our memory map.
	 * Fix it, but check if it's kernel or user first.
	 */
	/* User mode accesses just cause a SIGSEGV */
	if (user_mode(regs)) {//检查是否在用户模式下运行。
		do_trap(regs, SIGSEGV, code, addr);//如果是用户模式，发送 SIGSEGV 信号，表示访问了无效内存。
		return;
	}

	no_context(regs, addr);//函数处理内核态错误。
}

static inline void
bad_area(struct pt_regs *regs, struct mm_struct *mm, int code,
	 unsigned long addr)
{
	mmap_read_unlock(mm);

	bad_area_nosemaphore(regs, code, addr);
}

static inline void vmalloc_fault(struct pt_regs *regs, int code, unsigned long addr)
{
	pgd_t *pgd, *pgd_k;
	pud_t *pud_k;
	p4d_t *p4d_k;
	pmd_t *pmd_k;
	pte_t *pte_k;
	int index;
	unsigned long pfn;

	/* User mode accesses just cause a SIGSEGV */
	if (user_mode(regs))
		return do_trap(regs, SIGSEGV, code, addr);//用户模式访问，调用该函数发送 SIGSEGV 信号并返回

	/*
	 * Synchronize this task's top level page-table
	 * with the 'reference' page table.
	 *
	 * Do _not_ use "tsk->active_mm->pgd" here.
	 * We might be inside an interrupt in the middle
	 * of a task switch.
	 */
	index = pgd_index(addr);// 获取地址对应的页全局目录索引
	pfn = csr_read(CSR_SATP) & SATP_PPN;//从 SATP 寄存器中读取页帧号
	pgd = (pgd_t *)pfn_to_virt(pfn) + index;//计算页全局目录指针
	pgd_k = init_mm.pgd + index;//获取初始页全局目录指针

	if (!pgd_present(pgdp_get(pgd_k))) {//检查页全局目录项是否存在
		no_context(regs, addr);//调用该函数处理无效上下文
		return;
	}
	set_pgd(pgd, pgdp_get(pgd_k));//设置页全局目录项

	p4d_k = p4d_offset(pgd_k, addr);//获取页4级目录指针
	if (!p4d_present(p4dp_get(p4d_k))) {//检查页4级目录项是否存在
		no_context(regs, addr);//调用该函数处理无效上下文
		return;
	}

	pud_k = pud_offset(p4d_k, addr);//获取页上层目录指针
	if (!pud_present(pudp_get(pud_k))) {//检查页上层目录项是否存在
		no_context(regs, addr);//调用该函数处理无效上下文
		return;
	}
	if (pud_leaf(pudp_get(pud_k)))//检查页上层目录项是否为叶子节点
		goto flush_tlb;//跳转到 flush_tlb 标签

	/*
	 * Since the vmalloc area is global, it is unnecessary
	 * to copy individual PTEs
	 * 由于 vmalloc 区域是全局的，因此不需要复制单个 PTE。
	 */
	pmd_k = pmd_offset(pud_k, addr);//获取页中层目录指针
	if (!pmd_present(pmdp_get(pmd_k))) {//检查页中层目录项是否存在
		no_context(regs, addr);//调用该函数处理无效上下文
		return;
	}
	if (pmd_leaf(pmdp_get(pmd_k)))//检查页中层目录项是否为叶子节点
		goto flush_tlb;//跳转到 flush_tlb 标签

	/*
	 * 确保实际的 PTE 也存在，以捕获内核 vmalloc 区域访问未映射地址的情况。
	 * 如果我们不这样做，这将只是无限循环。
	 */
	pte_k = pte_offset_kernel(pmd_k, addr);//获取页表项指针
	if (!pte_present(ptep_get(pte_k))) {//检查页表项是否存在
		no_context(regs, addr);//调用该函数处理无效上下文
		return;//
	}

	/*
	 * 内核假定 TLB 不缓存无效条目，但在 RISC-V 中，
	 * SFENCE.VMA 指定的是顺序约束，而不是缓存刷新；
	 * 即使在写入无效条目后也是必要的。
	 */
flush_tlb:
	local_flush_tlb_page(addr);//刷新本地 TLB 页面
}

static inline bool access_error(unsigned long cause, struct vm_area_struct *vma)
{
	switch (cause) {//根据页面错误的原因选择处理方式
	case EXC_INST_PAGE_FAULT://指令页面错误
		if (!(vma->vm_flags & VM_EXEC)) {//检查虚拟内存区域是否具有执行权限
			return true;//没有执行权限，返回true表示访问错误
		}
		break;
	case EXC_LOAD_PAGE_FAULT://如果是加载页面错误
		/* Write implies read */
		if (!(vma->vm_flags & (VM_READ | VM_WRITE))) {//检查虚拟内存区域是否具有读或写权限
			return true;//如果没有读或写权限，返回true表示访问错误
		}
		break;
	case EXC_STORE_PAGE_FAULT://如果是存储页面错误
		if (!(vma->vm_flags & VM_WRITE)) {//检查是否具有写权限
			return true;//没有写权限，返回true表示访问错误
		}
		break;
	default:
		panic("%s: unhandled cause %lu", __func__, cause);
	}
	return false;//返回false，表示没有访问错误
}

/*
 * This routine handles page faults.  It determines the address and the
 * problem, and then passes it off to one of the appropriate routines.
 */
void handle_page_fault(struct pt_regs *regs)
{
	struct task_struct *tsk;
	struct vm_area_struct *vma;
	struct mm_struct *mm;
	unsigned long addr, cause;
	unsigned int flags = FAULT_FLAG_DEFAULT;
	int code = SEGV_MAPERR;
	vm_fault_t fault;

	cause = regs->cause;//从寄存器中获取导致页面错误的原因
	addr = regs->badaddr;//从寄存器中获取导致页面错误的地址

	tsk = current;//获取当前任务
	mm = tsk->mm;//获取当前任务的内存描述符

	if (kprobe_page_fault(regs, cause))//检查是否为Kprobe页面错误,如果是则返回
		return;

	/*
	 * 按需处理内核空间的虚拟内存错误。参考页表是 init_mm.pgd。
	 *
	 * 注意! 我们不能在这种情况下获取任何锁。我们可能在中断或关键区域中，
	 * 只能从主页表复制信息，不做其他操作。
	 */
	if ((!IS_ENABLED(CONFIG_MMU) || !IS_ENABLED(CONFIG_64BIT)) &&
	    unlikely(addr >= VMALLOC_START && addr < VMALLOC_END)) {//检查地址是否在VMALLOC_START和VMALLOC_END之间，
		vmalloc_fault(regs, code, addr);//处理 vmalloc 区域的页面错误
		return;
	}

	/* Enable interrupts if they were enabled in the parent context. */
	if (!regs_irqs_disabled(regs))//如果在父上下文中启用了中断，则启用本地中断。
		local_irq_enable();

	/*
	 * 如果我们处于中断中，没有用户上下文，或正在运行在原子区域中，
	 * 则不能处理错误
	 */
	if (unlikely(faulthandler_disabled() || !mm)) {//如果处于在中断中或没有用户上下文
		tsk->thread.bad_cause = cause;//设置任务的错误原因
		no_context(regs, addr);
		return;
	}

	if (user_mode(regs))//如果在用户模式下运行，则设置 FAULT_FLAG_USER 标志。
		flags |= FAULT_FLAG_USER;

	if (!user_mode(regs) && addr < TASK_SIZE && unlikely(!(regs->status & SR_SUM))) {//检查是否在内核模式下且地址小于 TASK_SIZE
		if (fixup_exception(regs))//尝试修复异常
			return;//如果修复成功，返回

		die_kernel_fault("access to user memory without uaccess routines", addr, regs);//输出错误日志
	}

	perf_sw_event(PERF_COUNT_SW_PAGE_FAULTS, 1, regs, addr);

	if (cause == EXC_STORE_PAGE_FAULT)//检查是否为存储页面错误
		flags |= FAULT_FLAG_WRITE;//如果是，设置 FAULT_FLAG_WRITE 标志
	else if (cause == EXC_INST_PAGE_FAULT)//检查是否为指令页面错误
		flags |= FAULT_FLAG_INSTRUCTION;//如果是，设置 FAULT_FLAG_INSTRUCTION 标志
	if (!(flags & FAULT_FLAG_USER))//检查是否在用户模式下
		goto lock_mmap;//如果不是，跳转到 lock_mmap 标签

	vma = lock_vma_under_rcu(mm, addr);//查找并锁定虚拟内存区域
	if (!vma)//检查是否成功锁定
		goto lock_mmap;//如果失败，跳转到 lock_mmap 标签

	if (unlikely(access_error(cause, vma))) {//检查是否有页面访问错误
		vma_end_read(vma);//结束虚拟内存区域读取
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);//
		tsk->thread.bad_cause = cause;//设置任务的错误原因
		bad_area_nosemaphore(regs, SEGV_ACCERR, addr);//调用 bad_area_nosemaphore 处理函数
		return;
	}

	fault = handle_mm_fault(vma, addr, flags | FAULT_FLAG_VMA_LOCK, regs);//处理内存缺页
	if (!(fault & (VM_FAULT_RETRY | VM_FAULT_COMPLETED)))//检查是否需要重试或已完成
		vma_end_read(vma);//如果不需要，结束虚拟内存区域读取

	if (!(fault & VM_FAULT_RETRY)) {//检查是否需要重试
		count_vm_vma_lock_event(VMA_LOCK_SUCCESS);//记录虚拟内存区域锁定事件
		goto done;
	}
	count_vm_vma_lock_event(VMA_LOCK_RETRY);//记录虚拟内存区域锁定重试事件
	if (fault & VM_FAULT_MAJOR)//检查是否为主要故障
		flags |= FAULT_FLAG_TRIED;//如果是，设置 FAULT_FLAG_TRIED 标志

	if (fault_signal_pending(fault, regs)) {//检查是否有挂起的信号
		if (!user_mode(regs))//检查是否在用户模式下
			no_context(regs, addr);//如果不是，调用 no_context 处理函数
		return;
	}
lock_mmap:

retry:
	vma = lock_mm_and_find_vma(mm, addr, regs);//锁定内存管理并查找虚拟内存区域
	if (unlikely(!vma)) {//检查是否成功锁定和查找
		tsk->thread.bad_cause = cause;//设置任务的错误原因
		bad_area_nosemaphore(regs, code, addr);
		return;
	}

	/*
	 * Ok, we have a good vm_area for this memory access, so
	 * we can handle it.
	 * 我们有一个适合此内存访问的 vm_area，因此可以处理它。
	 */
	code = SEGV_ACCERR;//设置错误代码

	if (unlikely(access_error(cause, vma))) {//检查是否有访问错误
		tsk->thread.bad_cause = cause;//设置任务的错误原因
		bad_area(regs, mm, code, addr);
		return;
	}

	/*
	 * 如果我们无法处理页面错误，请确保能够退出，而不是无限重试。
	 */
	fault = handle_mm_fault(vma, addr, flags, regs);//处理内存管理页错误

	/*
	 * 如果需要重试但有致命信号挂起，则先处理信号。
	 * 我们不需要释放 mmap_lock 因为它会在 mm/filemap.c 中的 __lock_page_or_retry 中释放。
	 */
	if (fault_signal_pending(fault, regs)) {//检查是否有挂起的信号
		if (!user_mode(regs))//检查是否在用户模式下
			no_context(regs, addr);// 如果不是，调用 no_context 处理函数
		return;
	}

	/*  页面错误已完全处理，包括释放 mmap 锁 */
	if (fault & VM_FAULT_COMPLETED)//检查页面错误是否已完成
		return;

	if (unlikely(fault & VM_FAULT_RETRY)) {//检查是否需要重试
		flags |= FAULT_FLAG_TRIED;//设置 FAULT_FLAG_TRIED 标志

		/*
		 * 无需调用 mmap_read_unlock(mm) 因为它已经在 mm/filemap.c 中的
		 * __lock_page_or_retry 中释放了。
		 */
		goto retry;
	}

	mmap_read_unlock(mm);//释放内存管理读锁

done:
	if (unlikely(fault & VM_FAULT_ERROR)) {//检查是否有页面错误
		tsk->thread.bad_cause = cause;//设置任务的错误原因
		mm_fault_error(regs, addr, fault);//调用 mm_fault_error 处理函数
		return;
	}
	return;
}
