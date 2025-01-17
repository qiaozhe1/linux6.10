// SPDX-License-Identifier: GPL-2.0-only
/* Kernel thread helper functions.
 *   Copyright (C) 2004 IBM Corporation, Rusty Russell.
 *   Copyright (C) 2009 Red Hat, Inc.
 *
 * Creation is done via kthreadd, so that we get a clean environment
 * even if we're invoked from userspace (think modprobe, hotplug cpu,
 * etc.).
 */
#include <uapi/linux/sched/types.h>
#include <linux/mm.h>
#include <linux/mmu_context.h>
#include <linux/sched.h>
#include <linux/sched/mm.h>
#include <linux/sched/task.h>
#include <linux/kthread.h>
#include <linux/completion.h>
#include <linux/err.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/unistd.h>
#include <linux/file.h>
#include <linux/export.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/freezer.h>
#include <linux/ptrace.h>
#include <linux/uaccess.h>
#include <linux/numa.h>
#include <linux/sched/isolation.h>
#include <trace/events/sched.h>


static DEFINE_SPINLOCK(kthread_create_lock);
static LIST_HEAD(kthread_create_list);
struct task_struct *kthreadd_task;

struct kthread_create_info
{
	/* Information passed to kthread() from kthreadd. */
	char *full_name;//线程的完整名称，使用格式化字符串生成
	int (*threadfn)(void *data);//线程的主函数
	void *data;//传递给线程函数的参数
	int node;//NUMA 节点编号，-1 表示未指定节点

	/* Result passed back to kthread_create() from kthreadd. */
	struct task_struct *result;//创建的线程任务结构指针
	struct completion *done;//同步机制，用于通知线程创建完成

	struct list_head list;//链表节点，用于将创建请求加入全局队列
};

struct kthread {
	unsigned long flags;
	unsigned int cpu;
	int result;
	int (*threadfn)(void *);
	void *data;
	struct completion parked;
	struct completion exited;
#ifdef CONFIG_BLK_CGROUP
	struct cgroup_subsys_state *blkcg_css;
#endif
	/* To store the full name if task comm is truncated. */
	char *full_name;
};

enum KTHREAD_BITS {
	KTHREAD_IS_PER_CPU = 0,
	KTHREAD_SHOULD_STOP,
	KTHREAD_SHOULD_PARK,
};

static inline struct kthread *to_kthread(struct task_struct *k)
{
	WARN_ON(!(k->flags & PF_KTHREAD));
	return k->worker_private;
}

/*
 * Variant of to_kthread() that doesn't assume @p is a kthread.
 *
 * Per construction; when:
 *
 *   (p->flags & PF_KTHREAD) && p->worker_private
 *
 * the task is both a kthread and struct kthread is persistent. However
 * PF_KTHREAD on it's own is not, kernel_thread() can exec() (See umh.c and
 * begin_new_exec()).
 */
static inline struct kthread *__to_kthread(struct task_struct *p)
{
	void *kthread = p->worker_private;
	if (kthread && !(p->flags & PF_KTHREAD))
		kthread = NULL;
	return kthread;
}

void get_kthread_comm(char *buf, size_t buf_size, struct task_struct *tsk)
{
	struct kthread *kthread = to_kthread(tsk);

	if (!kthread || !kthread->full_name) {
		__get_task_comm(buf, buf_size, tsk);
		return;
	}

	strscpy_pad(buf, kthread->full_name, buf_size);
}

bool set_kthread_struct(struct task_struct *p)
{
	struct kthread *kthread;

	if (WARN_ON_ONCE(to_kthread(p)))
		return false;

	kthread = kzalloc(sizeof(*kthread), GFP_KERNEL);
	if (!kthread)
		return false;

	init_completion(&kthread->exited);
	init_completion(&kthread->parked);
	p->vfork_done = &kthread->exited;

	p->worker_private = kthread;
	return true;
}

void free_kthread_struct(struct task_struct *k)
{
	struct kthread *kthread;

	/*
	 * Can be NULL if kmalloc() in set_kthread_struct() failed.
	 */
	kthread = to_kthread(k);
	if (!kthread)
		return;

#ifdef CONFIG_BLK_CGROUP
	WARN_ON_ONCE(kthread->blkcg_css);
#endif
	k->worker_private = NULL;
	kfree(kthread->full_name);
	kfree(kthread);
}

/**
 * kthread_should_stop - should this kthread return now?
 *
 * When someone calls kthread_stop() on your kthread, it will be woken
 * and this will return true.  You should then return, and your return
 * value will be passed through to kthread_stop().
 */
bool kthread_should_stop(void)
{
	return test_bit(KTHREAD_SHOULD_STOP, &to_kthread(current)->flags);
}
EXPORT_SYMBOL(kthread_should_stop);

static bool __kthread_should_park(struct task_struct *k)
{
	return test_bit(KTHREAD_SHOULD_PARK, &to_kthread(k)->flags);
}

/**
 * kthread_should_park - should this kthread park now?
 *
 * When someone calls kthread_park() on your kthread, it will be woken
 * and this will return true.  You should then do the necessary
 * cleanup and call kthread_parkme()
 *
 * Similar to kthread_should_stop(), but this keeps the thread alive
 * and in a park position. kthread_unpark() "restarts" the thread and
 * calls the thread function again.
 */
bool kthread_should_park(void)
{
	return __kthread_should_park(current);
}
EXPORT_SYMBOL_GPL(kthread_should_park);

bool kthread_should_stop_or_park(void)
{
	struct kthread *kthread = __to_kthread(current);

	if (!kthread)
		return false;

	return kthread->flags & (BIT(KTHREAD_SHOULD_STOP) | BIT(KTHREAD_SHOULD_PARK));
}

/**
 * kthread_freezable_should_stop - should this freezable kthread return now?
 * @was_frozen: optional out parameter, indicates whether %current was frozen
 *
 * kthread_should_stop() for freezable kthreads, which will enter
 * refrigerator if necessary.  This function is safe from kthread_stop() /
 * freezer deadlock and freezable kthreads should use this function instead
 * of calling try_to_freeze() directly.
 */
bool kthread_freezable_should_stop(bool *was_frozen)
{
	bool frozen = false;

	might_sleep();

	if (unlikely(freezing(current)))
		frozen = __refrigerator(true);

	if (was_frozen)
		*was_frozen = frozen;

	return kthread_should_stop();
}
EXPORT_SYMBOL_GPL(kthread_freezable_should_stop);

/**
 * kthread_func - return the function specified on kthread creation
 * @task: kthread task in question
 *
 * Returns NULL if the task is not a kthread.
 */
void *kthread_func(struct task_struct *task)
{
	struct kthread *kthread = __to_kthread(task);
	if (kthread)
		return kthread->threadfn;
	return NULL;
}
EXPORT_SYMBOL_GPL(kthread_func);

/**
 * kthread_data - return data value specified on kthread creation
 * @task: kthread task in question
 *
 * Return the data value specified when kthread @task was created.
 * The caller is responsible for ensuring the validity of @task when
 * calling this function.
 */
void *kthread_data(struct task_struct *task)
{
	return to_kthread(task)->data;
}
EXPORT_SYMBOL_GPL(kthread_data);

/**
 * kthread_probe_data - speculative version of kthread_data()
 * @task: possible kthread task in question
 *
 * @task could be a kthread task.  Return the data value specified when it
 * was created if accessible.  If @task isn't a kthread task or its data is
 * inaccessible for any reason, %NULL is returned.  This function requires
 * that @task itself is safe to dereference.
 */
void *kthread_probe_data(struct task_struct *task)
{
	struct kthread *kthread = __to_kthread(task);
	void *data = NULL;

	if (kthread)
		copy_from_kernel_nofault(&data, &kthread->data, sizeof(data));
	return data;
}

static void __kthread_parkme(struct kthread *self)
{
	for (;;) {
		/*
		 * TASK_PARKED is a special state; we must serialize against
		 * possible pending wakeups to avoid store-store collisions on
		 * task->state.
		 *
		 * Such a collision might possibly result in the task state
		 * changin from TASK_PARKED and us failing the
		 * wait_task_inactive() in kthread_park().
		 */
		set_special_state(TASK_PARKED);
		if (!test_bit(KTHREAD_SHOULD_PARK, &self->flags))
			break;

		/*
		 * Thread is going to call schedule(), do not preempt it,
		 * or the caller of kthread_park() may spend more time in
		 * wait_task_inactive().
		 */
		preempt_disable();
		complete(&self->parked);
		schedule_preempt_disabled();
		preempt_enable();
	}
	__set_current_state(TASK_RUNNING);
}

void kthread_parkme(void)
{
	__kthread_parkme(to_kthread(current));
}
EXPORT_SYMBOL_GPL(kthread_parkme);

/**
 * kthread_exit - Cause the current kthread return @result to kthread_stop().
 * @result: The integer value to return to kthread_stop().
 *
 * While kthread_exit can be called directly, it exists so that
 * functions which do some additional work in non-modular code such as
 * module_put_and_kthread_exit can be implemented.
 *
 * Does not return.
 */
void __noreturn kthread_exit(long result)
{
	struct kthread *kthread = to_kthread(current);
	kthread->result = result;
	do_exit(0);
}
EXPORT_SYMBOL(kthread_exit);

/**
 * kthread_complete_and_exit - Exit the current kthread.
 * @comp: Completion to complete
 * @code: The integer value to return to kthread_stop().
 *
 * If present, complete @comp and then return code to kthread_stop().
 *
 * A kernel thread whose module may be removed after the completion of
 * @comp can use this function to exit safely.
 *
 * Does not return.
 */
void __noreturn kthread_complete_and_exit(struct completion *comp, long code)
{
	if (comp)
		complete(comp);

	kthread_exit(code);
}
EXPORT_SYMBOL(kthread_complete_and_exit);

static int kthread(void *_create)
{
	static const struct sched_param param = { .sched_priority = 0 };
	/* Copy data: it's on kthread's stack */
	struct kthread_create_info *create = _create;
	int (*threadfn)(void *data) = create->threadfn;
	void *data = create->data;
	struct completion *done;
	struct kthread *self;
	int ret;

	self = to_kthread(current);

	/* Release the structure when caller killed by a fatal signal. */
	done = xchg(&create->done, NULL);
	if (!done) {
		kfree(create->full_name);
		kfree(create);
		kthread_exit(-EINTR);
	}

	self->full_name = create->full_name;
	self->threadfn = threadfn;
	self->data = data;

	/*
	 * The new thread inherited kthreadd's priority and CPU mask. Reset
	 * back to default in case they have been changed.
	 */
	sched_setscheduler_nocheck(current, SCHED_NORMAL, &param);
	set_cpus_allowed_ptr(current, housekeeping_cpumask(HK_TYPE_KTHREAD));

	/* OK, tell user we're spawned, wait for stop or wakeup */
	__set_current_state(TASK_UNINTERRUPTIBLE);
	create->result = current;
	/*
	 * Thread is going to call schedule(), do not preempt it,
	 * or the creator may spend more time in wait_task_inactive().
	 */
	preempt_disable();
	complete(done);
	schedule_preempt_disabled();
	preempt_enable();

	ret = -EINTR;
	if (!test_bit(KTHREAD_SHOULD_STOP, &self->flags)) {
		cgroup_kthread_ready();
		__kthread_parkme(self);
		ret = threadfn(data);
	}
	kthread_exit(ret);
}

/* called from kernel_clone() to get node information for about to be created task */
int tsk_fork_get_node(struct task_struct *tsk)
{
#ifdef CONFIG_NUMA
	if (tsk == kthreadd_task)
		return tsk->pref_node_fork;
#endif
	return NUMA_NO_NODE;
}

static void create_kthread(struct kthread_create_info *create)
{
	int pid;//用于存储新创建线程的 PID

#ifdef CONFIG_NUMA
	current->pref_node_fork = create->node;//设置 NUMA 节点偏好，确保新线程在特定节点上创建
#endif
	/* 设置信号处理上下文，使新线程使用自己的信号处理程序（默认不接收信号） */
	pid = kernel_thread(kthread, create, create->full_name,
			    CLONE_FS | CLONE_FILES | SIGCHLD);//创建新内核线程，并设置CLONE标志
	if (pid < 0) {//检查线程创建是否失败
		/* 如果线程创建请求因致命信号中断，释放相关资源 */
		struct completion *done = xchg(&create->done, NULL);//交换 `done` 指针，防止重复访问

		kfree(create->full_name);//释放线程名称的内存
		if (!done) {//如果 `done` 为空，释放 `create` 结构体本身
			kfree(create);
			return;//直接返回，退出函数
		}
		create->result = ERR_PTR(pid);//将错误指针存入 `result` 字段，供调用者检查
		complete(done);//通知调用者创建已完成（即使失败）
	}
}

static __printf(4, 0)
struct task_struct *__kthread_create_on_node(int (*threadfn)(void *data),
						    void *data, int node,
						    const char namefmt[],
						    va_list args)
{
	DECLARE_COMPLETION_ONSTACK(done);//在栈上声明一个完成变量，用于同步
	struct task_struct *task;//内核线程的任务结构指针
	struct kthread_create_info *create = kmalloc(sizeof(*create),
						     GFP_KERNEL);//为线程创建信息分配内存

	if (!create)
		return ERR_PTR(-ENOMEM);//如果失败，返回内存不足错误指针
	create->threadfn = threadfn;//设置线程函数
	create->data = data;//设置传递给线程函数的数据
	create->node = node;// 设置NUMA节点
	create->done = &done;//关联完成变量
	create->full_name = kvasprintf(GFP_KERNEL, namefmt, args);//格式化线程名称并分配内存
	if (!create->full_name) {
		task = ERR_PTR(-ENOMEM);//如果失败，返回内存不足错误指针
		goto free_create;
	}

	spin_lock(&kthread_create_lock);//获取线程创建的全局自旋锁
	list_add_tail(&create->list, &kthread_create_list);//将创建信息添加到全局创建列表
	spin_unlock(&kthread_create_lock);//释放全局自旋锁

	wake_up_process(kthreadd_task);//唤醒 kthreadd 线程以处理创建请求
	/*
	 * Wait for completion in killable state, for I might be chosen by
	 * the OOM killer while kthreadd is trying to allocate memory for
	 * new kernel thread.
	 * 在 killable 状态下等待完成，以防 OOM killer 在 kthreadd为新线程分配内存时选择当前线程。
	 */
	if (unlikely(wait_for_completion_killable(&done))) {//等待完成，如果被信号中断
		/*
		 * If I was killed by a fatal signal before kthreadd (or new
		 * kernel thread) calls complete(), leave the cleanup of this
		 * structure to that thread.
		 * 如果在kthreadd或新线程调用complete()之前被杀死，将清理工作交给kthreadd或新线程。
		 */
		if (xchg(&create->done, NULL))//清除完成变量，避免多次完成
			return ERR_PTR(-EINTR);//返回被信号中断的错误指针
		/*
		 * kthreadd (or new kernel thread) will call complete()
		 * shortly.
		 * kthreadd 或新线程将很快调用 complete()。
		 */
		wait_for_completion(&done);//再次等待完成
	}
	task = create->result;//获取创建的线程任务结构
free_create:
	kfree(create);//释放创建信息的内存
	return task;//返回创建的线程任务结构
}

/**
 * kthread_create_on_node - create a kthread.
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @node: task and thread structures for the thread are allocated on this node
 * @namefmt: printf-style name for the thread.
 *
 * Description: This helper function creates and names a kernel
 * thread.  The thread will be stopped: use wake_up_process() to start
 * it.  See also kthread_run().  The new thread has SCHED_NORMAL policy and
 * is affine to all CPUs.
 *
 * If thread is going to be bound on a particular cpu, give its node
 * in @node, to get NUMA affinity for kthread stack, or else give NUMA_NO_NODE.
 * When woken, the thread will run @threadfn() with @data as its
 * argument. @threadfn() can either return directly if it is a
 * standalone thread for which no one will call kthread_stop(), or
 * return when 'kthread_should_stop()' is true (which means
 * kthread_stop() has been called).  The return value should be zero
 * or a negative error number; it will be passed to kthread_stop().
 *
 * Returns a task_struct or ERR_PTR(-ENOMEM) or ERR_PTR(-EINTR).
 */
struct task_struct *kthread_create_on_node(int (*threadfn)(void *data),
					   void *data, int node,
					   const char namefmt[],
					   ...)
{
	struct task_struct *task;
	va_list args;

	va_start(args, namefmt);
	task = __kthread_create_on_node(threadfn, data, node, namefmt, args);
	va_end(args);

	return task;
}
EXPORT_SYMBOL(kthread_create_on_node);

static void __kthread_bind_mask(struct task_struct *p, const struct cpumask *mask, unsigned int state)
{
	unsigned long flags;

	if (!wait_task_inactive(p, state)) {
		WARN_ON(1);
		return;
	}

	/* It's safe because the task is inactive. */
	raw_spin_lock_irqsave(&p->pi_lock, flags);
	do_set_cpus_allowed(p, mask);
	p->flags |= PF_NO_SETAFFINITY;
	raw_spin_unlock_irqrestore(&p->pi_lock, flags);
}

static void __kthread_bind(struct task_struct *p, unsigned int cpu, unsigned int state)
{
	__kthread_bind_mask(p, cpumask_of(cpu), state);
}

void kthread_bind_mask(struct task_struct *p, const struct cpumask *mask)
{
	__kthread_bind_mask(p, mask, TASK_UNINTERRUPTIBLE);
}

/**
 * kthread_bind - bind a just-created kthread to a cpu.
 * @p: thread created by kthread_create().
 * @cpu: cpu (might not be online, must be possible) for @k to run on.
 *
 * Description: This function is equivalent to set_cpus_allowed(),
 * except that @cpu doesn't need to be online, and the thread must be
 * stopped (i.e., just returned from kthread_create()).
 */
void kthread_bind(struct task_struct *p, unsigned int cpu)
{
	__kthread_bind(p, cpu, TASK_UNINTERRUPTIBLE);
}
EXPORT_SYMBOL(kthread_bind);

/**
 * kthread_create_on_cpu - Create a cpu bound kthread
 * @threadfn: the function to run until signal_pending(current).
 * @data: data ptr for @threadfn.
 * @cpu: The cpu on which the thread should be bound,
 * @namefmt: printf-style name for the thread. Format is restricted
 *	     to "name.*%u". Code fills in cpu number.
 *
 * Description: This helper function creates and names a kernel thread
 */
struct task_struct *kthread_create_on_cpu(int (*threadfn)(void *data),
					  void *data, unsigned int cpu,
					  const char *namefmt)
{
	struct task_struct *p;

	p = kthread_create_on_node(threadfn, data, cpu_to_node(cpu), namefmt,
				   cpu);
	if (IS_ERR(p))
		return p;
	kthread_bind(p, cpu);
	/* CPU hotplug need to bind once again when unparking the thread. */
	to_kthread(p)->cpu = cpu;
	return p;
}
EXPORT_SYMBOL(kthread_create_on_cpu);

void kthread_set_per_cpu(struct task_struct *k, int cpu)
{
	struct kthread *kthread = to_kthread(k);
	if (!kthread)
		return;

	WARN_ON_ONCE(!(k->flags & PF_NO_SETAFFINITY));

	if (cpu < 0) {
		clear_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
		return;
	}

	kthread->cpu = cpu;
	set_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
}

bool kthread_is_per_cpu(struct task_struct *p)
{
	struct kthread *kthread = __to_kthread(p);
	if (!kthread)
		return false;

	return test_bit(KTHREAD_IS_PER_CPU, &kthread->flags);
}

/**
 * kthread_unpark - unpark a thread created by kthread_create().
 * @k:		thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return false, wakes it, and
 * waits for it to return. If the thread is marked percpu then its
 * bound to the cpu again.
 */
void kthread_unpark(struct task_struct *k)
{
	struct kthread *kthread = to_kthread(k);

	/*
	 * Newly created kthread was parked when the CPU was offline.
	 * The binding was lost and we need to set it again.
	 */
	if (test_bit(KTHREAD_IS_PER_CPU, &kthread->flags))
		__kthread_bind(k, kthread->cpu, TASK_PARKED);

	clear_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	/*
	 * __kthread_parkme() will either see !SHOULD_PARK or get the wakeup.
	 */
	wake_up_state(k, TASK_PARKED);
}
EXPORT_SYMBOL_GPL(kthread_unpark);

/**
 * kthread_park - park a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_park() for @k to return true, wakes it, and
 * waits for it to return. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will park without
 * calling threadfn().
 *
 * Returns 0 if the thread is parked, -ENOSYS if the thread exited.
 * If called by the kthread itself just the park bit is set.
 */
int kthread_park(struct task_struct *k)
{
	struct kthread *kthread = to_kthread(k);

	if (WARN_ON(k->flags & PF_EXITING))
		return -ENOSYS;

	if (WARN_ON_ONCE(test_bit(KTHREAD_SHOULD_PARK, &kthread->flags)))
		return -EBUSY;

	set_bit(KTHREAD_SHOULD_PARK, &kthread->flags);
	if (k != current) {
		wake_up_process(k);
		/*
		 * Wait for __kthread_parkme() to complete(), this means we
		 * _will_ have TASK_PARKED and are about to call schedule().
		 */
		wait_for_completion(&kthread->parked);
		/*
		 * Now wait for that schedule() to complete and the task to
		 * get scheduled out.
		 */
		WARN_ON_ONCE(!wait_task_inactive(k, TASK_PARKED));
	}

	return 0;
}
EXPORT_SYMBOL_GPL(kthread_park);

/**
 * kthread_stop - stop a thread created by kthread_create().
 * @k: thread created by kthread_create().
 *
 * Sets kthread_should_stop() for @k to return true, wakes it, and
 * waits for it to exit. This can also be called after kthread_create()
 * instead of calling wake_up_process(): the thread will exit without
 * calling threadfn().
 *
 * If threadfn() may call kthread_exit() itself, the caller must ensure
 * task_struct can't go away.
 *
 * Returns the result of threadfn(), or %-EINTR if wake_up_process()
 * was never called.
 */
int kthread_stop(struct task_struct *k)
{
	struct kthread *kthread;
	int ret;

	trace_sched_kthread_stop(k);

	get_task_struct(k);
	kthread = to_kthread(k);
	set_bit(KTHREAD_SHOULD_STOP, &kthread->flags);
	kthread_unpark(k);
	set_tsk_thread_flag(k, TIF_NOTIFY_SIGNAL);
	wake_up_process(k);
	wait_for_completion(&kthread->exited);
	ret = kthread->result;
	put_task_struct(k);

	trace_sched_kthread_stop_ret(ret);
	return ret;
}
EXPORT_SYMBOL(kthread_stop);

/**
 * kthread_stop_put - stop a thread and put its task struct
 * @k: thread created by kthread_create().
 *
 * Stops a thread created by kthread_create() and put its task_struct.
 * Only use when holding an extra task struct reference obtained by
 * calling get_task_struct().
 */
int kthread_stop_put(struct task_struct *k)
{
	int ret;

	ret = kthread_stop(k);
	put_task_struct(k);
	return ret;
}
EXPORT_SYMBOL(kthread_stop_put);

int kthreadd(void *unused)//内核线程管理器线程主函数,用于监听线程创建请求并生成其他内核线程
{
	struct task_struct *tsk = current;//获取当前线程的 task_struct

	/* 设置线程的上下文环境，供其创建的子线程继承 */
	set_task_comm(tsk, "kthreadd");//设置线程名称为 kthreadd
	ignore_signals(tsk);//忽略所有信号，防止被意外终止
	set_cpus_allowed_ptr(tsk, housekeeping_cpumask(HK_TYPE_KTHREAD));//将线程绑定到 housekeeping CPU，以保证在特定 CPU 上运行
	set_mems_allowed(node_states[N_MEMORY]);//设置线程允许访问的内存节点范围，确保线程能访问内存资源

	current->flags |= PF_NOFREEZE;//设置不可冻结标志，确保该线程在系统冻结时仍然运行
	cgroup_init_kthreadd();//初始化 cgroup，将 kthreadd 线程加入到适当的控制组中

	for (;;) {//无限循环，负责监听创建请求并生成新线程
		set_current_state(TASK_INTERRUPTIBLE);//将线程设置为可中断睡眠状态，等待新的任务请求
		if (list_empty(&kthread_create_list))//检查线程创建请求列表是否为空
			schedule();//让出 CPU 资源，使线程进入睡眠状态，等待唤醒
		__set_current_state(TASK_RUNNING);//线程被唤醒后，设置为运行状态

		spin_lock(&kthread_create_lock);//获取自旋锁，保护创建请求列表的访问
		while (!list_empty(&kthread_create_list)) {//遍历创建请求列表，处理每个请求
			struct kthread_create_info *create;//用于存储当前请求的信息结构体

			create = list_entry(kthread_create_list.next,
					    struct kthread_create_info, list);//获取列表中的第一个创建请求
			list_del_init(&create->list);//将请求节点从列表中删除，并重新初始化节点
			spin_unlock(&kthread_create_lock);//释放自旋锁，允许其他线程访问创建列表

			create_kthread(create);//调用函数处理当前请求，创建对应的内核线程

			spin_lock(&kthread_create_lock);//释放自旋锁，完成本轮请求处理
		}
		spin_unlock(&kthread_create_lock);//永远不会到达此处，因为 for 循环不会退出
	}

	return 0;
}

void __kthread_init_worker(struct kthread_worker *worker,
				const char *name,
				struct lock_class_key *key)
{
	memset(worker, 0, sizeof(struct kthread_worker));//清零工作者结构体以初始化所有字段
	raw_spin_lock_init(&worker->lock);//初始化原始自旋锁，用于同步工作队列的操作
	lockdep_set_class_and_name(&worker->lock, key, name);//为锁分配类和名称，用于锁依赖关系检测
	INIT_LIST_HEAD(&worker->work_list);//初始化工作队列的链表头
	INIT_LIST_HEAD(&worker->delayed_work_list);//初始化延迟工作队列的链表头
}
EXPORT_SYMBOL_GPL(__kthread_init_worker);

/**
 * kthread_worker_fn - kthread function to process kthread_worker
 * @worker_ptr: pointer to initialized kthread_worker
 *
 * This function implements the main cycle of kthread worker. It processes
 * work_list until it is stopped with kthread_stop(). It sleeps when the queue
 * is empty.
 *
 * The works are not allowed to keep any locks, disable preemption or interrupts
 * when they finish. There is defined a safe point for freezing when one work
 * finishes and before a new one is started.
 *
 * Also the works must not be handled by more than one worker at the same time,
 * see also kthread_queue_work().
 */
int kthread_worker_fn(void *worker_ptr)//内核线程工作者的主函数,负责从工作列表中提取工作并调用相应的处理函数
{
	struct kthread_worker *worker = worker_ptr;//获取传递的工作者结构体指针
	struct kthread_work *work;//指向当前要处理的工作

	/*
	 * FIXME: Update the check and remove the assignment when all kthread
	 * worker users are created using kthread_create_worker*() functions.
	 */
	WARN_ON(worker->task && worker->task != current);//检查当前线程是否已正确关联到工作者
	worker->task = current;//将当前线程与工作者结构体关联

	if (worker->flags & KTW_FREEZABLE)//如果工作者允许冻结
		set_freezable();//设置当前线程为可冻结状态

repeat:
	set_current_state(TASK_INTERRUPTIBLE);//将线程设置为可中断睡眠状态

	if (kthread_should_stop()) {//检查线程是否应该停止
		__set_current_state(TASK_RUNNING);//将线程状态设置为运行状态
		raw_spin_lock_irq(&worker->lock);//获取工作者的自旋锁
		worker->task = NULL;//将工作者的任务指针清空
		raw_spin_unlock_irq(&worker->lock);//释放自旋锁
		return 0;//返回，结束线程执行
	}

	work = NULL;//初始化工作指针为空
	raw_spin_lock_irq(&worker->lock);// 获取工作者的自旋锁
	if (!list_empty(&worker->work_list)) {//如果工作列表非空
		work = list_first_entry(&worker->work_list,
					struct kthread_work, node);//获取第一个工作
		list_del_init(&work->node);//从列表中移除该工作，并重新初始化节点
	}
	worker->current_work = work;//设置当前处理的工作
	raw_spin_unlock_irq(&worker->lock);//释放自旋锁

	if (work) {//如果有工作需要处理
		kthread_work_func_t func = work->func;//获取工作的处理函数
		__set_current_state(TASK_RUNNING);//将线程状态设置为运行
		trace_sched_kthread_work_execute_start(work);//记录工作开始执行的跟踪信息
		work->func(work);//调用工作函数处理工作
		/*
		 * Avoid dereferencing work after this point.  The trace
		 * event only cares about the address.
		 */
		trace_sched_kthread_work_execute_end(work, func);// 记录工作执行结束的跟踪信息
	} else if (!freezing(current))//如果没有工作且线程未被冻结
		schedule();//调度线程，释放 CPU

	try_to_freeze();//检查并冻结线程（如果需要）
	cond_resched();//检查是否需要主动调度
	goto repeat;//继续循环，处理下一个工作
}
EXPORT_SYMBOL_GPL(kthread_worker_fn);

static __printf(3, 0) struct kthread_worker *
__kthread_create_worker(int cpu, unsigned int flags,
			const char namefmt[], va_list args)
{
	struct kthread_worker *worker;//用于存储创建的工作者对象
	struct task_struct *task;//用于存储关联的内核线程任务
	int node = NUMA_NO_NODE;//默认未指定的 NUMA 节点

	worker = kzalloc(sizeof(*worker), GFP_KERNEL);//分配并清零工作者对象内存
	if (!worker)
		return ERR_PTR(-ENOMEM);

	kthread_init_worker(worker);//初始化工作者对象

	if (cpu >= 0)
		node = cpu_to_node(cpu);//如果指定CPU,获取该 CPU 所属的 NUMA 节点

	task = __kthread_create_on_node(kthread_worker_fn, worker,
						node, namefmt, args);//创建并绑定到指定节点的内核工作者线程
	if (IS_ERR(task))
		goto fail_task;//如果失败，跳转到错误处理逻辑

	if (cpu >= 0)
		kthread_bind(task, cpu);//如果指定CPU,将线程绑定到指定的 CPU

	worker->flags = flags;//设置工作者的标志位
	worker->task = task;//关联创建的内核线程任务
	wake_up_process(task);//唤醒线程开始运行
	return worker;//返回创建的工作者对象

fail_task:
	kfree(worker);//释放已分配的工作者对象内存
	return ERR_CAST(task);//返回与错误任务指针关联的错误码
}

/**
 * kthread_create_worker - create a kthread worker
 * @flags: flags modifying the default behavior of the worker
 * @namefmt: printf-style name for the kthread worker (task).
 *
 * Returns a pointer to the allocated worker on success, ERR_PTR(-ENOMEM)
 * when the needed structures could not get allocated, and ERR_PTR(-EINTR)
 * when the caller was killed by a fatal signal.
 */
struct kthread_worker *
kthread_create_worker(unsigned int flags, const char namefmt[], ...)
{
	struct kthread_worker *worker;//用于存储创建的内核线程工作者指针
	va_list args;//变参列表，用于格式化线程名称

	va_start(args, namefmt);//初始化变参列表，开始处理可变参数
	worker = __kthread_create_worker(-1, flags, namefmt, args);//调用底层函数创建工作者线程
	va_end(args);// 结束变参处理

	return worker;
}
EXPORT_SYMBOL(kthread_create_worker);

/**
 * kthread_create_worker_on_cpu - create a kthread worker and bind it
 *	to a given CPU and the associated NUMA node.
 * @cpu: CPU number
 * @flags: flags modifying the default behavior of the worker
 * @namefmt: printf-style name for the kthread worker (task).
 *
 * Use a valid CPU number if you want to bind the kthread worker
 * to the given CPU and the associated NUMA node.
 *
 * A good practice is to add the cpu number also into the worker name.
 * For example, use kthread_create_worker_on_cpu(cpu, "helper/%d", cpu).
 *
 * CPU hotplug:
 * The kthread worker API is simple and generic. It just provides a way
 * to create, use, and destroy workers.
 *
 * It is up to the API user how to handle CPU hotplug. They have to decide
 * how to handle pending work items, prevent queuing new ones, and
 * restore the functionality when the CPU goes off and on. There are a
 * few catches:
 *
 *    - CPU affinity gets lost when it is scheduled on an offline CPU.
 *
 *    - The worker might not exist when the CPU was off when the user
 *      created the workers.
 *
 * Good practice is to implement two CPU hotplug callbacks and to
 * destroy/create the worker when the CPU goes down/up.
 *
 * Return:
 * The pointer to the allocated worker on success, ERR_PTR(-ENOMEM)
 * when the needed structures could not get allocated, and ERR_PTR(-EINTR)
 * when the caller was killed by a fatal signal.
 */
struct kthread_worker *
kthread_create_worker_on_cpu(int cpu, unsigned int flags,
			     const char namefmt[], ...)
{
	struct kthread_worker *worker;
	va_list args;

	va_start(args, namefmt);
	worker = __kthread_create_worker(cpu, flags, namefmt, args);
	va_end(args);

	return worker;
}
EXPORT_SYMBOL(kthread_create_worker_on_cpu);

/*
 * Returns true when the work could not be queued at the moment.
 * It happens when it is already pending in a worker list
 * or when it is being cancelled.
 */
static inline bool queuing_blocked(struct kthread_worker *worker,
				   struct kthread_work *work)
{
	lockdep_assert_held(&worker->lock);

	return !list_empty(&work->node) || work->canceling;
}

static void kthread_insert_work_sanity_check(struct kthread_worker *worker,
					     struct kthread_work *work)
{
	lockdep_assert_held(&worker->lock);
	WARN_ON_ONCE(!list_empty(&work->node));
	/* Do not use a work with >1 worker, see kthread_queue_work() */
	WARN_ON_ONCE(work->worker && work->worker != worker);
}

/* insert @work before @pos in @worker */
static void kthread_insert_work(struct kthread_worker *worker,
				struct kthread_work *work,
				struct list_head *pos)
{
	kthread_insert_work_sanity_check(worker, work);

	trace_sched_kthread_work_queue_work(worker, work);

	list_add_tail(&work->node, pos);
	work->worker = worker;
	if (!worker->current_work && likely(worker->task))
		wake_up_process(worker->task);
}

/**
 * kthread_queue_work - queue a kthread_work
 * @worker: target kthread_worker
 * @work: kthread_work to queue
 *
 * Queue @work to work processor @task for async execution.  @task
 * must have been created with kthread_worker_create().  Returns %true
 * if @work was successfully queued, %false if it was already pending.
 *
 * Reinitialize the work if it needs to be used by another worker.
 * For example, when the worker was stopped and started again.
 */
bool kthread_queue_work(struct kthread_worker *worker,
			struct kthread_work *work)
{
	bool ret = false;
	unsigned long flags;

	raw_spin_lock_irqsave(&worker->lock, flags);
	if (!queuing_blocked(worker, work)) {
		kthread_insert_work(worker, work, &worker->work_list);
		ret = true;
	}
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_queue_work);

/**
 * kthread_delayed_work_timer_fn - callback that queues the associated kthread
 *	delayed work when the timer expires.
 * @t: pointer to the expired timer
 *
 * The format of the function is defined by struct timer_list.
 * It should have been called from irqsafe timer with irq already off.
 */
void kthread_delayed_work_timer_fn(struct timer_list *t)
{
	struct kthread_delayed_work *dwork = from_timer(dwork, t, timer);
	struct kthread_work *work = &dwork->work;
	struct kthread_worker *worker = work->worker;
	unsigned long flags;

	/*
	 * This might happen when a pending work is reinitialized.
	 * It means that it is used a wrong way.
	 */
	if (WARN_ON_ONCE(!worker))
		return;

	raw_spin_lock_irqsave(&worker->lock, flags);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	WARN_ON_ONCE(work->worker != worker);

	/* Move the work from worker->delayed_work_list. */
	WARN_ON_ONCE(list_empty(&work->node));
	list_del_init(&work->node);
	if (!work->canceling)
		kthread_insert_work(worker, work, &worker->work_list);

	raw_spin_unlock_irqrestore(&worker->lock, flags);
}
EXPORT_SYMBOL(kthread_delayed_work_timer_fn);

static void __kthread_queue_delayed_work(struct kthread_worker *worker,
					 struct kthread_delayed_work *dwork,
					 unsigned long delay)
{
	struct timer_list *timer = &dwork->timer;
	struct kthread_work *work = &dwork->work;

	WARN_ON_ONCE(timer->function != kthread_delayed_work_timer_fn);

	/*
	 * If @delay is 0, queue @dwork->work immediately.  This is for
	 * both optimization and correctness.  The earliest @timer can
	 * expire is on the closest next tick and delayed_work users depend
	 * on that there's no such delay when @delay is 0.
	 */
	if (!delay) {
		kthread_insert_work(worker, work, &worker->work_list);
		return;
	}

	/* Be paranoid and try to detect possible races already now. */
	kthread_insert_work_sanity_check(worker, work);

	list_add(&work->node, &worker->delayed_work_list);
	work->worker = worker;
	timer->expires = jiffies + delay;
	add_timer(timer);
}

/**
 * kthread_queue_delayed_work - queue the associated kthread work
 *	after a delay.
 * @worker: target kthread_worker
 * @dwork: kthread_delayed_work to queue
 * @delay: number of jiffies to wait before queuing
 *
 * If the work has not been pending it starts a timer that will queue
 * the work after the given @delay. If @delay is zero, it queues the
 * work immediately.
 *
 * Return: %false if the @work has already been pending. It means that
 * either the timer was running or the work was queued. It returns %true
 * otherwise.
 */
bool kthread_queue_delayed_work(struct kthread_worker *worker,
				struct kthread_delayed_work *dwork,
				unsigned long delay)
{
	struct kthread_work *work = &dwork->work;
	unsigned long flags;
	bool ret = false;

	raw_spin_lock_irqsave(&worker->lock, flags);

	if (!queuing_blocked(worker, work)) {
		__kthread_queue_delayed_work(worker, dwork, delay);
		ret = true;
	}

	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_queue_delayed_work);

struct kthread_flush_work {
	struct kthread_work	work;
	struct completion	done;
};

static void kthread_flush_work_fn(struct kthread_work *work)
{
	struct kthread_flush_work *fwork =
		container_of(work, struct kthread_flush_work, work);
	complete(&fwork->done);
}

/**
 * kthread_flush_work - flush a kthread_work
 * @work: work to flush
 *
 * If @work is queued or executing, wait for it to finish execution.
 */
void kthread_flush_work(struct kthread_work *work)
{
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};
	struct kthread_worker *worker;
	bool noop = false;

	worker = work->worker;
	if (!worker)
		return;

	raw_spin_lock_irq(&worker->lock);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	WARN_ON_ONCE(work->worker != worker);

	if (!list_empty(&work->node))
		kthread_insert_work(worker, &fwork.work, work->node.next);
	else if (worker->current_work == work)
		kthread_insert_work(worker, &fwork.work,
				    worker->work_list.next);
	else
		noop = true;

	raw_spin_unlock_irq(&worker->lock);

	if (!noop)
		wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(kthread_flush_work);

/*
 * Make sure that the timer is neither set nor running and could
 * not manipulate the work list_head any longer.
 *
 * The function is called under worker->lock. The lock is temporary
 * released but the timer can't be set again in the meantime.
 */
static void kthread_cancel_delayed_work_timer(struct kthread_work *work,
					      unsigned long *flags)
{
	struct kthread_delayed_work *dwork =
		container_of(work, struct kthread_delayed_work, work);
	struct kthread_worker *worker = work->worker;

	/*
	 * del_timer_sync() must be called to make sure that the timer
	 * callback is not running. The lock must be temporary released
	 * to avoid a deadlock with the callback. In the meantime,
	 * any queuing is blocked by setting the canceling counter.
	 */
	work->canceling++;
	raw_spin_unlock_irqrestore(&worker->lock, *flags);
	del_timer_sync(&dwork->timer);
	raw_spin_lock_irqsave(&worker->lock, *flags);
	work->canceling--;
}

/*
 * This function removes the work from the worker queue.
 *
 * It is called under worker->lock. The caller must make sure that
 * the timer used by delayed work is not running, e.g. by calling
 * kthread_cancel_delayed_work_timer().
 *
 * The work might still be in use when this function finishes. See the
 * current_work proceed by the worker.
 *
 * Return: %true if @work was pending and successfully canceled,
 *	%false if @work was not pending
 */
static bool __kthread_cancel_work(struct kthread_work *work)
{
	/*
	 * Try to remove the work from a worker list. It might either
	 * be from worker->work_list or from worker->delayed_work_list.
	 */
	if (!list_empty(&work->node)) {
		list_del_init(&work->node);
		return true;
	}

	return false;
}

/**
 * kthread_mod_delayed_work - modify delay of or queue a kthread delayed work
 * @worker: kthread worker to use
 * @dwork: kthread delayed work to queue
 * @delay: number of jiffies to wait before queuing
 *
 * If @dwork is idle, equivalent to kthread_queue_delayed_work(). Otherwise,
 * modify @dwork's timer so that it expires after @delay. If @delay is zero,
 * @work is guaranteed to be queued immediately.
 *
 * Return: %false if @dwork was idle and queued, %true otherwise.
 *
 * A special case is when the work is being canceled in parallel.
 * It might be caused either by the real kthread_cancel_delayed_work_sync()
 * or yet another kthread_mod_delayed_work() call. We let the other command
 * win and return %true here. The return value can be used for reference
 * counting and the number of queued works stays the same. Anyway, the caller
 * is supposed to synchronize these operations a reasonable way.
 *
 * This function is safe to call from any context including IRQ handler.
 * See __kthread_cancel_work() and kthread_delayed_work_timer_fn()
 * for details.
 */
bool kthread_mod_delayed_work(struct kthread_worker *worker,
			      struct kthread_delayed_work *dwork,
			      unsigned long delay)
{
	struct kthread_work *work = &dwork->work;
	unsigned long flags;
	int ret;

	raw_spin_lock_irqsave(&worker->lock, flags);

	/* Do not bother with canceling when never queued. */
	if (!work->worker) {
		ret = false;
		goto fast_queue;
	}

	/* Work must not be used with >1 worker, see kthread_queue_work() */
	WARN_ON_ONCE(work->worker != worker);

	/*
	 * Temporary cancel the work but do not fight with another command
	 * that is canceling the work as well.
	 *
	 * It is a bit tricky because of possible races with another
	 * mod_delayed_work() and cancel_delayed_work() callers.
	 *
	 * The timer must be canceled first because worker->lock is released
	 * when doing so. But the work can be removed from the queue (list)
	 * only when it can be queued again so that the return value can
	 * be used for reference counting.
	 */
	kthread_cancel_delayed_work_timer(work, &flags);
	if (work->canceling) {
		/* The number of works in the queue does not change. */
		ret = true;
		goto out;
	}
	ret = __kthread_cancel_work(work);

fast_queue:
	__kthread_queue_delayed_work(worker, dwork, delay);
out:
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	return ret;
}
EXPORT_SYMBOL_GPL(kthread_mod_delayed_work);

static bool __kthread_cancel_work_sync(struct kthread_work *work, bool is_dwork)
{
	struct kthread_worker *worker = work->worker;
	unsigned long flags;
	int ret = false;

	if (!worker)
		goto out;

	raw_spin_lock_irqsave(&worker->lock, flags);
	/* Work must not be used with >1 worker, see kthread_queue_work(). */
	WARN_ON_ONCE(work->worker != worker);

	if (is_dwork)
		kthread_cancel_delayed_work_timer(work, &flags);

	ret = __kthread_cancel_work(work);

	if (worker->current_work != work)
		goto out_fast;

	/*
	 * The work is in progress and we need to wait with the lock released.
	 * In the meantime, block any queuing by setting the canceling counter.
	 */
	work->canceling++;
	raw_spin_unlock_irqrestore(&worker->lock, flags);
	kthread_flush_work(work);
	raw_spin_lock_irqsave(&worker->lock, flags);
	work->canceling--;

out_fast:
	raw_spin_unlock_irqrestore(&worker->lock, flags);
out:
	return ret;
}

/**
 * kthread_cancel_work_sync - cancel a kthread work and wait for it to finish
 * @work: the kthread work to cancel
 *
 * Cancel @work and wait for its execution to finish.  This function
 * can be used even if the work re-queues itself. On return from this
 * function, @work is guaranteed to be not pending or executing on any CPU.
 *
 * kthread_cancel_work_sync(&delayed_work->work) must not be used for
 * delayed_work's. Use kthread_cancel_delayed_work_sync() instead.
 *
 * The caller must ensure that the worker on which @work was last
 * queued can't be destroyed before this function returns.
 *
 * Return: %true if @work was pending, %false otherwise.
 */
bool kthread_cancel_work_sync(struct kthread_work *work)
{
	return __kthread_cancel_work_sync(work, false);
}
EXPORT_SYMBOL_GPL(kthread_cancel_work_sync);

/**
 * kthread_cancel_delayed_work_sync - cancel a kthread delayed work and
 *	wait for it to finish.
 * @dwork: the kthread delayed work to cancel
 *
 * This is kthread_cancel_work_sync() for delayed works.
 *
 * Return: %true if @dwork was pending, %false otherwise.
 */
bool kthread_cancel_delayed_work_sync(struct kthread_delayed_work *dwork)
{
	return __kthread_cancel_work_sync(&dwork->work, true);
}
EXPORT_SYMBOL_GPL(kthread_cancel_delayed_work_sync);

/**
 * kthread_flush_worker - flush all current works on a kthread_worker
 * @worker: worker to flush
 *
 * Wait until all currently executing or pending works on @worker are
 * finished.
 */
void kthread_flush_worker(struct kthread_worker *worker)
{
	struct kthread_flush_work fwork = {
		KTHREAD_WORK_INIT(fwork.work, kthread_flush_work_fn),
		COMPLETION_INITIALIZER_ONSTACK(fwork.done),
	};

	kthread_queue_work(worker, &fwork.work);
	wait_for_completion(&fwork.done);
}
EXPORT_SYMBOL_GPL(kthread_flush_worker);

/**
 * kthread_destroy_worker - destroy a kthread worker
 * @worker: worker to be destroyed
 *
 * Flush and destroy @worker.  The simple flush is enough because the kthread
 * worker API is used only in trivial scenarios.  There are no multi-step state
 * machines needed.
 *
 * Note that this function is not responsible for handling delayed work, so
 * caller should be responsible for queuing or canceling all delayed work items
 * before invoke this function.
 */
void kthread_destroy_worker(struct kthread_worker *worker)
{
	struct task_struct *task;

	task = worker->task;
	if (WARN_ON(!task))
		return;

	kthread_flush_worker(worker);
	kthread_stop(task);
	WARN_ON(!list_empty(&worker->delayed_work_list));
	WARN_ON(!list_empty(&worker->work_list));
	kfree(worker);
}
EXPORT_SYMBOL(kthread_destroy_worker);

/**
 * kthread_use_mm - make the calling kthread operate on an address space
 * @mm: address space to operate on
 */
void kthread_use_mm(struct mm_struct *mm)
{
	struct mm_struct *active_mm;
	struct task_struct *tsk = current;

	WARN_ON_ONCE(!(tsk->flags & PF_KTHREAD));
	WARN_ON_ONCE(tsk->mm);

	/*
	 * It is possible for mm to be the same as tsk->active_mm, but
	 * we must still mmgrab(mm) and mmdrop_lazy_tlb(active_mm),
	 * because these references are not equivalent.
	 */
	mmgrab(mm);

	task_lock(tsk);
	/* Hold off tlb flush IPIs while switching mm's */
	local_irq_disable();
	active_mm = tsk->active_mm;
	tsk->active_mm = mm;
	tsk->mm = mm;
	membarrier_update_current_mm(mm);
	switch_mm_irqs_off(active_mm, mm, tsk);
	local_irq_enable();
	task_unlock(tsk);
#ifdef finish_arch_post_lock_switch
	finish_arch_post_lock_switch();
#endif

	/*
	 * When a kthread starts operating on an address space, the loop
	 * in membarrier_{private,global}_expedited() may not observe
	 * that tsk->mm, and not issue an IPI. Membarrier requires a
	 * memory barrier after storing to tsk->mm, before accessing
	 * user-space memory. A full memory barrier for membarrier
	 * {PRIVATE,GLOBAL}_EXPEDITED is implicitly provided by
	 * mmdrop_lazy_tlb().
	 */
	mmdrop_lazy_tlb(active_mm);
}
EXPORT_SYMBOL_GPL(kthread_use_mm);

/**
 * kthread_unuse_mm - reverse the effect of kthread_use_mm()
 * @mm: address space to operate on
 */
void kthread_unuse_mm(struct mm_struct *mm)
{
	struct task_struct *tsk = current;

	WARN_ON_ONCE(!(tsk->flags & PF_KTHREAD));
	WARN_ON_ONCE(!tsk->mm);

	task_lock(tsk);
	/*
	 * When a kthread stops operating on an address space, the loop
	 * in membarrier_{private,global}_expedited() may not observe
	 * that tsk->mm, and not issue an IPI. Membarrier requires a
	 * memory barrier after accessing user-space memory, before
	 * clearing tsk->mm.
	 */
	smp_mb__after_spinlock();
	local_irq_disable();
	tsk->mm = NULL;
	membarrier_update_current_mm(NULL);
	mmgrab_lazy_tlb(mm);
	/* active_mm is still 'mm' */
	enter_lazy_tlb(mm, tsk);
	local_irq_enable();
	task_unlock(tsk);

	mmdrop(mm);
}
EXPORT_SYMBOL_GPL(kthread_unuse_mm);

#ifdef CONFIG_BLK_CGROUP
/**
 * kthread_associate_blkcg - associate blkcg to current kthread
 * @css: the cgroup info
 *
 * Current thread must be a kthread. The thread is running jobs on behalf of
 * other threads. In some cases, we expect the jobs attach cgroup info of
 * original threads instead of that of current thread. This function stores
 * original thread's cgroup info in current kthread context for later
 * retrieval.
 */
void kthread_associate_blkcg(struct cgroup_subsys_state *css)
{
	struct kthread *kthread;

	if (!(current->flags & PF_KTHREAD))
		return;
	kthread = to_kthread(current);
	if (!kthread)
		return;

	if (kthread->blkcg_css) {
		css_put(kthread->blkcg_css);
		kthread->blkcg_css = NULL;
	}
	if (css) {
		css_get(css);
		kthread->blkcg_css = css;
	}
}
EXPORT_SYMBOL(kthread_associate_blkcg);

/**
 * kthread_blkcg - get associated blkcg css of current kthread
 *
 * Current thread must be a kthread.
 */
struct cgroup_subsys_state *kthread_blkcg(void)
{
	struct kthread *kthread;

	if (current->flags & PF_KTHREAD) {
		kthread = to_kthread(current);
		if (kthread)
			return kthread->blkcg_css;
	}
	return NULL;
}
#endif
