// SPDX-License-Identifier: GPL-2.0
#include <linux/mm_types.h>
#include <linux/maple_tree.h>
#include <linux/rwsem.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/cpumask.h>
#include <linux/mman.h>
#include <linux/pgtable.h>

#include <linux/atomic.h>
#include <linux/user_namespace.h>
#include <linux/iommu.h>
#include <asm/mmu.h>

#ifndef INIT_MM_CONTEXT
#define INIT_MM_CONTEXT(name)
#endif

const struct vm_operations_struct vma_dummy_vm_ops;

/*
 * For dynamically allocated mm_structs, there is a dynamically sized cpumask
 * at the end of the structure, the size of which depends on the maximum CPU
 * number the system can see. That way we allocate only as much memory for
 * mm_cpumask() as needed for the hundreds, or thousands of processes that
 * a system typically runs.
 *
 * Since there is only one init_mm in the entire system, keep it simple
 * and size this cpu_bitmask to NR_CPUS.
 */
struct mm_struct init_mm = {//定义一个名为 init_mm 的 mm_struct 结构体实例，表示初始内存管理结构
	.mm_mt		= MTREE_INIT_EXT(mm_mt, MM_MT_FLAGS, init_mm.mmap_lock),//初始化内存管理树，使用 mmap_lock 进行保护
	.pgd		= swapper_pg_dir,//设置进程全局目录（PGD）为交换进程的页目录
	.mm_users	= ATOMIC_INIT(2),//设置用户计数为 2，表示当前有两个引用
	.mm_count	= ATOMIC_INIT(1),// 设置内存管理结构体计数为 1，表示正在被使用
	.write_protect_seq = SEQCNT_ZERO(init_mm.write_protect_seq),//初始化写保护序列号为 0
	MMAP_LOCK_INITIALIZER(init_mm)//初始化 mmap 锁
	.page_table_lock =  __SPIN_LOCK_UNLOCKED(init_mm.page_table_lock),//初始化页表锁为未锁定状态
	.arg_lock	=  __SPIN_LOCK_UNLOCKED(init_mm.arg_lock),// 初始化参数锁为未锁定状态
	.mmlist		= LIST_HEAD_INIT(init_mm.mmlist),//初始化 mm_list 为一个空链表
#ifdef CONFIG_PER_VMA_LOCK
	.mm_lock_seq	= 0,//如果启用了每个虚拟内存区域锁，则初始化 mm_lock_seq 为 0
#endif
	.user_ns	= &init_user_ns,//设置用户命名空间为初始用户命名空间
	.cpu_bitmap	= CPU_BITS_NONE,//初始化 CPU 位图为空，表示没有 CPU 关联
	INIT_MM_CONTEXT(init_mm)//初始化内存管理上下文
};

void setup_initial_init_mm(void *start_code, void *end_code,
			   void *end_data, void *brk)
{
	init_mm.start_code = (unsigned long)start_code;
	init_mm.end_code = (unsigned long)end_code;
	init_mm.end_data = (unsigned long)end_data;
	init_mm.brk = (unsigned long)brk;
}
