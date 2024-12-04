// SPDX-License-Identifier: GPL-2.0-only
/*
 * Common SMP CPU bringup/teardown functions
 */
#include <linux/cpu.h>
#include <linux/err.h>
#include <linux/smp.h>
#include <linux/delay.h>
#include <linux/init.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/export.h>
#include <linux/percpu.h>
#include <linux/kthread.h>
#include <linux/smpboot.h>

#include "smpboot.h"

#ifdef CONFIG_SMP

#ifdef CONFIG_GENERIC_SMP_IDLE_THREAD
/*
 * For the hotplug case we keep the task structs around and reuse
 * them.
 */
static DEFINE_PER_CPU(struct task_struct *, idle_threads);

struct task_struct *idle_thread_get(unsigned int cpu)
{
	struct task_struct *tsk = per_cpu(idle_threads, cpu);

	if (!tsk)
		return ERR_PTR(-ENOMEM);
	return tsk;
}

void __init idle_thread_set_boot_cpu(void)
{
	per_cpu(idle_threads, smp_processor_id()) = current;
}

/**
 * idle_init - Initialize the idle thread for a cpu
 * @cpu:	The cpu for which the idle thread should be initialized
 *
 * Creates the thread if it does not exist.
 */
static __always_inline void idle_init(unsigned int cpu)//用于为指定的 CPU 初始化空闲线程。确保每个 CPU 都有一个空闲线程用于在没有任务执行时运行
{
	struct task_struct *tsk = per_cpu(idle_threads, cpu);//获取指定 CPU 的空闲线程任务结构体指针

	if (!tsk) {//如果该 CPU 的空闲线程任务尚未初始化
		tsk = fork_idle(cpu);//调用 fork_idle 为该 CPU 创建一个新的空闲线程
		if (IS_ERR(tsk))
			pr_err("SMP: fork_idle() failed for CPU %u\n", cpu);//创建失败，输出错误信息
		else
			per_cpu(idle_threads, cpu) = tsk;//创建成功，将该空闲线程的任务结构体指针保存到对应的 CPU 数据中
	}
}

/**
 * idle_threads_init - Initialize idle threads for all cpus
 */
void __init idle_threads_init(void)//在多核系统中初始化每个CPU的空闲线程
{
	unsigned int cpu, boot_cpu;

	boot_cpu = smp_processor_id();//获取当前引导 CPU 的 ID

	for_each_possible_cpu(cpu) {//遍历系统中所有可用的 CPU
		if (cpu != boot_cpu)//如果当前 CPU 不是引导 CPU
			idle_init(cpu);//为该 CPU 初始化空闲线程
	}
}
#endif

#endif /* #ifdef CONFIG_SMP */

static LIST_HEAD(hotplug_threads);
static DEFINE_MUTEX(smpboot_threads_lock);

struct smpboot_thread_data {
	unsigned int			cpu;
	unsigned int			status;
	struct smp_hotplug_thread	*ht;
};

enum {
	HP_THREAD_NONE = 0,
	HP_THREAD_ACTIVE,
	HP_THREAD_PARKED,
};

/**
 * smpboot_thread_fn - percpu hotplug thread loop function
 * @data:	thread data pointer
 *
 * Checks for thread stop and park conditions. Calls the necessary
 * setup, cleanup, park and unpark functions for the registered
 * thread.
 *
 * Returns 1 when the thread should exit, 0 otherwise.
 */
static int smpboot_thread_fn(void *data)
{
	struct smpboot_thread_data *td = data;
	struct smp_hotplug_thread *ht = td->ht;

	while (1) {
		set_current_state(TASK_INTERRUPTIBLE);
		preempt_disable();
		if (kthread_should_stop()) {
			__set_current_state(TASK_RUNNING);
			preempt_enable();
			/* cleanup must mirror setup */
			if (ht->cleanup && td->status != HP_THREAD_NONE)
				ht->cleanup(td->cpu, cpu_online(td->cpu));
			kfree(td);
			return 0;
		}

		if (kthread_should_park()) {
			__set_current_state(TASK_RUNNING);
			preempt_enable();
			if (ht->park && td->status == HP_THREAD_ACTIVE) {
				BUG_ON(td->cpu != smp_processor_id());
				ht->park(td->cpu);
				td->status = HP_THREAD_PARKED;
			}
			kthread_parkme();
			/* We might have been woken for stop */
			continue;
		}

		BUG_ON(td->cpu != smp_processor_id());

		/* Check for state change setup */
		switch (td->status) {
		case HP_THREAD_NONE:
			__set_current_state(TASK_RUNNING);
			preempt_enable();
			if (ht->setup)
				ht->setup(td->cpu);
			td->status = HP_THREAD_ACTIVE;
			continue;

		case HP_THREAD_PARKED:
			__set_current_state(TASK_RUNNING);
			preempt_enable();
			if (ht->unpark)
				ht->unpark(td->cpu);
			td->status = HP_THREAD_ACTIVE;
			continue;
		}

		if (!ht->thread_should_run(td->cpu)) {
			preempt_enable_no_resched();
			schedule();
		} else {
			__set_current_state(TASK_RUNNING);
			preempt_enable();
			ht->thread_fn(td->cpu);
		}
	}
}

static int
__smpboot_create_thread(struct smp_hotplug_thread *ht, unsigned int cpu)//为指定的 CPU 创建并初始化一个热插拔线程。
{
	struct task_struct *tsk = *per_cpu_ptr(ht->store, cpu);//获取与指定 CPU 相关联的任务结构体指针
	struct smpboot_thread_data *td;//定义指向 smpboot 线程数据的指针

	if (tsk)//如果任务结构体已经存在，说明线程已创建，直接返回 0
		return 0;

	td = kzalloc_node(sizeof(*td), GFP_KERNEL, cpu_to_node(cpu));//为线程数据分配内存，并将其绑定到指定的 CPU 节点
	if (!td)
		return -ENOMEM;//分配失败，返回错误码，表示内存不足
	td->cpu = cpu;//设置线程数据中的 CPU ID
	td->ht = ht;//设置线程数据中的热插拔线程指针

	tsk = kthread_create_on_cpu(smpboot_thread_fn, td, cpu,
				    ht->thread_comm);//在指定的 CPU 上创建内核线程，并将线程数据和命令名称传递给它
	if (IS_ERR(tsk)) {// 如果线程创建失败
		kfree(td);//释放已分配的线程数据内存
		return PTR_ERR(tsk);//返回错误码，表示线程创建失败
	}
	kthread_set_per_cpu(tsk, cpu);//将线程标记为与指定的 CPU 相关联
	/*
	 * Park the thread so that it could start right on the CPU
	 * when it is available.
	 */
	kthread_park(tsk);//将线程置于停放状态，等待被解除停放
	get_task_struct(tsk);//增加线程的引用计数，确保线程在被使用期间不会被释放
	*per_cpu_ptr(ht->store, cpu) = tsk;//将线程结构体指针存储在指定 CPU 的存储位置中
	if (ht->create) {//如果存在创建回调函数
		/*
		 * Make sure that the task has actually scheduled out
		 * into park position, before calling the create
		 * callback. At least the migration thread callback
		 * requires that the task is off the runqueue.
		 * 确保线程已成功调度出去并处于停放状态，然后再调用创建回调函数。至少迁移线程回调需要线程脱离运行队列。
		 */
		if (!wait_task_inactive(tsk, TASK_PARKED))//等待线程处于停放状态，如果未成功停放
			WARN_ON(1);
		else
			ht->create(cpu);//调用创建回调函数，执行额外的创建操作
	}
	return 0;
}

int smpboot_create_threads(unsigned int cpu)//创建指定 CPU 的 SMP 热插拔线程
{
	struct smp_hotplug_thread *cur;
	int ret = 0;

	mutex_lock(&smpboot_threads_lock);
	list_for_each_entry(cur, &hotplug_threads, list) {
		ret = __smpboot_create_thread(cur, cpu);
		if (ret)
			break;
	}
	mutex_unlock(&smpboot_threads_lock);
	return ret;
}

static void smpboot_unpark_thread(struct smp_hotplug_thread *ht, unsigned int cpu)//用于解除指定 CPU 的热插拔线程的停放状态。
{
	struct task_struct *tsk = *per_cpu_ptr(ht->store, cpu);//获取与指定 CPU 相关联的任务结构体指针

	if (!ht->selfparking)//如果线程不支持自我停放
		kthread_unpark(tsk);//解除线程的停放状态，使其进入活动状态
}

int smpboot_unpark_threads(unsigned int cpu)
{
	struct smp_hotplug_thread *cur;

	mutex_lock(&smpboot_threads_lock);
	list_for_each_entry(cur, &hotplug_threads, list)
		smpboot_unpark_thread(cur, cpu);
	mutex_unlock(&smpboot_threads_lock);
	return 0;
}

static void smpboot_park_thread(struct smp_hotplug_thread *ht, unsigned int cpu)
{
	struct task_struct *tsk = *per_cpu_ptr(ht->store, cpu);

	if (tsk && !ht->selfparking)
		kthread_park(tsk);
}

int smpboot_park_threads(unsigned int cpu)
{
	struct smp_hotplug_thread *cur;

	mutex_lock(&smpboot_threads_lock);
	list_for_each_entry_reverse(cur, &hotplug_threads, list)
		smpboot_park_thread(cur, cpu);
	mutex_unlock(&smpboot_threads_lock);
	return 0;
}

static void smpboot_destroy_threads(struct smp_hotplug_thread *ht)
{
	unsigned int cpu;

	/* We need to destroy also the parked threads of offline cpus */
	for_each_possible_cpu(cpu) {
		struct task_struct *tsk = *per_cpu_ptr(ht->store, cpu);

		if (tsk) {
			kthread_stop_put(tsk);
			*per_cpu_ptr(ht->store, cpu) = NULL;
		}
	}
}

/**
 * smpboot_register_percpu_thread - Register a per_cpu thread related
 * 					    to hotplug
 * @plug_thread:	Hotplug thread descriptor
 *
 * Creates and starts the threads on all online cpus.
 */
int smpboot_register_percpu_thread(struct smp_hotplug_thread *plug_thread)//为系统中每个在线的CPU注册和创建热插拔相关的线程，
{
	unsigned int cpu;
	int ret = 0;

	cpus_read_lock();//获取 CPU 读取锁，防止在创建线程时 CPU 热插拔导致的不一致
	mutex_lock(&smpboot_threads_lock);//获取 smpboot 线程锁，确保线程创建的互斥性
	for_each_online_cpu(cpu) {// 遍历所有在线的 CPU
		ret = __smpboot_create_thread(plug_thread, cpu);//为每个在线 CPU 创建与热插拔相关的线程
		if (ret) {//如果创建线程失败
			smpboot_destroy_threads(plug_thread);//销毁已创建的所有线程，进行清理
			goto out;//跳转到 out 标签，执行资源释放
		}
		smpboot_unpark_thread(plug_thread, cpu);//解除线程的停放状态，使其可以运行
	}
	list_add(&plug_thread->list, &hotplug_threads);// 将新创建的热插拔线程添加到全局的热插拔线程链表中
out:
	mutex_unlock(&smpboot_threads_lock);//释放 smpboot 线程锁
	cpus_read_unlock();//释放 CPU 读取锁
	return ret;//返回线程创建的结果
}
EXPORT_SYMBOL_GPL(smpboot_register_percpu_thread);

/**
 * smpboot_unregister_percpu_thread - Unregister a per_cpu thread related to hotplug
 * @plug_thread:	Hotplug thread descriptor
 *
 * Stops all threads on all possible cpus.
 */
void smpboot_unregister_percpu_thread(struct smp_hotplug_thread *plug_thread)
{
	cpus_read_lock();
	mutex_lock(&smpboot_threads_lock);
	list_del(&plug_thread->list);
	smpboot_destroy_threads(plug_thread);
	mutex_unlock(&smpboot_threads_lock);
	cpus_read_unlock();
}
EXPORT_SYMBOL_GPL(smpboot_unregister_percpu_thread);
