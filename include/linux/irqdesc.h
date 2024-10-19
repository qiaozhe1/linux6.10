/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_IRQDESC_H
#define _LINUX_IRQDESC_H

#include <linux/rcupdate.h>
#include <linux/kobject.h>
#include <linux/mutex.h>

/*
 * Core internal functions to deal with irq descriptors
 */

struct irq_affinity_notify;
struct proc_dir_entry;
struct module;
struct irq_desc;
struct irq_domain;
struct pt_regs;

/**
 * struct irqstat - interrupt statistics
 * @cnt:	real-time interrupt count
 * @ref:	snapshot of interrupt count
 */
struct irqstat {
	unsigned int	cnt;
#ifdef CONFIG_GENERIC_IRQ_STAT_SNAPSHOT
	unsigned int	ref;
#endif
};

/**
 * struct irq_desc - interrupt descriptor
 * @irq_common_data:	per irq and chip data passed down to chip functions
 * @kstat_irqs:		irq stats per cpu
 * @handle_irq:		highlevel irq-events handler
 * @action:		the irq action chain
 * @status_use_accessors: status information
 * @core_internal_state__do_not_mess_with_it: core internal status information
 * @depth:		disable-depth, for nested irq_disable() calls
 * @wake_depth:		enable depth, for multiple irq_set_irq_wake() callers
 * @tot_count:		stats field for non-percpu irqs
 * @irq_count:		stats field to detect stalled irqs
 * @last_unhandled:	aging timer for unhandled count
 * @irqs_unhandled:	stats field for spurious unhandled interrupts
 * @threads_handled:	stats field for deferred spurious detection of threaded handlers
 * @threads_handled_last: comparator field for deferred spurious detection of threaded handlers
 * @lock:		locking for SMP
 * @affinity_hint:	hint to user space for preferred irq affinity
 * @affinity_notify:	context for notification of affinity changes
 * @pending_mask:	pending rebalanced interrupts
 * @threads_oneshot:	bitfield to handle shared oneshot threads
 * @threads_active:	number of irqaction threads currently running
 * @wait_for_threads:	wait queue for sync_irq to wait for threaded handlers
 * @nr_actions:		number of installed actions on this descriptor
 * @no_suspend_depth:	number of irqactions on a irq descriptor with
 *			IRQF_NO_SUSPEND set
 * @force_resume_depth:	number of irqactions on a irq descriptor with
 *			IRQF_FORCE_RESUME set
 * @rcu:		rcu head for delayed free
 * @kobj:		kobject used to represent this struct in sysfs
 * @request_mutex:	mutex to protect request/free before locking desc->lock
 * @dir:		/proc/irq/ procfs entry
 * @debugfs_file:	dentry for the debugfs file
 * @name:		flow handler name for /proc/interrupts output
 */
struct irq_desc {//用于描述每个中断的信息的结构体
	struct irq_common_data	irq_common_data;//包含中断的共享数据，例如中断处理器的亲和性和触发方式。
	struct irq_data		irq_data;//包含中断控制器特定的中断数据，例如中断号和触发配置。
	struct irqstat __percpu	*kstat_irqs;//存储per_CPU的中断统计信息，用于跟踪per_CPU中断的触发次数。
	irq_flow_handler_t	handle_irq;//指向中断流处理程序的函数指针，用于处理特定类型的中断。
	struct irqaction	*action;//指向中断的动作链表，链表中包含了处理该中断的函数列表。
	unsigned int		status_use_accessors;//存储中断的状态标志，通过访问器函数进行读取或修改。
	unsigned int		core_internal_state__do_not_mess_with_it;//内部状态标志，用于管理中断的内部状态，不建议外部代码修改。
	unsigned int		depth;//表示中断的嵌套禁用深度，指示中断被禁用的层级数。
	unsigned int		wake_depth;//表示唤醒的嵌套深度，用于管理中断的唤醒状态。
	unsigned int		tot_count;//用于统计中断的触发次数
	unsigned int		irq_count;//用于检测中断是否出错，尤其是在中断不能正常处理时会增加计数。
	unsigned long		last_unhandled;//存储上一次未处理中断的时间戳，用于跟踪中断问题。
	unsigned int		irqs_unhandled;//记录未处理中断的数量，用于统计和诊断中断的问题。
	atomic_t		threads_handled;//处理此中断的线程数量（原子计数）
	int			threads_handled_last;//上一次处理中断的线程数量
	raw_spinlock_t		lock;
	struct cpumask		*percpu_enabled;//指向per_CPU启用中断的掩码
	const struct cpumask	*percpu_affinity;//指向per_CPU中断亲和性掩码,指定哪些 CPU 可以处理此中断。
#ifdef CONFIG_SMP
	const struct cpumask	*affinity_hint;//中断亲和性提示掩码
	struct irq_affinity_notify *affinity_notify;//中断亲和性改变的通知结构体
#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_var_t		pending_mask;//用于跟踪挂起的中断，防止中断被过早处理
#endif
#endif
	unsigned long		threads_oneshot;//用于标记是否为 oneshot 线程处理模式
	atomic_t		threads_active;//正在活跃处理的线程数量
	wait_queue_head_t       wait_for_threads;//用于等待中断处理线程完成的等待队列
#ifdef CONFIG_PM_SLEEP
	unsigned int		nr_actions;//动作数量计数，用于睡眠相关的中断管理
	unsigned int		no_suspend_depth;//禁止挂起的深度
	unsigned int		cond_suspend_depth;//条件挂起深度
	unsigned int		force_resume_depth;//强制恢复的深度
#endif
#ifdef CONFIG_PROC_FS
	struct proc_dir_entry	*dir;//在 `/proc` 文件系统中与中断相关联的目录
#endif
#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
	struct dentry		*debugfs_file;//`debugfs` 中的调试文件
	const char		*dev_name;//设备名称
#endif
#ifdef CONFIG_SPARSE_IRQ
	struct rcu_head		rcu;//RCU 机制头，用于安全释放 `irq_desc` 结构体
	struct kobject		kobj;//设备模型中的 `kobject`，用于与用户空间进行交互
#endif
	struct mutex		request_mutex;//请求互斥锁，用于请求和释放 IRQ
	int			parent_irq;//父中断编号，用于中断级联
	struct module		*owner;//拥有此中断的模块
	const char		*name;//中断名称
#ifdef CONFIG_HARDIRQS_SW_RESEND
	struct hlist_node	resend_node;//用于重新发送中断的节点
#endif
} ____cacheline_internodealigned_in_smp;

#ifdef CONFIG_SPARSE_IRQ
extern void irq_lock_sparse(void);
extern void irq_unlock_sparse(void);
#else
static inline void irq_lock_sparse(void) { }
static inline void irq_unlock_sparse(void) { }
extern struct irq_desc irq_desc[NR_IRQS];
#endif

static inline unsigned int irq_desc_kstat_cpu(struct irq_desc *desc,
					      unsigned int cpu)
{
	return desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, cpu) : 0;
}

static inline struct irq_desc *irq_data_to_desc(struct irq_data *data)
{
	return container_of(data->common, struct irq_desc, irq_common_data);
}

static inline unsigned int irq_desc_get_irq(struct irq_desc *desc)
{
	return desc->irq_data.irq;
}

static inline struct irq_data *irq_desc_get_irq_data(struct irq_desc *desc)
{
	return &desc->irq_data;
}

static inline struct irq_chip *irq_desc_get_chip(struct irq_desc *desc)
{
	return desc->irq_data.chip;
}

static inline void *irq_desc_get_chip_data(struct irq_desc *desc)
{
	return desc->irq_data.chip_data;
}

static inline void *irq_desc_get_handler_data(struct irq_desc *desc)
{
	return desc->irq_common_data.handler_data;
}

/*
 * Architectures call this to let the generic IRQ layer
 * handle an interrupt.
 */
static inline void generic_handle_irq_desc(struct irq_desc *desc)
{
	desc->handle_irq(desc);
}

int handle_irq_desc(struct irq_desc *desc);
int generic_handle_irq(unsigned int irq);
int generic_handle_irq_safe(unsigned int irq);

#ifdef CONFIG_IRQ_DOMAIN
/*
 * Convert a HW interrupt number to a logical one using a IRQ domain,
 * and handle the result interrupt number. Return -EINVAL if
 * conversion failed.
 */
int generic_handle_domain_irq(struct irq_domain *domain, unsigned int hwirq);
int generic_handle_domain_irq_safe(struct irq_domain *domain, unsigned int hwirq);
int generic_handle_domain_nmi(struct irq_domain *domain, unsigned int hwirq);
#endif

/* Test to see if a driver has successfully requested an irq */
static inline int irq_desc_has_action(struct irq_desc *desc)
{
	return desc && desc->action != NULL;
}

/**
 * irq_set_handler_locked - Set irq handler from a locked region
 * @data:	Pointer to the irq_data structure which identifies the irq
 * @handler:	Flow control handler function for this interrupt
 *
 * Sets the handler in the irq descriptor associated to @data.
 *
 * Must be called with irq_desc locked and valid parameters. Typical
 * call site is the irq_set_type() callback.
 */
static inline void irq_set_handler_locked(struct irq_data *data,
					  irq_flow_handler_t handler)
{
	struct irq_desc *desc = irq_data_to_desc(data);

	desc->handle_irq = handler;
}

/**
 * irq_set_chip_handler_name_locked - Set chip, handler and name from a locked region
 * @data:	Pointer to the irq_data structure for which the chip is set
 * @chip:	Pointer to the new irq chip
 * @handler:	Flow control handler function for this interrupt
 * @name:	Name of the interrupt
 *
 * Replace the irq chip at the proper hierarchy level in @data and
 * sets the handler and name in the associated irq descriptor.
 *
 * Must be called with irq_desc locked and valid parameters.
 */
static inline void
irq_set_chip_handler_name_locked(struct irq_data *data,
				 const struct irq_chip *chip,
				 irq_flow_handler_t handler, const char *name)
{
	struct irq_desc *desc = irq_data_to_desc(data);

	desc->handle_irq = handler;
	desc->name = name;
	data->chip = (struct irq_chip *)chip;
}

bool irq_check_status_bit(unsigned int irq, unsigned int bitmask);

static inline bool irq_balancing_disabled(unsigned int irq)
{
	return irq_check_status_bit(irq, IRQ_NO_BALANCING_MASK);
}

static inline bool irq_is_percpu(unsigned int irq)
{
	return irq_check_status_bit(irq, IRQ_PER_CPU);
}

static inline bool irq_is_percpu_devid(unsigned int irq)
{
	return irq_check_status_bit(irq, IRQ_PER_CPU_DEVID);
}

void __irq_set_lockdep_class(unsigned int irq, struct lock_class_key *lock_class,
			     struct lock_class_key *request_class);
static inline void
irq_set_lockdep_class(unsigned int irq, struct lock_class_key *lock_class,
		      struct lock_class_key *request_class)
{
	if (IS_ENABLED(CONFIG_LOCKDEP))
		__irq_set_lockdep_class(irq, lock_class, request_class);
}

#endif
