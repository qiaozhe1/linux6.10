/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SMPBOOT_H
#define _LINUX_SMPBOOT_H

#include <linux/types.h>

struct task_struct;
/* Cookie handed to the thread_fn*/
struct smpboot_thread_data;

/**
 * struct smp_hotplug_thread - CPU hotplug related thread descriptor
 * @store:		Pointer to per cpu storage for the task pointers
 * @list:		List head for core management
 * @thread_should_run:	Check whether the thread should run or not. Called with
 *			preemption disabled.
 * @thread_fn:		The associated thread function
 * @create:		Optional setup function, called when the thread gets
 *			created (Not called from the thread context)
 * @setup:		Optional setup function, called when the thread gets
 *			operational the first time
 * @cleanup:		Optional cleanup function, called when the thread
 *			should stop (module exit)
 * @park:		Optional park function, called when the thread is
 *			parked (cpu offline)
 * @unpark:		Optional unpark function, called when the thread is
 *			unparked (cpu online)
 * @selfparking:	Thread is not parked by the park function.
 * @thread_comm:	The base name of the thread
 */
struct smp_hotplug_thread {//用于描述在多处理器系统中与 CPU 热插拔相关的线程信息。
	struct task_struct		* __percpu *store;//每个 CPU 的任务结构指针，用于存储与该线程关联的任务
	struct list_head		list;// 链表节点，用于将该线程加入到热插拔线程的链表中
	int				(*thread_should_run)(unsigned int cpu);//函数指针，用于判断线程是否应该在指定的 CPU 上运行
	void				(*thread_fn)(unsigned int cpu);//函数指针，定义线程的主功能函数，在指定的 CPU 上运行
	void				(*create)(unsigned int cpu);//函数指针，用于创建线程时的回调操作
	void				(*setup)(unsigned int cpu);//函数指针，用于在线程启动时的设置操作
	void				(*cleanup)(unsigned int cpu, bool online);//函数指针，用于清理线程，当 CPU 上线或下线时调用
	void				(*park)(unsigned int cpu);//函数指针，用于将线程置于“停放”状态的操作
	void				(*unpark)(unsigned int cpu);//函数指针，用于将线程从“停放”状态恢复的操作
	bool				selfparking;//标志变量，指示线程是否支持自我停放
	const char			*thread_comm;//线程的命令名称字符串，用于标识线程
};

int smpboot_register_percpu_thread(struct smp_hotplug_thread *plug_thread);

void smpboot_unregister_percpu_thread(struct smp_hotplug_thread *plug_thread);

#endif
