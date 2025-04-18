// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 1992, 1998-2006 Linus Torvalds, Ingo Molnar
 * Copyright (C) 2005-2006, Thomas Gleixner, Russell King
 *
 * This file contains the interrupt descriptor management code. Detailed
 * information is available in Documentation/core-api/genericirq.rst
 *
 */
#include <linux/irq.h>
#include <linux/slab.h>
#include <linux/export.h>
#include <linux/interrupt.h>
#include <linux/kernel_stat.h>
#include <linux/maple_tree.h>
#include <linux/irqdomain.h>
#include <linux/sysfs.h>

#include "internals.h"

/*
 * lockdep: we want to handle all irq_desc locks as a single lock-class:
 */
static struct lock_class_key irq_desc_lock_class;

#if defined(CONFIG_SMP)
static int __init irq_affinity_setup(char *str)
{
	alloc_bootmem_cpumask_var(&irq_default_affinity);
	cpulist_parse(str, irq_default_affinity);
	/*
	 * Set at least the boot cpu. We don't want to end up with
	 * bugreports caused by random commandline masks
	 */
	cpumask_set_cpu(smp_processor_id(), irq_default_affinity);
	return 1;
}
__setup("irqaffinity=", irq_affinity_setup);

static void __init init_irq_default_affinity(void)
{
	if (!cpumask_available(irq_default_affinity))//检查中断默认亲和性掩码（irq_default_affinity）是否可用。
		zalloc_cpumask_var(&irq_default_affinity, GFP_NOWAIT);//不可用则分配一个新的CPU掩码。
	if (cpumask_empty(irq_default_affinity))//检查 irq_default_affinity 是否为空，即没有CPU被设置为可以处理中断。
		cpumask_setall(irq_default_affinity);//调用 cpumask_setall 函数将所有 CPU 标记为可处理中断，即设置 irq_default_affinity 掩码中的所有位。
}
#else
static void __init init_irq_default_affinity(void)
{
}
#endif

#ifdef CONFIG_SMP
static int alloc_masks(struct irq_desc *desc, int node)
{
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.affinity,
				     GFP_KERNEL, node))
		return -ENOMEM;

#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	if (!zalloc_cpumask_var_node(&desc->irq_common_data.effective_affinity,
				     GFP_KERNEL, node)) {
		free_cpumask_var(desc->irq_common_data.affinity);
		return -ENOMEM;
	}
#endif

#ifdef CONFIG_GENERIC_PENDING_IRQ
	if (!zalloc_cpumask_var_node(&desc->pending_mask, GFP_KERNEL, node)) {
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
		free_cpumask_var(desc->irq_common_data.effective_affinity);
#endif
		free_cpumask_var(desc->irq_common_data.affinity);
		return -ENOMEM;
	}
#endif
	return 0;
}

static void desc_smp_init(struct irq_desc *desc, int node,
			  const struct cpumask *affinity)
{
	if (!affinity)
		affinity = irq_default_affinity;
	cpumask_copy(desc->irq_common_data.affinity, affinity);

#ifdef CONFIG_GENERIC_PENDING_IRQ
	cpumask_clear(desc->pending_mask);
#endif
#ifdef CONFIG_NUMA
	desc->irq_common_data.node = node;
#endif
}

static void free_masks(struct irq_desc *desc)
{
#ifdef CONFIG_GENERIC_PENDING_IRQ
	free_cpumask_var(desc->pending_mask);
#endif
	free_cpumask_var(desc->irq_common_data.affinity);
#ifdef CONFIG_GENERIC_IRQ_EFFECTIVE_AFF_MASK
	free_cpumask_var(desc->irq_common_data.effective_affinity);
#endif
}

#else
static inline int
alloc_masks(struct irq_desc *desc, int node) { return 0; }
static inline void
desc_smp_init(struct irq_desc *desc, int node, const struct cpumask *affinity) { }
static inline void free_masks(struct irq_desc *desc) { }
#endif

static void desc_set_defaults(unsigned int irq, struct irq_desc *desc, int node,
			      const struct cpumask *affinity, struct module *owner)
{
	int cpu;

	desc->irq_common_data.handler_data = NULL;// 初始化通用中断数据，将 handler_data 设置为 NULL，表示没有关联的处理数据
	desc->irq_common_data.msi_desc = NULL;//将 MSI（Message Signaled Interrupts）描述符设置为 NULL，表示当前中断没有关联的 MSI 描述符

	desc->irq_data.common = &desc->irq_common_data;//将 irq_data 的 common 指向 irq_common_data，建立通用中断数据与 irq_data 的关联
	desc->irq_data.irq = irq;//设置 irq_data 的中断号为给定的 irq
	desc->irq_data.chip = &no_irq_chip;//设置 irq_data 的中断控制器为 no_irq_chip，表示当前中断暂时没有有效的中断控制器
	desc->irq_data.chip_data = NULL;// 将中断控制器的相关数据设置为 NULL
	irq_settings_clr_and_set(desc, ~0, _IRQ_DEFAULT_INIT_FLAGS);//清除并设置中断描述符的默认标志
	irqd_set(&desc->irq_data, IRQD_IRQ_DISABLED);//设置中断状态为禁用状态
	irqd_set(&desc->irq_data, IRQD_IRQ_MASKED);// 设置中断状态为屏蔽状态
	desc->handle_irq = handle_bad_irq;//设置默认的中断处理程序为 handle_bad_irq，表示当前中断没有有效的处理函数
	desc->depth = 1;//设置中断的嵌套深度为 1，表示中断被屏蔽，无法触发
	desc->irq_count = 0;// 初始化中断计数器
	desc->irqs_unhandled = 0;//初始化未处理中断计数器
	desc->tot_count = 0;//初始化总的中断计数
	desc->name = NULL;//将中断名称初始化为 NULL
	desc->owner = owner;//设置中断的所有者模块为传入的 owner
	for_each_possible_cpu(cpu)//遍历每一个可能的 CPU，初始化每个 CPU 的中断统计数据
		*per_cpu_ptr(desc->kstat_irqs, cpu) = (struct irqstat) { };
	desc_smp_init(desc, node, affinity);//初始化与 SMP（对称多处理）相关的中断属性
}

int nr_irqs = NR_IRQS;
EXPORT_SYMBOL_GPL(nr_irqs);

static DEFINE_MUTEX(sparse_irq_lock);
static struct maple_tree sparse_irqs = MTREE_INIT_EXT(sparse_irqs,
					MT_FLAGS_ALLOC_RANGE |
					MT_FLAGS_LOCK_EXTERN |
					MT_FLAGS_USE_RCU,
					sparse_irq_lock);

static int irq_find_free_area(unsigned int from, unsigned int cnt)
{
	MA_STATE(mas, &sparse_irqs, 0, 0);//定义并初始化区间状态对象，用于操作稀疏IRQ集合

	if (mas_empty_area(&mas, from, MAX_SPARSE_IRQS, cnt))//尝试在稀疏 IRQ 集合中查找从 'from' 开始的空闲区域，要求长度为 'cnt
		return -ENOSPC;//如果找不到满足要求的区域，返回 -ENOSPC，表示没有空间
	return mas.index;// 返回找到的空闲区域的起始索引
}

static unsigned int irq_find_at_or_after(unsigned int offset)
{
	unsigned long index = offset;
	struct irq_desc *desc;

	guard(rcu)();
	desc = mt_find(&sparse_irqs, &index, nr_irqs);

	return desc ? irq_desc_get_irq(desc) : nr_irqs;
}

static void irq_insert_desc(unsigned int irq, struct irq_desc *desc)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);//初始化 ma_state 对象，用于操作稀疏 IRQ 数据结构
	WARN_ON(mas_store_gfp(&mas, desc, GFP_KERNEL) != 0);//将中断描述符存储到稀疏 IRQ 数据结构中，并检查是否存储成功
}

static void delete_irq_desc(unsigned int irq)
{
	MA_STATE(mas, &sparse_irqs, irq, irq);
	mas_erase(&mas);
}

#ifdef CONFIG_SPARSE_IRQ
static const struct kobj_type irq_kobj_type;
#endif

static int init_desc(struct irq_desc *desc, int irq, int node,
		     unsigned int flags,
		     const struct cpumask *affinity,
		     struct module *owner)
{
	desc->kstat_irqs = alloc_percpu(struct irqstat);//为每个 CPU 分配中断统计信息的 percpu 数据结构
	if (!desc->kstat_irqs)
		return -ENOMEM;//分配失败，返回内存不足错误

	if (alloc_masks(desc, node)) {//为中断描述符分配必要的掩码数据
		free_percpu(desc->kstat_irqs);//如果分配失败，释放之前分配的 percpu 数据
		return -ENOMEM;//返回内存不足错误
	}

	raw_spin_lock_init(&desc->lock);//初始化原始自旋锁，用于保护中断描述符的并发访问
	lockdep_set_class(&desc->lock, &irq_desc_lock_class);//设置锁的锁依赖性类，用于调试锁的依赖关系
	mutex_init(&desc->request_mutex);//初始化请求互斥锁，保护中断请求和释放操作
	init_waitqueue_head(&desc->wait_for_threads);//初始化等待队列，用于等待中断处理线程完成
	desc_set_defaults(irq, desc, node, affinity, owner);//设置中断描述符的默认值
	irqd_set(&desc->irq_data, flags);//设置中断数据的标志位
	irq_resend_init(desc);//初始化中断重发设置
#ifdef CONFIG_SPARSE_IRQ
	kobject_init(&desc->kobj, &irq_kobj_type);//初始化 kobject，用于与用户空间交互
	init_rcu_head(&desc->rcu);//初始化 RCU 头，用于安全地释放中断描述符
#endif

	return 0;// 初始化成功，返回 0
}

#ifdef CONFIG_SPARSE_IRQ

static void irq_kobj_release(struct kobject *kobj);

#ifdef CONFIG_SYSFS
static struct kobject *irq_kobj_base;

#define IRQ_ATTR_RO(_name) \
static struct kobj_attribute _name##_attr = __ATTR_RO(_name)

static ssize_t per_cpu_count_show(struct kobject *kobj,
				  struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;
	char *p = "";
	int cpu;

	for_each_possible_cpu(cpu) {
		unsigned int c = irq_desc_kstat_cpu(desc, cpu);

		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s%u", p, c);
		p = ",";
	}

	ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");
	return ret;
}
IRQ_ATTR_RO(per_cpu_count);

static ssize_t chip_name_show(struct kobject *kobj,
			      struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;

	raw_spin_lock_irq(&desc->lock);
	if (desc->irq_data.chip && desc->irq_data.chip->name) {
		ret = scnprintf(buf, PAGE_SIZE, "%s\n",
				desc->irq_data.chip->name);
	}
	raw_spin_unlock_irq(&desc->lock);

	return ret;
}
IRQ_ATTR_RO(chip_name);

static ssize_t hwirq_show(struct kobject *kobj,
			  struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;

	raw_spin_lock_irq(&desc->lock);
	if (desc->irq_data.domain)
		ret = sprintf(buf, "%lu\n", desc->irq_data.hwirq);
	raw_spin_unlock_irq(&desc->lock);

	return ret;
}
IRQ_ATTR_RO(hwirq);

static ssize_t type_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;

	raw_spin_lock_irq(&desc->lock);
	ret = sprintf(buf, "%s\n",
		      irqd_is_level_type(&desc->irq_data) ? "level" : "edge");
	raw_spin_unlock_irq(&desc->lock);

	return ret;

}
IRQ_ATTR_RO(type);

static ssize_t wakeup_show(struct kobject *kobj,
			   struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;

	raw_spin_lock_irq(&desc->lock);
	ret = sprintf(buf, "%s\n",
		      irqd_is_wakeup_set(&desc->irq_data) ? "enabled" : "disabled");
	raw_spin_unlock_irq(&desc->lock);

	return ret;

}
IRQ_ATTR_RO(wakeup);

static ssize_t name_show(struct kobject *kobj,
			 struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	ssize_t ret = 0;

	raw_spin_lock_irq(&desc->lock);
	if (desc->name)
		ret = scnprintf(buf, PAGE_SIZE, "%s\n", desc->name);
	raw_spin_unlock_irq(&desc->lock);

	return ret;
}
IRQ_ATTR_RO(name);

static ssize_t actions_show(struct kobject *kobj,
			    struct kobj_attribute *attr, char *buf)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);
	struct irqaction *action;
	ssize_t ret = 0;
	char *p = "";

	raw_spin_lock_irq(&desc->lock);
	for_each_action_of_desc(desc, action) {
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "%s%s",
				 p, action->name);
		p = ",";
	}
	raw_spin_unlock_irq(&desc->lock);

	if (ret)
		ret += scnprintf(buf + ret, PAGE_SIZE - ret, "\n");

	return ret;
}
IRQ_ATTR_RO(actions);

static struct attribute *irq_attrs[] = {
	&per_cpu_count_attr.attr,
	&chip_name_attr.attr,
	&hwirq_attr.attr,
	&type_attr.attr,
	&wakeup_attr.attr,
	&name_attr.attr,
	&actions_attr.attr,
	NULL
};
ATTRIBUTE_GROUPS(irq);

static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
	.sysfs_ops	= &kobj_sysfs_ops,
	.default_groups = irq_groups,
};

static void irq_sysfs_add(int irq, struct irq_desc *desc)
{
	if (irq_kobj_base) {
		/*
		 * Continue even in case of failure as this is nothing
		 * crucial and failures in the late irq_sysfs_init()
		 * cannot be rolled back.
		 */
		if (kobject_add(&desc->kobj, irq_kobj_base, "%d", irq))
			pr_warn("Failed to add kobject for irq %d\n", irq);
		else
			desc->istate |= IRQS_SYSFS;
	}
}

static void irq_sysfs_del(struct irq_desc *desc)
{
	/*
	 * Only invoke kobject_del() when kobject_add() was successfully
	 * invoked for the descriptor. This covers both early boot, where
	 * sysfs is not initialized yet, and the case of a failed
	 * kobject_add() invocation.
	 */
	if (desc->istate & IRQS_SYSFS)
		kobject_del(&desc->kobj);
}

static int __init irq_sysfs_init(void)
{
	struct irq_desc *desc;
	int irq;

	/* Prevent concurrent irq alloc/free */
	irq_lock_sparse();

	irq_kobj_base = kobject_create_and_add("irq", kernel_kobj);
	if (!irq_kobj_base) {
		irq_unlock_sparse();
		return -ENOMEM;
	}

	/* Add the already allocated interrupts */
	for_each_irq_desc(irq, desc)
		irq_sysfs_add(irq, desc);
	irq_unlock_sparse();

	return 0;
}
postcore_initcall(irq_sysfs_init);

#else /* !CONFIG_SYSFS */

static const struct kobj_type irq_kobj_type = {
	.release	= irq_kobj_release,
};

static void irq_sysfs_add(int irq, struct irq_desc *desc) {}
static void irq_sysfs_del(struct irq_desc *desc) {}

#endif /* CONFIG_SYSFS */

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return mtree_load(&sparse_irqs, irq);
}
#ifdef CONFIG_KVM_BOOK3S_64_HV_MODULE
EXPORT_SYMBOL_GPL(irq_to_desc);
#endif

void irq_lock_sparse(void)
{
	mutex_lock(&sparse_irq_lock);
}

void irq_unlock_sparse(void)
{
	mutex_unlock(&sparse_irq_lock);
}

static struct irq_desc *alloc_desc(int irq, int node, unsigned int flags,
				   const struct cpumask *affinity,
				   struct module *owner)
{
	struct irq_desc *desc;//定义指向中断描述符的指针
	int ret;

	desc = kzalloc_node(sizeof(*desc), GFP_KERNEL, node);//为中断描述符分配内存,并将其初始化为 0
	if (!desc)
		return NULL;

	ret = init_desc(desc, irq, node, flags, affinity, owner);// 初始化中断描述符
	if (unlikely(ret)) {//如果初始化失败，释放之前分配的内存，并返回 NULL
		kfree(desc);
		return NULL;
	}

	return desc;//如果初始化成功，返回分配的中断描述符
}

static void irq_kobj_release(struct kobject *kobj)
{
	struct irq_desc *desc = container_of(kobj, struct irq_desc, kobj);

	free_masks(desc);
	free_percpu(desc->kstat_irqs);
	kfree(desc);
}

static void delayed_free_desc(struct rcu_head *rhp)
{
	struct irq_desc *desc = container_of(rhp, struct irq_desc, rcu);

	kobject_put(&desc->kobj);
}

static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	irq_remove_debugfs_entry(desc);
	unregister_irq_proc(irq, desc);

	/*
	 * sparse_irq_lock protects also show_interrupts() and
	 * kstat_irq_usr(). Once we deleted the descriptor from the
	 * sparse tree we can free it. Access in proc will fail to
	 * lookup the descriptor.
	 *
	 * The sysfs entry must be serialized against a concurrent
	 * irq_sysfs_init() as well.
	 */
	irq_sysfs_del(desc);
	delete_irq_desc(irq);

	/*
	 * We free the descriptor, masks and stat fields via RCU. That
	 * allows demultiplex interrupts to do rcu based management of
	 * the child interrupts.
	 * This also allows us to use rcu in kstat_irqs_usr().
	 */
	call_rcu(&desc->rcu, delayed_free_desc);
}

static int alloc_descs(unsigned int start, unsigned int cnt, int node,
		       const struct irq_affinity_desc *affinity,
		       struct module *owner)
{
	struct irq_desc *desc;//定义中断描述符指针，用于存储分配的中断描述符
	int i;

	/* Validate affinity mask(s) */
	if (affinity) {//如果传入了中断亲和性描述
		for (i = 0; i < cnt; i++) {//遍历每一个需要分配的中断
			if (cpumask_empty(&affinity[i].mask))//检查亲和性掩码是否为空，即是否没有指定任何CPU
				return -EINVAL;//如果掩码为空，返回 -EINVAL 表示无效参数，无法分配中断
		}
	}
	/*开始逐个分配中断描述符*/
	for (i = 0; i < cnt; i++) {//遍历需要分配的中断数量
		const struct cpumask *mask = NULL;//定义中断亲和性掩码指针，初始为空
		unsigned int flags = 0;//定义中断标志，初始为 0，表示没有特殊标志

		if (affinity) {//如果有指定的中断亲和性描述
			if (affinity->is_managed) {//如果中断是托管的（表示中断由系统管理）
				flags = IRQD_AFFINITY_MANAGED |
					IRQD_MANAGED_SHUTDOWN;// 设置中断标志，IRQD_AFFINITY_MANAGED - 表示该中断是托管中断，由系统负责管理其亲和性。IRQD_MANAGED_SHUTDOWN - 表示该中断在系统关机时需要特殊处理。
			}
			mask = &affinity->mask;//设置亲和性掩码为当前中断的亲和性掩码
			node = cpu_to_node(cpumask_first(mask));// 获取掩码中第一个 CPU 所属的 NUMA 节点，确保中断在正确的节点上分配
			affinity++;//移动到下一个亲和性描述，用于下一次循环
		}

		desc = alloc_desc(start + i, node, flags, mask, owner);//分配中断描述符，并指定相关的属性（如 NUMA 节点、标志、亲和性等）
		if (!desc)//如果分配失败，跳转到错误处理部分
			goto err;
		irq_insert_desc(start + i, desc);//将分配的中断描述符插入到中断描述符数组中，以便系统管理和使用
		irq_sysfs_add(start + i, desc);//将中断信息添加到 sysfs 文件系统中，使用户空间程序可以访问和查看中断的信息
		irq_add_debugfs_entry(start + i, desc);//将中断信息添加到 debugfs 文件系统中，以便内核开发人员进行调试和分析
	}
	return start;//返回成功分配的起始中断号，表示操作成功

err:
	for (i--; i >= 0; i--)//如果在分配过程中出现错误，释放已经成功分配的中断描述符，防止资源泄漏
		free_desc(start + i);
	return -ENOMEM;// 返回-ENOMEM表示内存不足，无法完成所有中断的分配
}

static int irq_expand_nr_irqs(unsigned int nr)
{
	if (nr > MAX_SPARSE_IRQS)
		return -ENOMEM;
	nr_irqs = nr;
	return 0;
}

int __init early_irq_init(void)
{
	int i, initcnt, node = first_online_node;//定义循环变量 i，预分配的 IRQ 数量 initcnt，以及第一个在线节点 node
	struct irq_desc *desc;//指向 IRQ 描述符的指针

	init_irq_default_affinity();//初始化 IRQ 的默认亲和性设置

	/* Let arch update nr_irqs and return the nr of preallocated irqs */
	initcnt = arch_probe_nr_irqs();//让架构特定代码更新 nr_irqs，并返回预分配的 IRQ 数量(返回0)
	printk(KERN_INFO "NR_IRQS: %d, nr_irqs: %d, preallocated irqs: %d\n",
	       NR_IRQS, nr_irqs, initcnt);//打印系统支持的 IRQ 信息，包括总数、已分配的 IRQ 数量和预分配数量

	if (WARN_ON(nr_irqs > MAX_SPARSE_IRQS))//检查并确保 nr_irqs 不超过最大稀疏 IRQ 数量限制
		nr_irqs = MAX_SPARSE_IRQS;

	if (WARN_ON(initcnt > MAX_SPARSE_IRQS))//检查并确保预分配数量 initcnt 不超过最大稀疏 IRQ 数量限制
		initcnt = MAX_SPARSE_IRQS;

	if (initcnt > nr_irqs)//如果 initcnt 大于当前的 nr_irqs，则更新 nr_irqs 为 initcnt
		nr_irqs = initcnt;

	for (i = 0; i < initcnt; i++) {//为每个 IRQ 分配描述符并插入到描述符表中
		desc = alloc_desc(i, node, 0, NULL, NULL);//为 IRQ 号 i 分配描述符
		irq_insert_desc(i, desc);//将描述符插入到 IRQ 描述符表中
	}
	return arch_early_irq_init();//调用架构特定的早期 IRQ 初始化函数
}

#else /* !CONFIG_SPARSE_IRQ */

struct irq_desc irq_desc[NR_IRQS] __cacheline_aligned_in_smp = {
	[0 ... NR_IRQS-1] = {
		.handle_irq	= handle_bad_irq,
		.depth		= 1,
		.lock		= __RAW_SPIN_LOCK_UNLOCKED(irq_desc->lock),
	}
};

int __init early_irq_init(void)
{
	int count, i, node = first_online_node;
	int ret;

	init_irq_default_affinity();

	printk(KERN_INFO "NR_IRQS: %d\n", NR_IRQS);

	count = ARRAY_SIZE(irq_desc);

	for (i = 0; i < count; i++) {
		ret = init_desc(irq_desc + i, i, node, 0, NULL, NULL);
		if (unlikely(ret))
			goto __free_desc_res;
	}

	return arch_early_irq_init();

__free_desc_res:
	while (--i >= 0) {
		free_masks(irq_desc + i);
		free_percpu(irq_desc[i].kstat_irqs);
	}

	return ret;
}

struct irq_desc *irq_to_desc(unsigned int irq)
{
	return (irq < NR_IRQS) ? irq_desc + irq : NULL;
}
EXPORT_SYMBOL(irq_to_desc);

static void free_desc(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);
	unsigned long flags;

	raw_spin_lock_irqsave(&desc->lock, flags);
	desc_set_defaults(irq, desc, irq_desc_get_node(desc), NULL, NULL);
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	delete_irq_desc(irq);
}

static inline int alloc_descs(unsigned int start, unsigned int cnt, int node,
			      const struct irq_affinity_desc *affinity,
			      struct module *owner)
{
	u32 i;

	for (i = 0; i < cnt; i++) {
		struct irq_desc *desc = irq_to_desc(start + i);

		desc->owner = owner;
		irq_insert_desc(start + i, desc);
	}
	return start;
}

static int irq_expand_nr_irqs(unsigned int nr)
{
	return -ENOMEM;
}

void irq_mark_irq(unsigned int irq)
{
	mutex_lock(&sparse_irq_lock);
	irq_insert_desc(irq, irq_desc + irq);
	mutex_unlock(&sparse_irq_lock);
}

#ifdef CONFIG_GENERIC_IRQ_LEGACY
void irq_init_desc(unsigned int irq)
{
	free_desc(irq);
}
#endif

#endif /* !CONFIG_SPARSE_IRQ */

int handle_irq_desc(struct irq_desc *desc)
{
	struct irq_data *data;

	if (!desc)
		return -EINVAL;

	data = irq_desc_get_irq_data(desc);
	if (WARN_ON_ONCE(!in_hardirq() && handle_enforce_irqctx(data)))
		return -EPERM;

	generic_handle_irq_desc(desc);
	return 0;
}

/**
 * generic_handle_irq - Invoke the handler for a particular irq
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
  */
int generic_handle_irq(unsigned int irq)
{
	return handle_irq_desc(irq_to_desc(irq));
}
EXPORT_SYMBOL_GPL(generic_handle_irq);

/**
 * generic_handle_irq_safe - Invoke the handler for a particular irq from any
 *			     context.
 * @irq:	The irq number to handle
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process context). It
 * will report an error if not invoked from IRQ context and the irq has been
 * marked to enforce IRQ-context only.
 */
int generic_handle_irq_safe(unsigned int irq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = handle_irq_desc(irq_to_desc(irq));
	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(generic_handle_irq_safe);

#ifdef CONFIG_IRQ_DOMAIN
/**
 * generic_handle_domain_irq - Invoke the handler for a HW irq belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an IRQ context with irq regs
 * 		initialized.
 */
int generic_handle_domain_irq(struct irq_domain *domain, unsigned int hwirq)
{
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq);

 /**
 * generic_handle_irq_safe - Invoke the handler for a HW irq belonging
 *			     to a domain from any context.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, a negative value on error.
 *
 * This function can be called from any context (IRQ or process
 * context). If the interrupt is marked as 'enforce IRQ-context only' then
 * the function must be invoked from hard interrupt context.
 */
int generic_handle_domain_irq_safe(struct irq_domain *domain, unsigned int hwirq)
{
	unsigned long flags;
	int ret;

	local_irq_save(flags);
	ret = handle_irq_desc(irq_resolve_mapping(domain, hwirq));
	local_irq_restore(flags);
	return ret;
}
EXPORT_SYMBOL_GPL(generic_handle_domain_irq_safe);

/**
 * generic_handle_domain_nmi - Invoke the handler for a HW nmi belonging
 *                             to a domain.
 * @domain:	The domain where to perform the lookup
 * @hwirq:	The HW irq number to convert to a logical one
 *
 * Returns:	0 on success, or -EINVAL if conversion has failed
 *
 * 		This function must be called from an NMI context with irq regs
 * 		initialized.
 **/
int generic_handle_domain_nmi(struct irq_domain *domain, unsigned int hwirq)
{
	WARN_ON_ONCE(!in_nmi());
	return handle_irq_desc(irq_resolve_mapping(domain, hwirq));
}
#endif

/* Dynamic interrupt handling */

/**
 * irq_free_descs - free irq descriptors
 * @from:	Start of descriptor range
 * @cnt:	Number of consecutive irqs to free
 */
void irq_free_descs(unsigned int from, unsigned int cnt)
{
	int i;

	if (from >= nr_irqs || (from + cnt) > nr_irqs)
		return;

	mutex_lock(&sparse_irq_lock);
	for (i = 0; i < cnt; i++)
		free_desc(from + i);

	mutex_unlock(&sparse_irq_lock);
}
EXPORT_SYMBOL_GPL(irq_free_descs);

/**
 * __irq_alloc_descs - allocate and initialize a range of irq descriptors
 * @irq:	Allocate for specific irq number if irq >= 0
 * @from:	Start the search from this irq number
 * @cnt:	Number of consecutive irqs to allocate.
 * @node:	Preferred node on which the irq descriptor should be allocated
 * @owner:	Owning module (can be NULL)
 * @affinity:	Optional pointer to an affinity mask array of size @cnt which
 *		hints where the irq descriptors should be allocated and which
 *		default affinities to use
 *
 * Returns the first irq number or error code
 */
int __ref
__irq_alloc_descs(int irq, unsigned int from, unsigned int cnt, int node,
		  struct module *owner, const struct irq_affinity_desc *affinity)
{
	int start, ret;//定义开始分配的位置和返回值

	if (!cnt)//如果需要分配的中断数量为 0，返回无效参数错误
		return -EINVAL;

	if (irq >= 0) {//如果指定了 irq，则检查起始位置是否合理，并将 from 更新为指定的 irq
		if (from > irq)//如果指定的起始位置大于 irq，返回无效参数错误
			return -EINVAL;
		from = irq;//如果指定了有效的 irq，将 from 设置为指定的 irq
	} else {
		/*
		 * For interrupts which are freely allocated the
		 * architecture can force a lower bound to the @from
		 * argument. x86 uses this to exclude the GSI space.
		 */
		from = arch_dynirq_lower_bound(from);//如果没有指定具体的irq，使用体系结构提供的下限进行限制
	}

	mutex_lock(&sparse_irq_lock);//加锁，防止其他线程并发修改稀疏 IRQ 表

	start = irq_find_free_area(from, cnt);//查找从指定位置开始的空闲中断描述符区域
	ret = -EEXIST;//初始化返回值，表示区域不可用
	if (irq >=0 && start != irq)//如果指定了 irq，但找到的空闲位置不等于 irq，解锁并返回
		goto unlock;

	if (start + cnt > nr_irqs) {//如果分配的区域超出了系统支持的中断数，则尝试扩展中断数量
		ret = irq_expand_nr_irqs(start + cnt);
		if (ret)// 如果扩展失败，解锁并返回错误
			goto unlock;
	}
	ret = alloc_descs(start, cnt, node, affinity, owner);//分配中断描述符
unlock:
	mutex_unlock(&sparse_irq_lock);//解锁，释放对稀疏 IRQ 表的控制
	return ret;//返回分配的起始位置或者错误代码
}
EXPORT_SYMBOL_GPL(__irq_alloc_descs);

/**
 * irq_get_next_irq - get next allocated irq number
 * @offset:	where to start the search
 *
 * Returns next irq number after offset or nr_irqs if none is found.
 */
unsigned int irq_get_next_irq(unsigned int offset)
{
	return irq_find_at_or_after(offset);
}

struct irq_desc *
__irq_get_desc_lock(unsigned int irq, unsigned long *flags, bool bus,
		    unsigned int check)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		if (check & _IRQ_DESC_CHECK) {
			if ((check & _IRQ_DESC_PERCPU) &&
			    !irq_settings_is_per_cpu_devid(desc))
				return NULL;

			if (!(check & _IRQ_DESC_PERCPU) &&
			    irq_settings_is_per_cpu_devid(desc))
				return NULL;
		}

		if (bus)
			chip_bus_lock(desc);
		raw_spin_lock_irqsave(&desc->lock, *flags);
	}
	return desc;
}

void __irq_put_desc_unlock(struct irq_desc *desc, unsigned long flags, bool bus)
	__releases(&desc->lock)
{
	raw_spin_unlock_irqrestore(&desc->lock, flags);
	if (bus)
		chip_bus_sync_unlock(desc);
}

int irq_set_percpu_devid_partition(unsigned int irq,
				   const struct cpumask *affinity)
{
	struct irq_desc *desc = irq_to_desc(irq);//获取虚拟中断号对应的中断描述符

	if (!desc || desc->percpu_enabled)// 检查中断描述符是否存在以及是否已经启用了per_CPU的中断ID
		return -EINVAL;//如果中断描述符无效或已经启用了per_CPU ID，返回 -EINVAL 表示无效参数

	desc->percpu_enabled = kzalloc(sizeof(*desc->percpu_enabled), GFP_KERNEL);//为percpu_enabled分配内存，用于标记每个CPU是否启用该中断

	if (!desc->percpu_enabled)//如果内存分配失败，返回 -ENOMEM 表示内存不足
		return -ENOMEM;

	desc->percpu_affinity = affinity ? : cpu_possible_mask;//设置每 CPU 的中断亲和性掩码，如果未指定亲和性，则默认为所有可能的 CPU

	irq_set_percpu_devid_flags(irq);//设置中断描述符的标志，表示该中断为每 CPU 唯一的设备 ID
	return 0;//返回 0 表示成功设置每 CPU 的中断 ID
}

int irq_set_percpu_devid(unsigned int irq)
{
	return irq_set_percpu_devid_partition(irq, NULL);
}

int irq_get_percpu_devid_partition(unsigned int irq, struct cpumask *affinity)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->percpu_enabled)
		return -EINVAL;

	if (affinity)
		cpumask_copy(affinity, desc->percpu_affinity);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_get_percpu_devid_partition);

void kstat_incr_irq_this_cpu(unsigned int irq)
{
	kstat_incr_irqs_this_cpu(irq_to_desc(irq));
}

/**
 * kstat_irqs_cpu - Get the statistics for an interrupt on a cpu
 * @irq:	The interrupt number
 * @cpu:	The cpu number
 *
 * Returns the sum of interrupt counts on @cpu since boot for
 * @irq. The caller must ensure that the interrupt is not removed
 * concurrently.
 */
unsigned int kstat_irqs_cpu(unsigned int irq, int cpu)
{
	struct irq_desc *desc = irq_to_desc(irq);

	return desc && desc->kstat_irqs ? per_cpu(desc->kstat_irqs->cnt, cpu) : 0;
}

unsigned int kstat_irqs_desc(struct irq_desc *desc, const struct cpumask *cpumask)
{
	unsigned int sum = 0;
	int cpu;

	if (!irq_settings_is_per_cpu_devid(desc) &&
	    !irq_settings_is_per_cpu(desc) &&
	    !irq_is_nmi(desc))
		return data_race(desc->tot_count);

	for_each_cpu(cpu, cpumask)
		sum += data_race(per_cpu(desc->kstat_irqs->cnt, cpu));
	return sum;
}

static unsigned int kstat_irqs(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->kstat_irqs)
		return 0;
	return kstat_irqs_desc(desc, cpu_possible_mask);
}

#ifdef CONFIG_GENERIC_IRQ_STAT_SNAPSHOT

void kstat_snapshot_irqs(void)
{
	struct irq_desc *desc;
	unsigned int irq;

	for_each_irq_desc(irq, desc) {
		if (!desc->kstat_irqs)
			continue;
		this_cpu_write(desc->kstat_irqs->ref, this_cpu_read(desc->kstat_irqs->cnt));
	}
}

unsigned int kstat_get_irq_since_snapshot(unsigned int irq)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (!desc || !desc->kstat_irqs)
		return 0;
	return this_cpu_read(desc->kstat_irqs->cnt) - this_cpu_read(desc->kstat_irqs->ref);
}

#endif

/**
 * kstat_irqs_usr - Get the statistics for an interrupt from thread context
 * @irq:	The interrupt number
 *
 * Returns the sum of interrupt counts on all cpus since boot for @irq.
 *
 * It uses rcu to protect the access since a concurrent removal of an
 * interrupt descriptor is observing an rcu grace period before
 * delayed_free_desc()/irq_kobj_release().
 */
unsigned int kstat_irqs_usr(unsigned int irq)
{
	unsigned int sum;

	rcu_read_lock();
	sum = kstat_irqs(irq);
	rcu_read_unlock();
	return sum;
}

#ifdef CONFIG_LOCKDEP
void __irq_set_lockdep_class(unsigned int irq, struct lock_class_key *lock_class,
			     struct lock_class_key *request_class)
{
	struct irq_desc *desc = irq_to_desc(irq);

	if (desc) {
		lockdep_set_class(&desc->lock, lock_class);
		lockdep_set_class(&desc->request_mutex, request_class);
	}
}
EXPORT_SYMBOL_GPL(__irq_set_lockdep_class);
#endif
