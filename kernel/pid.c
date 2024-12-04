// SPDX-License-Identifier: GPL-2.0-only
/*
 * Generic pidhash and scalable, time-bounded PID allocator
 *
 * (C) 2002-2003 Nadia Yvette Chambers, IBM
 * (C) 2004 Nadia Yvette Chambers, Oracle
 * (C) 2002-2004 Ingo Molnar, Red Hat
 *
 * pid-structures are backing objects for tasks sharing a given ID to chain
 * against. There is very little to them aside from hashing them and
 * parking tasks using given ID's on a list.
 *
 * The hash is always changed with the tasklist_lock write-acquired,
 * and the hash is only accessed with the tasklist_lock at least
 * read-acquired, so there's no additional SMP locking needed here.
 *
 * We have a list of bitmap pages, which bitmaps represent the PID space.
 * Allocating and freeing PIDs is completely lockless. The worst-case
 * allocation scenario when all but one out of 1 million PIDs possible are
 * allocated already: the scanning of 32 list entries and at most PAGE_SIZE
 * bytes. The typical fastpath is a single successful setbit. Freeing is O(1).
 *
 * Pid namespaces:
 *    (C) 2007 Pavel Emelyanov <xemul@openvz.org>, OpenVZ, SWsoft Inc.
 *    (C) 2007 Sukadev Bhattiprolu <sukadev@us.ibm.com>, IBM
 *     Many thanks to Oleg Nesterov for comments and help
 *
 */

#include <linux/mm.h>
#include <linux/export.h>
#include <linux/slab.h>
#include <linux/init.h>
#include <linux/rculist.h>
#include <linux/memblock.h>
#include <linux/pid_namespace.h>
#include <linux/init_task.h>
#include <linux/syscalls.h>
#include <linux/proc_ns.h>
#include <linux/refcount.h>
#include <linux/anon_inodes.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/idr.h>
#include <linux/pidfs.h>
#include <net/sock.h>
#include <uapi/linux/pidfd.h>

struct pid init_struct_pid = {
	.count		= REFCOUNT_INIT(1),//初始化引用计数为 1，表示该 PID 至少被一个任务引用
	.tasks		= {
		{ .first = NULL },//初始化 PID 类型链表（如 PIDTYPE_PID）的第一个节点为空
		{ .first = NULL },//初始化 PID 类型链表（如 PIDTYPE_TGID）的第一个节点为空
		{ .first = NULL },//初始化 PID 类型链表（如 PIDTYPE_PGID）的第一个节点为空
	},
	.level		= 0,//设置 PID 的级别为 0，表示它是初始命名空间的 PID
	.numbers	= { {
		.nr		= 0,//设置 PID 的编号为 0，表示这是系统的第一个进程（即 init 进程）
		.ns		= &init_pid_ns,//指向初始 PID 命名空间
	}, }
};

int pid_max = PID_MAX_DEFAULT;

int pid_max_min = RESERVED_PIDS + 1;
int pid_max_max = PID_MAX_LIMIT;
/*
 * Pseudo filesystems start inode numbering after one. We use Reserved
 * PIDs as a natural offset.
 */
static u64 pidfs_ino = RESERVED_PIDS;

/*
 * PID-map pages start out as NULL, they get allocated upon
 * first use and are never deallocated. This way a low pid_max
 * value does not cause lots of bitmaps to be allocated, but
 * the scheme scales to up to 4 million PIDs, runtime.
 */
struct pid_namespace init_pid_ns = {
	.ns.count = REFCOUNT_INIT(2),
	.idr = IDR_INIT(init_pid_ns.idr),
	.pid_allocated = PIDNS_ADDING,
	.level = 0,
	.child_reaper = &init_task,
	.user_ns = &init_user_ns,
	.ns.inum = PROC_PID_INIT_INO,
#ifdef CONFIG_PID_NS
	.ns.ops = &pidns_operations,
#endif
#if defined(CONFIG_SYSCTL) && defined(CONFIG_MEMFD_CREATE)
	.memfd_noexec_scope = MEMFD_NOEXEC_SCOPE_EXEC,
#endif
};
EXPORT_SYMBOL_GPL(init_pid_ns);

/*
 * Note: disable interrupts while the pidmap_lock is held as an
 * interrupt might come in and do read_lock(&tasklist_lock).
 *
 * If we don't disable interrupts there is a nasty deadlock between
 * detach_pid()->free_pid() and another cpu that does
 * spin_lock(&pidmap_lock) followed by an interrupt routine that does
 * read_lock(&tasklist_lock);
 *
 * After we clean up the tasklist_lock and know there are no
 * irq handlers that take it we can leave the interrupts enabled.
 * For now it is easier to be safe than to prove it can't happen.
 */

static  __cacheline_aligned_in_smp DEFINE_SPINLOCK(pidmap_lock);

void put_pid(struct pid *pid)
{
	struct pid_namespace *ns;

	if (!pid)
		return;

	ns = pid->numbers[pid->level].ns;
	if (refcount_dec_and_test(&pid->count)) {
		kmem_cache_free(ns->pid_cachep, pid);
		put_pid_ns(ns);
	}
}
EXPORT_SYMBOL_GPL(put_pid);

static void delayed_put_pid(struct rcu_head *rhp)
{
	struct pid *pid = container_of(rhp, struct pid, rcu);
	put_pid(pid);
}

void free_pid(struct pid *pid)
{
	/* We can be called with write_lock_irq(&tasklist_lock) held */
	int i;
	unsigned long flags;

	spin_lock_irqsave(&pidmap_lock, flags);
	for (i = 0; i <= pid->level; i++) {
		struct upid *upid = pid->numbers + i;
		struct pid_namespace *ns = upid->ns;
		switch (--ns->pid_allocated) {
		case 2:
		case 1:
			/* When all that is left in the pid namespace
			 * is the reaper wake up the reaper.  The reaper
			 * may be sleeping in zap_pid_ns_processes().
			 */
			wake_up_process(ns->child_reaper);
			break;
		case PIDNS_ADDING:
			/* Handle a fork failure of the first process */
			WARN_ON(ns->child_reaper);
			ns->pid_allocated = 0;
			break;
		}

		idr_remove(&ns->idr, upid->nr);
	}
	spin_unlock_irqrestore(&pidmap_lock, flags);

	call_rcu(&pid->rcu, delayed_put_pid);
}

struct pid *alloc_pid(struct pid_namespace *ns, pid_t *set_tid,
		      size_t set_tid_size)
{
	struct pid *pid;//指向要分配的 PID 结构体的指针
	enum pid_type type;//枚举变量，用于遍历所有 PID 类型
	int i, nr;
	struct pid_namespace *tmp;//用于遍历 PID 命名空间
	struct upid *upid;//指向与 PID 相关的 upid 结构体
	int retval = -ENOMEM;//返回值变量，初始为内存不足的错误码

	/*
	 * set_tid_size contains the size of the set_tid array. Starting at
	 * the most nested currently active PID namespace it tells alloc_pid()
	 * which PID to set for a process in that most nested PID namespace
	 * up to set_tid_size PID namespaces. It does not have to set the PID
	 * for a process in all nested PID namespaces but set_tid_size must
	 * never be greater than the current ns->level + 1.
	 * set_tid_size 包含 set_tid 数组的大小。从当前最内层的活动 PID 命名空间开始，
	 * 它告诉 alloc_pid() 在最内层的 PID 命名空间中为进程设置哪个 PID，最多可设置
	 * set_tid_size 个 PID 命名空间。它不必在所有嵌套的 PID 命名空间中设置 PID，
	 * 但 set_tid_size 不能大于当前 ns->level + 1。
	 */
	if (set_tid_size > ns->level + 1)//检查 set_tid_size 是否超过当前 PID 命名空间层级数
		return ERR_PTR(-EINVAL);//返回错误指针，表示无效参数

	pid = kmem_cache_alloc(ns->pid_cachep, GFP_KERNEL);//从 PID 缓存中分配一个新的 PID 结构体
	if (!pid)
		return ERR_PTR(retval);//返回错误指针，表示内存不足

	tmp = ns;//初始化临时命名空间指针为当前命名空间
	pid->level = ns->level;//设置 PID 的层级为当前命名空间的层级

	for (i = ns->level; i >= 0; i--) {//从当前命名空间向上遍历所有命名空间,检查是否合规
		int tid = 0;

		if (set_tid_size) {//如果 set_tid_size 非零，表示需要设置特定的 PID
			tid = set_tid[ns->level - i];// 获取需要设置的 PID

			retval = -EINVAL;
			if (tid < 1 || tid >= pid_max)//如果 PID 不在有效范围内
				goto out_free;//跳转到错误处理部分，释放已分配的资源
			/*
			 * Also fail if a PID != 1 is requested and
			 * no PID 1 exists.
			 *  如果请求的 PID 不是 1 且没有 PID 1 存在，则失败。
			 */
			if (tid != 1 && !tmp->child_reaper)//如果请求的 PID 不是 1 且没有子进程回收器
				goto out_free;//跳转到错误处理部分
			retval = -EPERM;// 设置返回值为权限不足错误
			if (!checkpoint_restore_ns_capable(tmp->user_ns))//检查是否有权限设置 PID
				goto out_free;
			set_tid_size--;//减少 set_tid_size，表示已处理一个命名空间
		}

		idr_preload(GFP_KERNEL);//预加载 IDR（整数分配器）以提高分配效率
		spin_lock_irq(&pidmap_lock);//获取 pidmap_lock 自旋锁，保护 PID 分配过程

		if (tid) {// 如果需要设置特定的 PID
			nr = idr_alloc(&tmp->idr, NULL, tid,
				       tid + 1, GFP_ATOMIC);//在 IDR 中分配指定的 PID
			/*
			 * If ENOSPC is returned it means that the PID is
			 * alreay in use. Return EEXIST in that case.
			 * 如果返回 ENOSPC，则表示 PID 已被使用，返回 EEXIST 错误。
			 */
			if (nr == -ENOSPC)//如果返回值为 ENOSPC，表示没有空间分配
				nr = -EEXIST;//设置返回值为已存在错误
		} else {
			int pid_min = 1;//初始化最小 PID 为 1
			/*
			 * init really needs pid 1, but after reaching the
			 * maximum wrap back to RESERVED_PIDS
			 * init 进程需要 PID 1，但在达到最大值后，会回绕到 RESERVED_PIDS。
			 */
			if (idr_get_cursor(&tmp->idr) > RESERVED_PIDS)//如果 IDR 游标超过保留的 PID
				pid_min = RESERVED_PIDS;// 设置最小 PID 为保留的 PID

			/*
			 * Store a null pointer so find_pid_ns does not find
			 * a partially initialized PID (see below).
			 * 存储一个空指针，以防止 find_pid_ns 查找到部分初始化的 PID。
			 */
			nr = idr_alloc_cyclic(&tmp->idr, NULL, pid_min,
					      pid_max, GFP_ATOMIC);//循环分配一个可用的 PID
		}
		spin_unlock_irq(&pidmap_lock);//释放 pidmap_lock 自旋锁
		idr_preload_end();//结束 IDR 预加载

		if (nr < 0) {// 如果分配失败
			retval = (nr == -ENOSPC) ? -EAGAIN : nr;//设置返回值为适当的错误码
			goto out_free;
		}

		pid->numbers[i].nr = nr;//设置进程PID结构体中对应层级的PID编号
		pid->numbers[i].ns = tmp;//设置 PID 结构体中对应层级的命名空间
		tmp = tmp->parent;// 移动到父命名空间，继续处理
	}

	/*
	 * ENOMEM is not the most obvious choice especially for the case
	 * where the child subreaper has already exited and the pid
	 * namespace denies the creation of any new processes. But ENOMEM
	 * is what we have exposed to userspace for a long time and it is
	 * documented behavior for pid namespaces. So we can't easily
	 * change it even if there were an error code better suited.
	 */
	retval = -ENOMEM;//设置返回值为内存不足错误

	get_pid_ns(ns);//增加命名空间的引用计数
	refcount_set(&pid->count, 1);//初始化 PID 的引用计数为 1
	spin_lock_init(&pid->lock);//初始化 PID 的自旋锁
	for (type = 0; type < PIDTYPE_MAX; ++type)//初始化 PID 的任务链表头
		INIT_HLIST_HEAD(&pid->tasks[type]);

	init_waitqueue_head(&pid->wait_pidfd);//初始化等待队列头
	INIT_HLIST_HEAD(&pid->inodes);//初始化 PID 的 inode 链表头

	upid = pid->numbers + ns->level;//获取最高层级的 upid 指针
	spin_lock_irq(&pidmap_lock);//获取 pidmap_lock 自旋锁
	if (!(ns->pid_allocated & PIDNS_ADDING))//检查命名空间是否允许分配 PID
		goto out_unlock;
	pid->stashed = NULL;
	pid->ino = ++pidfs_ino;//为 PID 分配唯一的 inode 编号
	for ( ; upid >= pid->numbers; --upid) {//遍历所有层级的 upid
		/* Make the PID visible to find_pid_ns. */
		idr_replace(&upid->ns->idr, pid, upid->nr);//替换 IDR 中的条目，使 PID 可见
		upid->ns->pid_allocated++;//增加命名空间中分配的 PID 数量
	}
	spin_unlock_irq(&pidmap_lock);//释放 pidmap_lock 自旋锁

	return pid;//返回分配的 PID 结构体

out_unlock:
	spin_unlock_irq(&pidmap_lock);//释放 pidmap_lock 自旋锁
	put_pid_ns(ns);//释放命名空间的引用

out_free:
	spin_lock_irq(&pidmap_lock);//获取 pidmap_lock 自旋锁，保护资源释放过程
	while (++i <= ns->level) {//遍历所有已分配的 PID，进行清理
		upid = pid->numbers + i;//获取对应层级的 upid 指针
		idr_remove(&upid->ns->idr, upid->nr);//从 IDR 中移除该 PID，以释放资源
	}

	/* 如果首次分配 PID 失败，重置状态 */
	if (ns->pid_allocated == PIDNS_ADDING)//检查命名空间的 PID 分配状态是否为正在添加
		idr_set_cursor(&ns->idr, 0);//重置 IDR 游标，以便下次重新分配

	spin_unlock_irq(&pidmap_lock);//释放 pidmap_lock 自旋锁

	kmem_cache_free(ns->pid_cachep, pid);//释放分配的 PID 结构体内存
	return ERR_PTR(retval);//返回错误指针，指示分配失败的原因
}

void disable_pid_allocation(struct pid_namespace *ns)
{
	spin_lock_irq(&pidmap_lock);
	ns->pid_allocated &= ~PIDNS_ADDING;
	spin_unlock_irq(&pidmap_lock);
}

struct pid *find_pid_ns(int nr, struct pid_namespace *ns)
{
	return idr_find(&ns->idr, nr);
}
EXPORT_SYMBOL_GPL(find_pid_ns);

struct pid *find_vpid(int nr)
{
	return find_pid_ns(nr, task_active_pid_ns(current));
}
EXPORT_SYMBOL_GPL(find_vpid);

static struct pid **task_pid_ptr(struct task_struct *task, enum pid_type type)
{
	return (type == PIDTYPE_PID) ?
		&task->thread_pid :
		&task->signal->pids[type];
}

/*
 * attach_pid() must be called with the tasklist_lock write-held.
 */
void attach_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid = *task_pid_ptr(task, type);
	hlist_add_head_rcu(&task->pid_links[type], &pid->tasks[type]);
}

static void __change_pid(struct task_struct *task, enum pid_type type,
			struct pid *new)
{
	struct pid **pid_ptr = task_pid_ptr(task, type);
	struct pid *pid;
	int tmp;

	pid = *pid_ptr;

	hlist_del_rcu(&task->pid_links[type]);
	*pid_ptr = new;

	if (type == PIDTYPE_PID) {
		WARN_ON_ONCE(pid_has_task(pid, PIDTYPE_PID));
		wake_up_all(&pid->wait_pidfd);
	}

	for (tmp = PIDTYPE_MAX; --tmp >= 0; )
		if (pid_has_task(pid, tmp))
			return;

	free_pid(pid);
}

void detach_pid(struct task_struct *task, enum pid_type type)
{
	__change_pid(task, type, NULL);
}

void change_pid(struct task_struct *task, enum pid_type type,
		struct pid *pid)
{
	__change_pid(task, type, pid);
	attach_pid(task, type);
}

void exchange_tids(struct task_struct *left, struct task_struct *right)
{
	struct pid *pid1 = left->thread_pid;
	struct pid *pid2 = right->thread_pid;
	struct hlist_head *head1 = &pid1->tasks[PIDTYPE_PID];
	struct hlist_head *head2 = &pid2->tasks[PIDTYPE_PID];

	/* Swap the single entry tid lists */
	hlists_swap_heads_rcu(head1, head2);

	/* Swap the per task_struct pid */
	rcu_assign_pointer(left->thread_pid, pid2);
	rcu_assign_pointer(right->thread_pid, pid1);

	/* Swap the cached value */
	WRITE_ONCE(left->pid, pid_nr(pid2));
	WRITE_ONCE(right->pid, pid_nr(pid1));
}

/* transfer_pid is an optimization of attach_pid(new), detach_pid(old) */
void transfer_pid(struct task_struct *old, struct task_struct *new,
			   enum pid_type type)
{
	WARN_ON_ONCE(type == PIDTYPE_PID);
	hlist_replace_rcu(&old->pid_links[type], &new->pid_links[type]);
}

struct task_struct *pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result = NULL;
	if (pid) {
		struct hlist_node *first;
		first = rcu_dereference_check(hlist_first_rcu(&pid->tasks[type]),
					      lockdep_tasklist_lock_is_held());
		if (first)
			result = hlist_entry(first, struct task_struct, pid_links[(type)]);
	}
	return result;
}
EXPORT_SYMBOL(pid_task);

/*
 * Must be called under rcu_read_lock().
 */
struct task_struct *find_task_by_pid_ns(pid_t nr, struct pid_namespace *ns)
{
	RCU_LOCKDEP_WARN(!rcu_read_lock_held(),
			 "find_task_by_pid_ns() needs rcu_read_lock() protection");
	return pid_task(find_pid_ns(nr, ns), PIDTYPE_PID);
}

struct task_struct *find_task_by_vpid(pid_t vnr)
{
	return find_task_by_pid_ns(vnr, task_active_pid_ns(current));
}

struct task_struct *find_get_task_by_vpid(pid_t nr)
{
	struct task_struct *task;

	rcu_read_lock();
	task = find_task_by_vpid(nr);
	if (task)
		get_task_struct(task);
	rcu_read_unlock();

	return task;
}

struct pid *get_task_pid(struct task_struct *task, enum pid_type type)
{
	struct pid *pid;
	rcu_read_lock();
	pid = get_pid(rcu_dereference(*task_pid_ptr(task, type)));
	rcu_read_unlock();
	return pid;
}
EXPORT_SYMBOL_GPL(get_task_pid);

struct task_struct *get_pid_task(struct pid *pid, enum pid_type type)
{
	struct task_struct *result;
	rcu_read_lock();
	result = pid_task(pid, type);
	if (result)
		get_task_struct(result);
	rcu_read_unlock();
	return result;
}
EXPORT_SYMBOL_GPL(get_pid_task);

struct pid *find_get_pid(pid_t nr)
{
	struct pid *pid;

	rcu_read_lock();
	pid = get_pid(find_vpid(nr));
	rcu_read_unlock();

	return pid;
}
EXPORT_SYMBOL_GPL(find_get_pid);

pid_t pid_nr_ns(struct pid *pid, struct pid_namespace *ns)
{
	struct upid *upid;
	pid_t nr = 0;

	if (pid && ns->level <= pid->level) {
		upid = &pid->numbers[ns->level];
		if (upid->ns == ns)
			nr = upid->nr;
	}
	return nr;
}
EXPORT_SYMBOL_GPL(pid_nr_ns);

pid_t pid_vnr(struct pid *pid)
{
	return pid_nr_ns(pid, task_active_pid_ns(current));
}
EXPORT_SYMBOL_GPL(pid_vnr);

pid_t __task_pid_nr_ns(struct task_struct *task, enum pid_type type,
			struct pid_namespace *ns)
{
	pid_t nr = 0;

	rcu_read_lock();
	if (!ns)
		ns = task_active_pid_ns(current);
	nr = pid_nr_ns(rcu_dereference(*task_pid_ptr(task, type)), ns);
	rcu_read_unlock();

	return nr;
}
EXPORT_SYMBOL(__task_pid_nr_ns);

struct pid_namespace *task_active_pid_ns(struct task_struct *tsk)
{
	return ns_of_pid(task_pid(tsk));
}
EXPORT_SYMBOL_GPL(task_active_pid_ns);

/*
 * Used by proc to find the first pid that is greater than or equal to nr.
 *
 * If there is a pid at nr this function is exactly the same as find_pid_ns.
 */
struct pid *find_ge_pid(int nr, struct pid_namespace *ns)
{
	return idr_get_next(&ns->idr, &nr);
}
EXPORT_SYMBOL_GPL(find_ge_pid);

struct pid *pidfd_get_pid(unsigned int fd, unsigned int *flags)
{
	struct fd f;
	struct pid *pid;

	f = fdget(fd);
	if (!f.file)
		return ERR_PTR(-EBADF);

	pid = pidfd_pid(f.file);
	if (!IS_ERR(pid)) {
		get_pid(pid);
		*flags = f.file->f_flags;
	}

	fdput(f);
	return pid;
}

/**
 * pidfd_get_task() - Get the task associated with a pidfd
 *
 * @pidfd: pidfd for which to get the task
 * @flags: flags associated with this pidfd
 *
 * Return the task associated with @pidfd. The function takes a reference on
 * the returned task. The caller is responsible for releasing that reference.
 *
 * Return: On success, the task_struct associated with the pidfd.
 *	   On error, a negative errno number will be returned.
 */
struct task_struct *pidfd_get_task(int pidfd, unsigned int *flags)
{
	unsigned int f_flags;
	struct pid *pid;
	struct task_struct *task;

	pid = pidfd_get_pid(pidfd, &f_flags);
	if (IS_ERR(pid))
		return ERR_CAST(pid);

	task = get_pid_task(pid, PIDTYPE_TGID);
	put_pid(pid);
	if (!task)
		return ERR_PTR(-ESRCH);

	*flags = f_flags;
	return task;
}

/**
 * pidfd_create() - Create a new pid file descriptor.
 *
 * @pid:   struct pid that the pidfd will reference
 * @flags: flags to pass
 *
 * This creates a new pid file descriptor with the O_CLOEXEC flag set.
 *
 * Note, that this function can only be called after the fd table has
 * been unshared to avoid leaking the pidfd to the new process.
 *
 * This symbol should not be explicitly exported to loadable modules.
 *
 * Return: On success, a cloexec pidfd is returned.
 *         On error, a negative errno number will be returned.
 */
static int pidfd_create(struct pid *pid, unsigned int flags)
{
	int pidfd;
	struct file *pidfd_file;

	pidfd = pidfd_prepare(pid, flags, &pidfd_file);
	if (pidfd < 0)
		return pidfd;

	fd_install(pidfd, pidfd_file);
	return pidfd;
}

/**
 * sys_pidfd_open() - Open new pid file descriptor.
 *
 * @pid:   pid for which to retrieve a pidfd
 * @flags: flags to pass
 *
 * This creates a new pid file descriptor with the O_CLOEXEC flag set for
 * the task identified by @pid. Without PIDFD_THREAD flag the target task
 * must be a thread-group leader.
 *
 * Return: On success, a cloexec pidfd is returned.
 *         On error, a negative errno number will be returned.
 */
SYSCALL_DEFINE2(pidfd_open, pid_t, pid, unsigned int, flags)
{
	int fd;
	struct pid *p;

	if (flags & ~(PIDFD_NONBLOCK | PIDFD_THREAD))
		return -EINVAL;

	if (pid <= 0)
		return -EINVAL;

	p = find_get_pid(pid);
	if (!p)
		return -ESRCH;

	fd = pidfd_create(p, flags);

	put_pid(p);
	return fd;
}

void __init pid_idr_init(void)//初始化进程ID（PID）相关的结构
{
	/* Verify no one has done anything silly: */
	BUILD_BUG_ON(PID_MAX_LIMIT >= PIDNS_ADDING);//检查 PID_MAX_LIMIT 是否小于 PIDNS_ADDING，若不满足则编译错误

	/* bump default and minimum pid_max based on number of cpus */
	pid_max = min(pid_max_max, max_t(int, pid_max,
				PIDS_PER_CPU_DEFAULT * num_possible_cpus()));//更新 pid最大数量
	pid_max_min = max_t(int, pid_max_min,
				PIDS_PER_CPU_MIN * num_possible_cpus());//更新 pid最小数量
	pr_info("pid_max: default: %u minimum: %u\n", pid_max, pid_max_min);//输出当前的 pid_max 和 pid_max_min

	idr_init(&init_pid_ns.idr);// 初始化 PID 命名空间的 IDR（索引-动态数组）结构，为 PID 的分配和管理做好准备。

	init_pid_ns.pid_cachep = kmem_cache_create("pid",
			struct_size_t(struct pid, numbers, 1),
			__alignof__(struct pid),
			SLAB_HWCACHE_ALIGN | SLAB_PANIC | SLAB_ACCOUNT,
			NULL);//创建PID命名空间中用于管理PID结构的内存缓存
}

static struct file *__pidfd_fget(struct task_struct *task, int fd)
{
	struct file *file;
	int ret;

	ret = down_read_killable(&task->signal->exec_update_lock);
	if (ret)
		return ERR_PTR(ret);

	if (ptrace_may_access(task, PTRACE_MODE_ATTACH_REALCREDS))
		file = fget_task(task, fd);
	else
		file = ERR_PTR(-EPERM);

	up_read(&task->signal->exec_update_lock);

	if (!file) {
		/*
		 * It is possible that the target thread is exiting; it can be
		 * either:
		 * 1. before exit_signals(), which gives a real fd
		 * 2. before exit_files() takes the task_lock() gives a real fd
		 * 3. after exit_files() releases task_lock(), ->files is NULL;
		 *    this has PF_EXITING, since it was set in exit_signals(),
		 *    __pidfd_fget() returns EBADF.
		 * In case 3 we get EBADF, but that really means ESRCH, since
		 * the task is currently exiting and has freed its files
		 * struct, so we fix it up.
		 */
		if (task->flags & PF_EXITING)
			file = ERR_PTR(-ESRCH);
		else
			file = ERR_PTR(-EBADF);
	}

	return file;
}

static int pidfd_getfd(struct pid *pid, int fd)
{
	struct task_struct *task;
	struct file *file;
	int ret;

	task = get_pid_task(pid, PIDTYPE_PID);
	if (!task)
		return -ESRCH;

	file = __pidfd_fget(task, fd);
	put_task_struct(task);
	if (IS_ERR(file))
		return PTR_ERR(file);

	ret = receive_fd(file, NULL, O_CLOEXEC);
	fput(file);

	return ret;
}

/**
 * sys_pidfd_getfd() - Get a file descriptor from another process
 *
 * @pidfd:	the pidfd file descriptor of the process
 * @fd:		the file descriptor number to get
 * @flags:	flags on how to get the fd (reserved)
 *
 * This syscall gets a copy of a file descriptor from another process
 * based on the pidfd, and file descriptor number. It requires that
 * the calling process has the ability to ptrace the process represented
 * by the pidfd. The process which is having its file descriptor copied
 * is otherwise unaffected.
 *
 * Return: On success, a cloexec file descriptor is returned.
 *         On error, a negative errno number will be returned.
 */
SYSCALL_DEFINE3(pidfd_getfd, int, pidfd, int, fd,
		unsigned int, flags)
{
	struct pid *pid;
	struct fd f;
	int ret;

	/* flags is currently unused - make sure it's unset */
	if (flags)
		return -EINVAL;

	f = fdget(pidfd);
	if (!f.file)
		return -EBADF;

	pid = pidfd_pid(f.file);
	if (IS_ERR(pid))
		ret = PTR_ERR(pid);
	else
		ret = pidfd_getfd(pid, fd);

	fdput(f);
	return ret;
}
