/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright (C) 2012 Regents of the University of California
 */

#include <asm/asm-offsets.h>
#include <asm/asm.h>
#include <linux/init.h>
#include <linux/linkage.h>
#include <asm/thread_info.h>
#include <asm/page.h>
#include <asm/pgtable.h>
#include <asm/csr.h>
#include <asm/hwcap.h>
#include <asm/image.h>
#include <asm/scs.h>
#include <asm/xip_fixup.h>
#include "efi-header.S"

__HEAD
SYM_CODE_START(_start)
	/*
	 * Image header expected by Linux boot-loaders. The image header data
	 * structure is described in asm/image.h.
	 * Do not modify it without modifying the structure and all bootloaders
	 * that expects this header format!!
	 */
#ifdef CONFIG_EFI // 如果配置了 EFI
	/*
	 * This instruction decodes to "MZ" ASCII required by UEFI.
	 */
	c.li s4,-13 //将立即数 -13 装载到寄存器 s4
	j _start_kernel //跳转到 _start_kernel 函数
#else // 如果没有配置 EFI
	/* jump to start kernel */
	j _start_kernel //跳转到 _start_kernel 函数
	/* reserved */
	.word 0 //填充一个字节，值为0
#endif
	.balign 8 // 8 字节对齐
#ifdef CONFIG_RISCV_M_MODE //如果配置了 RISC-V M 模式
	/* Image load offset (0MB) from start of RAM for M-mode */
	.dword 0 //以 0 表示加载偏移量
#else //如果没有配置 M 模式
#if __riscv_xlen == 64 //如果 RISC-V 寄存器宽度是 64 位
	/* Image load offset(2MB) from start of RAM */
	.dword 0x200000 //2MB 加载偏移量
#else //如果寄存器宽度不是 64 位
	/* Image load offset(4MB) from start of RAM */
	.dword 0x400000 //4MB 加载偏移量
#endif 
#endif
	/* Effective size of kernel image */
	.dword _end - _start //内核镜像的有效大小
	.dword __HEAD_FLAGS //头部标志
	.word RISCV_HEADER_VERSION // RISC-V 头部版本号
	.word 0 //填充一个字，值为0
	.dword 0 //填充一个双字，值为0
	.ascii RISCV_IMAGE_MAGIC //RISC-V 镜像魔数
	.balign 4 //4 字节对齐
	.ascii RISCV_IMAGE_MAGIC2 //RISC-V 镜像魔数2
#ifdef CONFIG_EFI //如果配置了 EFI
	.word pe_head_start - _start //PE 头部的起始位置与 _start 的偏移
pe_head_start: // PE 头部起始位置标签

	__EFI_PE_HEADER //EFI PE 头部定义
#else // 如果没有配置 EFI
	.word 0 //填充一个字，值为0
#endif

.align 2
#ifdef CONFIG_MMU //如果配置了 MMU
	.global relocate_enable_mmu //声明全局符号
relocate_enable_mmu: //relocate_enable_mmu 函数标签
	/* Relocate return address */
	la a1, kernel_map //加载 kernel_map 地址到寄存器 a1
	XIP_FIXUP_OFFSET a1 // 修正 XIP 偏移
	REG_L a1, KERNEL_MAP_VIRT_ADDR(a1) // 从虚拟地址加载值到 a1
	la a2, _start //加载 _start 地址到寄存器 a2
	sub a1, a1, a2 //计算返回地址偏移
	add ra, ra, a1 //更新返回地址寄存器 ra

	/* Point stvec to virtual address of intruction after satp write */
	la a2, 1f // 加载下一指令地址到寄存器 a2
	add a2, a2, a1 // 更新 a2 为新虚拟地址
	csrw CSR_TVEC, a2 // 设置 CSR_TVEC 寄存器为新虚拟地址

	/* Compute satp for kernel page tables, but don't load it yet */
	srl a2, a0, PAGE_SHIFT //将 a0 右移 PAGE_SHIFT 存入 a2
	la a1, satp_mode // 加载 satp_mode 地址到 a1
	XIP_FIXUP_OFFSET a1 //修正 XIP 偏移
	REG_L a1, 0(a1) //从 a1 地址加载值到 a1
	or a2, a2, a1 //合并 satp 模式和页表信息

	/*
	 * Load trampoline page directory, which will cause us to trap to
	 * stvec if VA != PA, or simply fall through if VA == PA.  We need a
	 * full fence here because setup_vm() just wrote these PTEs and we need
	 * to ensure the new translations are in use.
	 */
	la a0, trampoline_pg_dir // 加载 trampoline_pg_dir 地址到 a0
	XIP_FIXUP_OFFSET a0 // 修正 XIP 偏移
	srl a0, a0, PAGE_SHIFT //将 a0 右移 PAGE_SHIFT 存入 a0
	or a0, a0, a1 //合并 a0 和 satp 信息
	sfence.vma //全局刷新页表
	csrw CSR_SATP, a0 //设置 CSR_SATP 寄存器为 a0
.align 2
1:
	/* Set trap vector to spin forever to help debug */
	la a0, .Lsecondary_park //加载 .Lsecondary_park 地址到 a0
	csrw CSR_TVEC, a0 // 设置 CSR_TVEC 寄存器为 a0

	/* Reload the global pointer */
	load_global_pointer //加载全局指针

	/*
	 * Switch to kernel page tables.  A full fence is necessary in order to
	 * avoid using the trampoline translations, which are only correct for
	 * the first superpage.  Fetching the fence is guaranteed to work
	 * because that first superpage is translated the same way.
	 */
	csrw CSR_SATP, a2 //设置 CSR_SATP 寄存器为 a2
	sfence.vma //全局刷新页表

	ret //返回
#endif /* CONFIG_MMU */
#ifdef CONFIG_SMP //如果配置了 SMP
	.global secondary_start_sbi //声明全局符号
secondary_start_sbi: //secondary_start_sbi 函数标签
	/* Mask all interrupts */
	csrw CSR_IE, zero //关闭中断
	csrw CSR_IP, zero //清空中断状态

	/* Load the global pointer */
	load_global_pointer //加载全局指针

	/*
	 * Disable FPU & VECTOR to detect illegal usage of
	 * floating point or vector in kernel space
	 */
	li t0, SR_FS_VS // 加载 FPU 状态值到 t0
	csrc CSR_STATUS, t0 // 清除状态寄存器中的 FPU 状态

	/* Set trap vector to spin forever to help debug */
	la a3, .Lsecondary_park //加载 .Lsecondary_park 地址到 a3
	csrw CSR_TVEC, a3 // 设置 CSR_TVEC 寄存器为 a3

	/* a0 contains the hartid & a1 contains boot data */
	li a2, SBI_HART_BOOT_TASK_PTR_OFFSET // 加载 HART 启动任务指针偏移到 a2
	XIP_FIXUP_OFFSET a2 //修正 XIP 偏移
	add a2, a2, a1 //计算任务指针地址
	REG_L tp, (a2) //从地址加载任务指针到 tp
	li a3, SBI_HART_BOOT_STACK_PTR_OFFSET //加载 HART 启动栈指针偏移到 a3
	XIP_FIXUP_OFFSET a3 //修正XIP偏移
	add a3, a3, a1 //计算栈指针地址
	REG_L sp, (a3) //从地址加载栈指针到 sp

.Lsecondary_start_common: //secondary_start_common 标签

#ifdef CONFIG_MMU //如果配置了MMU
	/* Enable virtual memory and relocate to virtual address */
	la a0, swapper_pg_dir // 加载 swapper_pg_dir 地址到 a0
	XIP_FIXUP_OFFSET a0 // 修正 XIP 偏移
	call relocate_enable_mmu //调用 relocate_enable_mmu 函数
#endif
	call .Lsetup_trap_vector //调用设置陷阱向量的函数
	scs_load_current // 加载当前状态
	tail smp_callin //调用 SMP 启动
#endif /* CONFIG_SMP */

.align 2
.Lsetup_trap_vector: //设置陷阱向量的标签
	/* Set trap vector to exception handler */
	la a0, handle_exception //加载异常处理函数地址到 a0
	csrw CSR_TVEC, a0 //设置 CSR_TVEC 寄存器为 a0

	/*
	 * Set sup0 scratch register to 0, indicating to exception vector that
	 * we are presently executing in kernel.
	 */
	csrw CSR_SCRATCH, zero //将 CSR_SCRATCH 寄存器清零
	ret //返回

.align 2
.Lsecondary_park: 
	/* We lack SMP support or have too many harts, so park this hart */
	wfi // 等待中断
	j .Lsecondary_park //循环跳转到 secondary_park

SYM_CODE_END(_start)

SYM_CODE_START(_start_kernel)
	/* Mask all interrupts */
	csrw CSR_IE, zero //将中断使能寄存器设置为0，禁用中断
	csrw CSR_IP, zero //清空中断挂起寄存器	

#ifdef CONFIG_RISCV_M_MODE //如果配置了 RISC-V M 模式
	/* flush the instruction cache */
	fence.i //执行栅栏操作，确保指令缓存被刷新

	/* Reset all registers except ra, a0, a1 */
	call reset_regs //调用 reset_regs 函数重置寄存器

	/*
	 * Setup a PMP to permit access to all of memory.  Some machines may
	 * not implement PMPs, so we set up a quick trap handler to just skip
	 * touching the PMPs on any trap.
	 */
	la a0, .Lpmp_done //加载 pmp_done 标签地址到寄存器 a0
	csrw CSR_TVEC, a0 //设置陷阱向量寄存器，指向 pmp_done

	li a0, -1 //将立即数-1载入寄存器 a0
	csrw CSR_PMPADDR0, a0; //设置 PMP 地址寄存器为 -1，允许访问所有内存
	li a0, (PMP_A_NAPOT | PMP_R | PMP_W | PMP_X) //设置PMP配置为NAPOT（Not a Power of Two）读、写、执行权限
	csrw CSR_PMPCFG0, a0 //写入PMP配置寄存器
.align 2 //对齐到2字节
.Lpmp_done:

	/*
	 * The hartid in a0 is expected later on, and we have no firmware
	 * to hand it to us.
	 */
	csrr a0, CSR_MHARTID //读取hart ID到a0寄存器
#endif /* CONFIG_RISCV_M_MODE */ //结束条件编译

	/* Load the global pointer */
	load_global_pointer //调用加载全局指针函数

	/*
	 * Disable FPU & VECTOR to detect illegal usage of
	 * floating point or vector in kernel space
	 */
	li t0, SR_FS_VS //将 FPU 状态值加载到 t0
	csrc CSR_STATUS, t0 //清除状态寄存器中的FPU状态

#ifdef CONFIG_RISCV_BOOT_SPINWAIT //如果配置了 RISC-V 启动自旋等待
	li t0, CONFIG_NR_CPUS //将CPU数量加载到t0
	blt a0, t0, .Lgood_cores //如果a0（当前 hart ID）小于t0，跳转到Lgood_cores
	tail .Lsecondary_park //否则，进入二级停车状态
.Lgood_cores://good_cores标签

	/* The lottery system is only required for spinwait booting method */
#ifndef CONFIG_XIP_KERNEL //如果没有配置XIP内核
	/* Pick one hart to run the main boot sequence */
	la a3, hart_lottery //加载 hart_lottery 地址到 a3
	li a2, 1 //将 1 加载到 a2
	amoadd.w a3, a2, (a3) //原子加法，将a2加入到 hart_lottery
	bnez a3, .Lsecondary_start //如果a3不为零，跳转到 Lsecondary_start

#else //如果配置了XIP内核
	/* hart_lottery in flash contains a magic number */
	la a3, hart_lottery //加载 hart_lottery 地址到 a3
	mv a2, a3 //将 a3 移动到 a2
	XIP_FIXUP_OFFSET a2 // 修正 a2 地址的 XIP 偏移
	XIP_FIXUP_FLASH_OFFSET a3 // 修正 a3 地址的 XIP 偏移
	lw t1, (a3) //从 a3 地址加载值到 t1
	amoswap.w t0, t1, (a2) //原子交换，将 t1 的值与 a2 的值进行交换
	/* first time here if hart_lottery in RAM is not set */
	beq t0, t1, .Lsecondary_start //如果 t0 等于 t1，跳转到 Lsecondary_start

#endif /* CONFIG_XIP */
#endif /* CONFIG_RISCV_BOOT_SPINWAIT */

#ifdef CONFIG_XIP_KERNEL //如果配置了 XIP 内核
	la sp, _end + THREAD_SIZE //设置栈指针为 _end + 线程大小
	XIP_FIXUP_OFFSET sp //修正栈指针地址的 XIP 偏移
	mv s0, a0 //保存 a0 到 s0
	mv s1, a1 //保存 a1 到 s1
	call __copy_data //调用数据复制函数

	/* Restore a0 & a1 copy */
	mv a0, s0 // 恢复a0
	mv a1, s1 // 恢复a1
#endif

#ifndef CONFIG_XIP_KERNEL //如果没有配置 XIP 内核
	/* Clear BSS for flat non-ELF images */
	la a3, __bss_start //加载 BSS 段开始地址到 a3
	la a4, __bss_stop //加载 BSS 段结束地址到 a4
	ble a4, a3, .Lclear_bss_done //如果 BSS 段结束地址小于或等于开始地址，跳转到清除完成
.Lclear_bss: //clear_bss 标签
	REG_S zero, (a3) //将零写入 BSS 段的当前地址
	add a3, a3, RISCV_SZPTR //移动到下一个地址
	blt a3, a4, .Lclear_bss //如果当前地址小于结束地址，继续清除
.Lclear_bss_done: //clear_bss_done 标签
#endif
	la a2, boot_cpu_hartid //加载 boot_cpu_hartid 地址到 a2
	XIP_FIXUP_OFFSET a2 //修正 a2 地址的 XIP 偏移
	REG_S a0, (a2) //将a0的值写入 boot_cpu_hartid

	/* Initialize page tables and relocate to virtual addresses */
	la tp, init_task //加载init_task结构体地址到tp
	la sp, init_thread_union + THREAD_SIZE //设置栈指针为 init_thread_union + 线程大小
	XIP_FIXUP_OFFSET sp //修正栈指针地址的 XIP 偏移
	addi sp, sp, -PT_SIZE_ON_STACK //调整栈指针，预留页表大小
	scs_load_init_stack //加载初始化栈
#ifdef CONFIG_BUILTIN_DTB //如果配置了内置设备树
	la a0, __dtb_start //加载设备树开始地址到 a0
	XIP_FIXUP_OFFSET a0 //修正a0地址的XIP偏移
#else //如果没有配置内置设备树
	mv a0, a1 //将 a1 移动到 a0
#endif /* CONFIG_BUILTIN_DTB */ //结束设备树条件编译
	call setup_vm // 调用设置虚拟内存的函数
#ifdef CONFIG_MMU //如果配置了MMU
	la a0, early_pg_dir //加载早期页表目录地址到 a0
	XIP_FIXUP_OFFSET a0 //修正 a0 地址的 XIP 偏移
	call relocate_enable_mmu //调用启用 MMU 的函数
#endif /* CONFIG_MMU */  //结束MMU条件编译

	call .Lsetup_trap_vector //调用设置陷阱向量的函数
	/* Restore C environment */
	la tp, init_task //加载 init_task 地址到tp
	la sp, init_thread_union + THREAD_SIZE //设置栈指针为 init_thread_union + 线程大小
	addi sp, sp, -PT_SIZE_ON_STACK //调整栈指针，预留页表大小
	scs_load_current //加载当前状态

#ifdef CONFIG_KASAN //如果配置了 KASAN
	call kasan_early_init //调用 KASAN 早期初始化
#endif 
	/* Start the kernel */
	call soc_early_init //调用系统单元早期初始化
	tail start_kernel //尾调用 start_kernel 函数

#ifdef CONFIG_RISCV_BOOT_SPINWAIT //如果配置了RISC-V启动自旋等待
.Lsecondary_start: //secondary_start 标签
	/* Set trap vector to spin forever to help debug */
	la a3, .Lsecondary_park //加载secondary_park地址到a3
	csrw CSR_TVEC, a3  //设置陷阱向量寄存器，指向 secondary_park

	slli a3, a0, LGREG //左移 a0 的值，计算栈指针
	la a1, __cpu_spinwait_stack_pointer //加载自旋等待栈指针地址到a1
	XIP_FIXUP_OFFSET a1 //修正 a1 地址的 XIP 偏移
	la a2, __cpu_spinwait_task_pointer //加载自旋等待任务指针地址到 a2
	XIP_FIXUP_OFFSET a2 //修正a2地址的XIP偏移
	add a1, a3, a1 //计算最终的栈指针
	add a2, a3, a2 //计算最终的任务指针

	/*
	 * This hart didn't win the lottery, so we wait for the winning hart to
	 * get far enough along the boot process that it should continue.
	 */
.Lwait_for_cpu_up: //wait_for_cpu_up标签
	/* FIXME: We should WFI to save some energy here. */
	REG_L sp, (a1) //从地址加载栈指针
	REG_L tp, (a2) //从地址加载任务指针
	beqz sp, .Lwait_for_cpu_up  //如果栈指针为零，继续等待
	beqz tp, .Lwait_for_cpu_up //如果任务指针为零，继续等待
	fence //执行栅栏操作，确保所有操作完成

	tail .Lsecondary_start_common //尾调用 secondary_start_common 函数
#endif /* CONFIG_RISCV_BOOT_SPINWAIT */

SYM_CODE_END(_start_kernel)

#ifdef CONFIG_RISCV_M_MODE
SYM_CODE_START_LOCAL(reset_regs)
	li	sp, 0
	li	gp, 0
	li	tp, 0
	li	t0, 0
	li	t1, 0
	li	t2, 0
	li	s0, 0
	li	s1, 0
	li	a2, 0
	li	a3, 0
	li	a4, 0
	li	a5, 0
	li	a6, 0
	li	a7, 0
	li	s2, 0
	li	s3, 0
	li	s4, 0
	li	s5, 0
	li	s6, 0
	li	s7, 0
	li	s8, 0
	li	s9, 0
	li	s10, 0
	li	s11, 0
	li	t3, 0
	li	t4, 0
	li	t5, 0
	li	t6, 0
	csrw	CSR_SCRATCH, 0

#ifdef CONFIG_FPU
	csrr	t0, CSR_MISA
	andi	t0, t0, (COMPAT_HWCAP_ISA_F | COMPAT_HWCAP_ISA_D)
	beqz	t0, .Lreset_regs_done_fpu

	li	t1, SR_FS
	csrs	CSR_STATUS, t1
	fmv.s.x	f0, zero
	fmv.s.x	f1, zero
	fmv.s.x	f2, zero
	fmv.s.x	f3, zero
	fmv.s.x	f4, zero
	fmv.s.x	f5, zero
	fmv.s.x	f6, zero
	fmv.s.x	f7, zero
	fmv.s.x	f8, zero
	fmv.s.x	f9, zero
	fmv.s.x	f10, zero
	fmv.s.x	f11, zero
	fmv.s.x	f12, zero
	fmv.s.x	f13, zero
	fmv.s.x	f14, zero
	fmv.s.x	f15, zero
	fmv.s.x	f16, zero
	fmv.s.x	f17, zero
	fmv.s.x	f18, zero
	fmv.s.x	f19, zero
	fmv.s.x	f20, zero
	fmv.s.x	f21, zero
	fmv.s.x	f22, zero
	fmv.s.x	f23, zero
	fmv.s.x	f24, zero
	fmv.s.x	f25, zero
	fmv.s.x	f26, zero
	fmv.s.x	f27, zero
	fmv.s.x	f28, zero
	fmv.s.x	f29, zero
	fmv.s.x	f30, zero
	fmv.s.x	f31, zero
	csrw	fcsr, 0
	/* note that the caller must clear SR_FS */
.Lreset_regs_done_fpu:
#endif /* CONFIG_FPU */

#ifdef CONFIG_RISCV_ISA_V
	csrr	t0, CSR_MISA
	li	t1, COMPAT_HWCAP_ISA_V
	and	t0, t0, t1
	beqz	t0, .Lreset_regs_done_vector

	/*
	 * Clear vector registers and reset vcsr
	 * VLMAX has a defined value, VLEN is a constant,
	 * and this form of vsetvli is defined to set vl to VLMAX.
	 */
	li	t1, SR_VS
	csrs	CSR_STATUS, t1
	csrs	CSR_VCSR, x0
	vsetvli t1, x0, e8, m8, ta, ma
	vmv.v.i v0, 0
	vmv.v.i v8, 0
	vmv.v.i v16, 0
	vmv.v.i v24, 0
	/* note that the caller must clear SR_VS */
.Lreset_regs_done_vector:
#endif /* CONFIG_RISCV_ISA_V */
	ret
SYM_CODE_END(reset_regs)
#endif /* CONFIG_RISCV_M_MODE */
