// SPDX-License-Identifier: GPL-2.0

#define pr_fmt(fmt)  "irq: " fmt

#include <linux/acpi.h>
#include <linux/debugfs.h>
#include <linux/hardirq.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdesc.h>
#include <linux/irqdomain.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/topology.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/fs.h>

static LIST_HEAD(irq_domain_list);
static DEFINE_MUTEX(irq_domain_mutex);

static struct irq_domain *irq_default_domain;

static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity);
static void irq_domain_check_hierarchy(struct irq_domain *domain);
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq);

struct irqchip_fwid {//用于表示一个中断控制器的相关信息
	struct fwnode_handle	fwnode;//设备树节点的句柄，表示该中断控制器在设备树中的位置及其属性。
	unsigned int		type;//中断控制器的类型标识，通常用于区分不同类型的中断控制器（例如，GPIO、PCI等）。
	char			*name;//中断控制器的名称，用于调试和日志记录，以便识别具体的中断控制器。
	phys_addr_t		*pa;//指向物理地址的指针，通常用于表示中断控制器寄存器的物理地址，以便进行直接访问。
};

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
static void debugfs_add_domain_dir(struct irq_domain *d);
static void debugfs_remove_domain_dir(struct irq_domain *d);
#else
static inline void debugfs_add_domain_dir(struct irq_domain *d) { }
static inline void debugfs_remove_domain_dir(struct irq_domain *d) { }
#endif

static const char *irqchip_fwnode_get_name(const struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

	return fwid->name;
}

const struct fwnode_operations irqchip_fwnode_ops = {
	.get_name = irqchip_fwnode_get_name,
};
EXPORT_SYMBOL_GPL(irqchip_fwnode_ops);

/**
 * __irq_domain_alloc_fwnode - Allocate a fwnode_handle suitable for
 *                           identifying an irq domain
 * @type:	Type of irqchip_fwnode. See linux/irqdomain.h
 * @id:		Optional user provided id if name != NULL
 * @name:	Optional user provided domain name
 * @pa:		Optional user-provided physical address
 *
 * Allocate a struct irqchip_fwid, and return a pointer to the embedded
 * fwnode_handle (or NULL on failure).
 *
 * Note: The types IRQCHIP_FWNODE_NAMED and IRQCHIP_FWNODE_NAMED_ID are
 * solely to transport name information to irqdomain creation code. The
 * node is not stored. For other types the pointer is kept in the irq
 * domain struct.
 */
struct fwnode_handle *__irq_domain_alloc_fwnode(unsigned int type, int id,
						const char *name,
						phys_addr_t *pa)
{
	struct irqchip_fwid *fwid;
	char *n;

	fwid = kzalloc(sizeof(*fwid), GFP_KERNEL);

	switch (type) {
	case IRQCHIP_FWNODE_NAMED:
		n = kasprintf(GFP_KERNEL, "%s", name);
		break;
	case IRQCHIP_FWNODE_NAMED_ID:
		n = kasprintf(GFP_KERNEL, "%s-%d", name, id);
		break;
	default:
		n = kasprintf(GFP_KERNEL, "irqchip@%pa", pa);
		break;
	}

	if (!fwid || !n) {
		kfree(fwid);
		kfree(n);
		return NULL;
	}

	fwid->type = type;
	fwid->name = n;
	fwid->pa = pa;
	fwnode_init(&fwid->fwnode, &irqchip_fwnode_ops);
	return &fwid->fwnode;
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_fwnode);

/**
 * irq_domain_free_fwnode - Free a non-OF-backed fwnode_handle
 *
 * Free a fwnode_handle allocated with irq_domain_alloc_fwnode.
 */
void irq_domain_free_fwnode(struct fwnode_handle *fwnode)
{
	struct irqchip_fwid *fwid;

	if (!fwnode || WARN_ON(!is_fwnode_irqchip(fwnode)))
		return;

	fwid = container_of(fwnode, struct irqchip_fwid, fwnode);
	kfree(fwid->name);
	kfree(fwid);
}
EXPORT_SYMBOL_GPL(irq_domain_free_fwnode);

static struct irq_domain *__irq_domain_create(struct fwnode_handle *fwnode,
					      unsigned int size,
					      irq_hw_number_t hwirq_max,
					      int direct_max,
					      const struct irq_domain_ops *ops,
					      void *host_data)
{
	struct irqchip_fwid *fwid;
	struct irq_domain *domain;

	static atomic_t unknown_domains;

	if (WARN_ON((size && direct_max) ||
		    (!IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) && direct_max) ||
		    (direct_max && (direct_max != hwirq_max))))
		return NULL;

	domain = kzalloc_node(struct_size(domain, revmap, size),
			      GFP_KERNEL, of_node_to_nid(to_of_node(fwnode)));
	if (!domain)
		return NULL;

	if (is_fwnode_irqchip(fwnode)) {
		fwid = container_of(fwnode, struct irqchip_fwid, fwnode);

		switch (fwid->type) {
		case IRQCHIP_FWNODE_NAMED:
		case IRQCHIP_FWNODE_NAMED_ID:
			domain->fwnode = fwnode;
			domain->name = kstrdup(fwid->name, GFP_KERNEL);
			if (!domain->name) {
				kfree(domain);
				return NULL;
			}
			domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
			break;
		default:
			domain->fwnode = fwnode;
			domain->name = fwid->name;
			break;
		}
	} else if (is_of_node(fwnode) || is_acpi_device_node(fwnode) ||
		   is_software_node(fwnode)) {
		char *name;

		/*
		 * fwnode paths contain '/', which debugfs is legitimately
		 * unhappy about. Replace them with ':', which does
		 * the trick and is not as offensive as '\'...
		 */
		name = kasprintf(GFP_KERNEL, "%pfw", fwnode);
		if (!name) {
			kfree(domain);
			return NULL;
		}

		domain->name = strreplace(name, '/', ':');
		domain->fwnode = fwnode;
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	}

	if (!domain->name) {
		if (fwnode)
			pr_err("Invalid fwnode type for irqdomain\n");
		domain->name = kasprintf(GFP_KERNEL, "unknown-%d",
					 atomic_inc_return(&unknown_domains));
		if (!domain->name) {
			kfree(domain);
			return NULL;
		}
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;
	}

	fwnode_handle_get(fwnode);
	fwnode_dev_initialized(fwnode, true);

	/* Fill structure */
	INIT_RADIX_TREE(&domain->revmap_tree, GFP_KERNEL);
	domain->ops = ops;
	domain->host_data = host_data;
	domain->hwirq_max = hwirq_max;

	if (direct_max)
		domain->flags |= IRQ_DOMAIN_FLAG_NO_MAP;

	domain->revmap_size = size;

	/*
	 * Hierarchical domains use the domain lock of the root domain
	 * (innermost domain).
	 *
	 * For non-hierarchical domains (as for root domains), the root
	 * pointer is set to the domain itself so that &domain->root->mutex
	 * always points to the right lock.
	 */
	mutex_init(&domain->mutex);
	domain->root = domain;

	irq_domain_check_hierarchy(domain);

	return domain;
}

static void __irq_domain_publish(struct irq_domain *domain)
{
	mutex_lock(&irq_domain_mutex);
	debugfs_add_domain_dir(domain);
	list_add(&domain->link, &irq_domain_list);
	mutex_unlock(&irq_domain_mutex);

	pr_debug("Added domain %s\n", domain->name);
}

/**
 * __irq_domain_add() - Allocate a new irq_domain data structure
 * @fwnode: firmware node for the interrupt controller
 * @size: Size of linear map; 0 for radix mapping only
 * @hwirq_max: Maximum number of interrupts supported by controller
 * @direct_max: Maximum value of direct maps; Use ~0 for no limit; 0 for no
 *              direct mapping
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates and initializes an irq_domain structure.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
struct irq_domain *__irq_domain_add(struct fwnode_handle *fwnode, unsigned int size,
				    irq_hw_number_t hwirq_max, int direct_max,
				    const struct irq_domain_ops *ops,
				    void *host_data)
{
	struct irq_domain *domain;

	domain = __irq_domain_create(fwnode, size, hwirq_max, direct_max,
				     ops, host_data);//创建一个中断域（irq_domain），指定固件节点、大小、中断号上限和域的操作函数
	if (domain)
		__irq_domain_publish(domain);//如果创建成功，发布该中断域，使其对其他部分可见

	return domain;
}
EXPORT_SYMBOL_GPL(__irq_domain_add);

/**
 * irq_domain_remove() - Remove an irq domain.
 * @domain: domain to remove
 *
 * This routine is used to remove an irq domain. The caller must ensure
 * that all mappings within the domain have been disposed of prior to
 * use, depending on the revmap type.
 */
void irq_domain_remove(struct irq_domain *domain)
{
	mutex_lock(&irq_domain_mutex);
	debugfs_remove_domain_dir(domain);

	WARN_ON(!radix_tree_empty(&domain->revmap_tree));

	list_del(&domain->link);

	/*
	 * If the going away domain is the default one, reset it.
	 */
	if (unlikely(irq_default_domain == domain))
		irq_set_default_host(NULL);

	mutex_unlock(&irq_domain_mutex);

	pr_debug("Removed domain %s\n", domain->name);

	fwnode_dev_initialized(domain->fwnode, false);
	fwnode_handle_put(domain->fwnode);
	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	kfree(domain);
}
EXPORT_SYMBOL_GPL(irq_domain_remove);

void irq_domain_update_bus_token(struct irq_domain *domain,
				 enum irq_domain_bus_token bus_token)
{
	char *name;

	if (domain->bus_token == bus_token)
		return;

	mutex_lock(&irq_domain_mutex);

	domain->bus_token = bus_token;

	name = kasprintf(GFP_KERNEL, "%s-%d", domain->name, bus_token);
	if (!name) {
		mutex_unlock(&irq_domain_mutex);
		return;
	}

	debugfs_remove_domain_dir(domain);

	if (domain->flags & IRQ_DOMAIN_NAME_ALLOCATED)
		kfree(domain->name);
	else
		domain->flags |= IRQ_DOMAIN_NAME_ALLOCATED;

	domain->name = name;
	debugfs_add_domain_dir(domain);

	mutex_unlock(&irq_domain_mutex);
}
EXPORT_SYMBOL_GPL(irq_domain_update_bus_token);

/**
 * irq_domain_create_simple() - Register an irq_domain and optionally map a range of irqs
 * @fwnode: firmware node for the interrupt controller
 * @size: total number of irqs in mapping
 * @first_irq: first number of irq block assigned to the domain,
 *	pass zero to assign irqs on-the-fly. If first_irq is non-zero, then
 *	pre-map all of the irqs in the domain to virqs starting at first_irq.
 * @ops: domain callbacks
 * @host_data: Controller private data pointer
 *
 * Allocates an irq_domain, and optionally if first_irq is positive then also
 * allocate irq_descs and map all of the hwirqs to virqs starting at first_irq.
 *
 * This is intended to implement the expected behaviour for most
 * interrupt controllers. If device tree is used, then first_irq will be 0 and
 * irqs get mapped dynamically on the fly. However, if the controller requires
 * static virq assignments (non-DT boot) then it will set that up correctly.
 */
struct irq_domain *irq_domain_create_simple(struct fwnode_handle *fwnode,
					    unsigned int size,
					    unsigned int first_irq,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain *domain;

	domain = __irq_domain_add(fwnode, size, size, 0, ops, host_data);
	if (!domain)
		return NULL;

	if (first_irq > 0) {
		if (IS_ENABLED(CONFIG_SPARSE_IRQ)) {
			/* attempt to allocated irq_descs */
			int rc = irq_alloc_descs(first_irq, first_irq, size,
						 of_node_to_nid(to_of_node(fwnode)));
			if (rc < 0)
				pr_info("Cannot allocate irq_descs @ IRQ%d, assuming pre-allocated\n",
					first_irq);
		}
		irq_domain_associate_many(domain, first_irq, 0, size);
	}

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_simple);

/**
 * irq_domain_add_legacy() - Allocate and register a legacy revmap irq_domain.
 * @of_node: pointer to interrupt controller's device tree node.
 * @size: total number of irqs in legacy mapping
 * @first_irq: first number of irq block assigned to the domain
 * @first_hwirq: first hwirq number to use for the translation. Should normally
 *               be '0', but a positive integer can be used if the effective
 *               hwirqs numbering does not begin at zero.
 * @ops: map/unmap domain callbacks
 * @host_data: Controller private data pointer
 *
 * Note: the map() callback will be called before this function returns
 * for all legacy interrupts except 0 (which is always the invalid irq for
 * a legacy controller).
 */
struct irq_domain *irq_domain_add_legacy(struct device_node *of_node,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	return irq_domain_create_legacy(of_node_to_fwnode(of_node), size,
					first_irq, first_hwirq, ops, host_data);
}
EXPORT_SYMBOL_GPL(irq_domain_add_legacy);

struct irq_domain *irq_domain_create_legacy(struct fwnode_handle *fwnode,
					 unsigned int size,
					 unsigned int first_irq,
					 irq_hw_number_t first_hwirq,
					 const struct irq_domain_ops *ops,
					 void *host_data)
{
	struct irq_domain *domain;

	domain = __irq_domain_add(fwnode, first_hwirq + size, first_hwirq + size, 0, ops, host_data);
	if (domain)
		irq_domain_associate_many(domain, first_irq, first_hwirq, size);

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_legacy);

/**
 * irq_find_matching_fwspec() - Locates a domain for a given fwspec
 * @fwspec: FW specifier for an interrupt
 * @bus_token: domain-specific data
 */
struct irq_domain *irq_find_matching_fwspec(struct irq_fwspec *fwspec,
					    enum irq_domain_bus_token bus_token)
{
	struct irq_domain *h, *found = NULL;
	struct fwnode_handle *fwnode = fwspec->fwnode;//从 fwspec 中获取固件节点的句柄
	int rc;

	/* We might want to match the legacy controller last since
	 * it might potentially be set to match all interrupts in
	 * the absence of a device node. This isn't a problem so far
	 * yet though...
	 *
	 * bus_token == DOMAIN_BUS_ANY matches any domain, any other
	 * values must generate an exact match for the domain to be
	 * selected.
	 * 我们可能希望在最后匹配遗留控制器，因为在没有设备节点的情况下，
	 * 它可能会被设置为匹配所有中断。这目前还不是一个问题。
	 *
	 * bus_token == DOMAIN_BUS_ANY 表示匹配任何中断域，任何其他值则必
	 * 须与中断域的 bus_token 完全匹配。
	 */
	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(h, &irq_domain_list, link) {//遍历全局的中断域列表，查找匹配的中断域
		if (h->ops->select && bus_token != DOMAIN_BUS_ANY)// 如果中断域具有select方法，并且bus_token不是DOMAIN_BUS_ANY
			rc = h->ops->select(h, fwspec, bus_token);//调用 select 函数判断当前中断域是否匹配
		else if (h->ops->match)//如果没有 select 方法但有 match 方法，调用 match 判断是否匹配
			rc = h->ops->match(h, to_of_node(fwnode), bus_token);
		else
			rc = ((fwnode != NULL) && (h->fwnode == fwnode) &&
			      ((bus_token == DOMAIN_BUS_ANY) ||
			       (h->bus_token == bus_token)));//如果都没有 select 和 match 方法，使用默认的匹配逻辑

		if (rc) {//如果当前中断域匹配，将其存储到 found 并跳出循环
			found = h;
			break;
		}
	}
	mutex_unlock(&irq_domain_mutex);
	return found;
}
EXPORT_SYMBOL_GPL(irq_find_matching_fwspec);

/**
 * irq_set_default_host() - Set a "default" irq domain
 * @domain: default domain pointer
 *
 * For convenience, it's possible to set a "default" domain that will be used
 * whenever NULL is passed to irq_create_mapping(). It makes life easier for
 * platforms that want to manipulate a few hard coded interrupt numbers that
 * aren't properly represented in the device-tree.
 */
void irq_set_default_host(struct irq_domain *domain)
{
	pr_debug("Default domain set to @0x%p\n", domain);

	irq_default_domain = domain;
}
EXPORT_SYMBOL_GPL(irq_set_default_host);

/**
 * irq_get_default_host() - Retrieve the "default" irq domain
 *
 * Returns: the default domain, if any.
 *
 * Modern code should never use this. This should only be used on
 * systems that cannot implement a firmware->fwnode mapping (which
 * both DT and ACPI provide).
 */
struct irq_domain *irq_get_default_host(void)
{
	return irq_default_domain;
}
EXPORT_SYMBOL_GPL(irq_get_default_host);

static bool irq_domain_is_nomap(struct irq_domain *domain)
{
	return IS_ENABLED(CONFIG_IRQ_DOMAIN_NOMAP) &&
	       (domain->flags & IRQ_DOMAIN_FLAG_NO_MAP);
}

static void irq_domain_clear_mapping(struct irq_domain *domain,
				     irq_hw_number_t hwirq)
{
	lockdep_assert_held(&domain->root->mutex);

	if (irq_domain_is_nomap(domain))
		return;

	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], NULL);
	else
		radix_tree_delete(&domain->revmap_tree, hwirq);
}

static void irq_domain_set_mapping(struct irq_domain *domain,
				   irq_hw_number_t hwirq,
				   struct irq_data *irq_data)
{
	/*
	 * This also makes sure that all domains point to the same root when
	 * called from irq_domain_insert_irq() for each domain in a hierarchy.
	 */
	lockdep_assert_held(&domain->root->mutex);

	if (irq_domain_is_nomap(domain))
		return;

	if (hwirq < domain->revmap_size)
		rcu_assign_pointer(domain->revmap[hwirq], irq_data);
	else
		radix_tree_insert(&domain->revmap_tree, hwirq, irq_data);
}

static void irq_domain_disassociate(struct irq_domain *domain, unsigned int irq)
{
	struct irq_data *irq_data = irq_get_irq_data(irq);
	irq_hw_number_t hwirq;

	if (WARN(!irq_data || irq_data->domain != domain,
		 "virq%i doesn't exist; cannot disassociate\n", irq))
		return;

	hwirq = irq_data->hwirq;

	mutex_lock(&domain->root->mutex);

	irq_set_status_flags(irq, IRQ_NOREQUEST);

	/* remove chip and handler */
	irq_set_chip_and_handler(irq, NULL, NULL);

	/* Make sure it's completed */
	synchronize_irq(irq);

	/* Tell the PIC about it */
	if (domain->ops->unmap)
		domain->ops->unmap(domain, irq);
	smp_mb();

	irq_data->domain = NULL;
	irq_data->hwirq = 0;
	domain->mapcount--;

	/* Clear reverse map for this hwirq */
	irq_domain_clear_mapping(domain, hwirq);

	mutex_unlock(&domain->root->mutex);
}

static int irq_domain_associate_locked(struct irq_domain *domain, unsigned int virq,
				       irq_hw_number_t hwirq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);//获取与虚拟中断号 (virq) 关联的中断数据结构
	int ret;
	/*验证硬件中断号是否超出域的最大范围*/
	if (WARN(hwirq >= domain->hwirq_max,
		 "error: hwirq 0x%x is too large for %s\n", (int)hwirq, domain->name))
		return -EINVAL;//如果硬件中断号超出范围，返回 -EINVAL 表示无效参数
	/*验证虚拟中断号是否已经被分配*/
	if (WARN(!irq_data, "error: virq%i is not allocated", virq))
		return -EINVAL;//如果虚拟中断号未分配，返回 -EINVAL
	/*检查虚拟中断号是否已经与其他中断域相关联*/
	if (WARN(irq_data->domain, "error: virq%i is already associated", virq))
		return -EINVAL;

	irq_data->hwirq = hwirq;//设置硬件中断号
	irq_data->domain = domain;//设置中断域
	if (domain->ops->map) {//如果中断域有映射操作函数，则调用它进行映射
		ret = domain->ops->map(domain, virq, hwirq);//调用映射函数(riscv_intc_domain_map)，将虚拟中断号和硬件中断号映射
		if (ret != 0) {//如果映射失败
			/*
			 * If map() returns -EPERM, this interrupt is protected
			 * by the firmware or some other service and shall not
			 * be mapped. Don't bother telling the user about it.
			 * 如果映射函数返回 -EPERM，表示该中断受固件或其他服务保护，不需要报告给用户
			 */
			if (ret != -EPERM) {
				pr_info("%s didn't like hwirq-0x%lx to VIRQ%i mapping (rc=%d)\n",
				       domain->name, hwirq, virq, ret);//打印映射失败的相关信息
			}
			irq_data->domain = NULL;//映射失败，清除关联的中断域
			irq_data->hwirq = 0;//清除关联的硬件中断号
			return ret;//返回映射失败的错误码
		}
	}

	domain->mapcount++;//增加中断域的映射计数
	irq_domain_set_mapping(domain, hwirq, irq_data);//将虚拟中断号和硬件中断号的映射关系添加到中断域的映射表中
	irq_clear_status_flags(virq, IRQ_NOREQUEST);//清除中断描述符中的 IRQ_NOREQUEST 标志，表示该中断可以请求使用

	return 0;// 返回 0 表示操作成功
}

int irq_domain_associate(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq)
{
	int ret;

	mutex_lock(&domain->root->mutex);
	ret = irq_domain_associate_locked(domain, virq, hwirq);
	mutex_unlock(&domain->root->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(irq_domain_associate);

void irq_domain_associate_many(struct irq_domain *domain, unsigned int irq_base,
			       irq_hw_number_t hwirq_base, int count)
{
	struct device_node *of_node;
	int i;

	of_node = irq_domain_get_of_node(domain);
	pr_debug("%s(%s, irqbase=%i, hwbase=%i, count=%i)\n", __func__,
		of_node_full_name(of_node), irq_base, (int)hwirq_base, count);

	for (i = 0; i < count; i++)
		irq_domain_associate(domain, irq_base + i, hwirq_base + i);
}
EXPORT_SYMBOL_GPL(irq_domain_associate_many);

#ifdef CONFIG_IRQ_DOMAIN_NOMAP
/**
 * irq_create_direct_mapping() - Allocate an irq for direct mapping
 * @domain: domain to allocate the irq for or NULL for default domain
 *
 * This routine is used for irq controllers which can choose the hardware
 * interrupt numbers they generate. In such a case it's simplest to use
 * the linux irq as the hardware interrupt number. It still uses the linear
 * or radix tree to store the mapping, but the irq controller can optimize
 * the revmap path by using the hwirq directly.
 */
unsigned int irq_create_direct_mapping(struct irq_domain *domain)
{
	struct device_node *of_node;
	unsigned int virq;

	if (domain == NULL)
		domain = irq_default_domain;

	of_node = irq_domain_get_of_node(domain);
	virq = irq_alloc_desc_from(1, of_node_to_nid(of_node));
	if (!virq) {
		pr_debug("create_direct virq allocation failed\n");
		return 0;
	}
	if (virq >= domain->hwirq_max) {
		pr_err("ERROR: no free irqs available below %lu maximum\n",
			domain->hwirq_max);
		irq_free_desc(virq);
		return 0;
	}
	pr_debug("create_direct obtained virq %d\n", virq);

	if (irq_domain_associate(domain, virq, virq)) {
		irq_free_desc(virq);
		return 0;
	}

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_direct_mapping);
#endif

static unsigned int irq_create_mapping_affinity_locked(struct irq_domain *domain,
						       irq_hw_number_t hwirq,
						       const struct irq_affinity_desc *affinity)
{
	struct device_node *of_node = irq_domain_get_of_node(domain);//获取中断域对应的设备节点
	int virq;// 定义虚拟 IRQ 号

	pr_debug("irq_create_mapping(0x%p, 0x%lx)\n", domain, hwirq);//打印调试信息，显示尝试创建映射的中断域和硬件中断号

	/* Allocate a virtual interrupt number */
	virq = irq_domain_alloc_descs(-1, 1, hwirq, of_node_to_nid(of_node),
				      affinity);//分配虚拟中断号（virq）
	if (virq <= 0) {//如果分配失败，打印调试信息并返回 0
		pr_debug("-> virq allocation failed\n");
		return 0;
	}

	if (irq_domain_associate_locked(domain, virq, hwirq)) {//将虚拟中断号与中断域和硬件中断号关联起来
		irq_free_desc(virq);// 如果关联失败，释放虚拟中断描述符
		return 0;
	}

	pr_debug("irq %lu on domain %s mapped to virtual irq %u\n",
		hwirq, of_node_full_name(of_node), virq);//打印调试信息，显示成功映射的硬件中断号、域名和虚拟中断号

	return virq;//返回分配的虚拟中断号
}

/**
 * irq_create_mapping_affinity() - Map a hardware interrupt into linux irq space
 * @domain: domain owning this hardware interrupt or NULL for default domain
 * @hwirq: hardware irq number in that domain space
 * @affinity: irq affinity
 *
 * Only one mapping per hardware interrupt is permitted. Returns a linux
 * irq number.
 * If the sense/trigger is to be specified, set_irq_type() should be called
 * on the number returned from that call.
 */
unsigned int irq_create_mapping_affinity(struct irq_domain *domain,
					 irq_hw_number_t hwirq,
					 const struct irq_affinity_desc *affinity)
{
	int virq;//定义变量 virq，用于存储虚拟中断号

	/* Look for default domain if necessary */
	if (domain == NULL)//如果未指定中断域，则使用默认的中断域
		domain = irq_default_domain;
	if (domain == NULL) {//如果依然没有找到有效的中断域，触发警告并返回 0
		WARN(1, "%s(, %lx) called with NULL domain\n", __func__, hwirq);
		return 0;
	}

	mutex_lock(&domain->root->mutex);//加锁，保护中断域的根节点结构体，防止并发访问

	/* Check if mapping already exists */
	virq = irq_find_mapping(domain, hwirq);// 检查是否已经存在与该硬件中断号对应的虚拟中断映射
	if (virq) {
		pr_debug("existing mapping on virq %d\n", virq);
		goto out;//如果映射已经存在，直接跳到解锁部分并返回
	}

	virq = irq_create_mapping_affinity_locked(domain, hwirq, affinity);//创建与硬件中断号的映射，使用提供的亲和性信息
out:
	mutex_unlock(&domain->root->mutex);//解锁中断域的根节点结构体

	return virq;//返回虚拟中断号
}
EXPORT_SYMBOL_GPL(irq_create_mapping_affinity);

static int irq_domain_translate(struct irq_domain *d,
				struct irq_fwspec *fwspec,
				irq_hw_number_t *hwirq, unsigned int *type)
{
#ifdef CONFIG_IRQ_DOMAIN_HIERARCHY
	if (d->ops->translate)
		return d->ops->translate(d, fwspec, hwirq, type);
#endif
	if (d->ops->xlate)
		return d->ops->xlate(d, to_of_node(fwspec->fwnode),
				     fwspec->param, fwspec->param_count,
				     hwirq, type);

	/* If domain has no translation, then we assume interrupt line */
	*hwirq = fwspec->param[0];
	return 0;
}

void of_phandle_args_to_fwspec(struct device_node *np, const u32 *args,
			       unsigned int count, struct irq_fwspec *fwspec)
{
	int i;

	fwspec->fwnode = of_node_to_fwnode(np);
	fwspec->param_count = count;

	for (i = 0; i < count; i++)
		fwspec->param[i] = args[i];
}
EXPORT_SYMBOL_GPL(of_phandle_args_to_fwspec);

unsigned int irq_create_fwspec_mapping(struct irq_fwspec *fwspec)
{
	struct irq_domain *domain;
	struct irq_data *irq_data;
	irq_hw_number_t hwirq;
	unsigned int type = IRQ_TYPE_NONE;
	int virq;

	if (fwspec->fwnode) {
		domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_WIRED);
		if (!domain)
			domain = irq_find_matching_fwspec(fwspec, DOMAIN_BUS_ANY);
	} else {
		domain = irq_default_domain;
	}

	if (!domain) {
		pr_warn("no irq domain found for %s !\n",
			of_node_full_name(to_of_node(fwspec->fwnode)));
		return 0;
	}

	if (irq_domain_translate(domain, fwspec, &hwirq, &type))
		return 0;

	/*
	 * WARN if the irqchip returns a type with bits
	 * outside the sense mask set and clear these bits.
	 */
	if (WARN_ON(type & ~IRQ_TYPE_SENSE_MASK))
		type &= IRQ_TYPE_SENSE_MASK;

	mutex_lock(&domain->root->mutex);

	/*
	 * If we've already configured this interrupt,
	 * don't do it again, or hell will break loose.
	 */
	virq = irq_find_mapping(domain, hwirq);
	if (virq) {
		/*
		 * If the trigger type is not specified or matches the
		 * current trigger type then we are done so return the
		 * interrupt number.
		 */
		if (type == IRQ_TYPE_NONE || type == irq_get_trigger_type(virq))
			goto out;

		/*
		 * If the trigger type has not been set yet, then set
		 * it now and return the interrupt number.
		 */
		if (irq_get_trigger_type(virq) == IRQ_TYPE_NONE) {
			irq_data = irq_get_irq_data(virq);
			if (!irq_data) {
				virq = 0;
				goto out;
			}

			irqd_set_trigger_type(irq_data, type);
			goto out;
		}

		pr_warn("type mismatch, failed to map hwirq-%lu for %s!\n",
			hwirq, of_node_full_name(to_of_node(fwspec->fwnode)));
		virq = 0;
		goto out;
	}

	if (irq_domain_is_hierarchy(domain)) {
		if (irq_domain_is_msi_device(domain)) {
			mutex_unlock(&domain->root->mutex);
			virq = msi_device_domain_alloc_wired(domain, hwirq, type);
			mutex_lock(&domain->root->mutex);
		} else
			virq = irq_domain_alloc_irqs_locked(domain, -1, 1, NUMA_NO_NODE,
							    fwspec, false, NULL);
		if (virq <= 0) {
			virq = 0;
			goto out;
		}
	} else {
		/* Create mapping */
		virq = irq_create_mapping_affinity_locked(domain, hwirq, NULL);
		if (!virq)
			goto out;
	}

	irq_data = irq_get_irq_data(virq);
	if (WARN_ON(!irq_data)) {
		virq = 0;
		goto out;
	}

	/* Store trigger type */
	irqd_set_trigger_type(irq_data, type);
out:
	mutex_unlock(&domain->root->mutex);

	return virq;
}
EXPORT_SYMBOL_GPL(irq_create_fwspec_mapping);

unsigned int irq_create_of_mapping(struct of_phandle_args *irq_data)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(irq_data->np, irq_data->args,
				  irq_data->args_count, &fwspec);

	return irq_create_fwspec_mapping(&fwspec);
}
EXPORT_SYMBOL_GPL(irq_create_of_mapping);

/**
 * irq_dispose_mapping() - Unmap an interrupt
 * @virq: linux irq number of the interrupt to unmap
 */
void irq_dispose_mapping(unsigned int virq)
{
	struct irq_data *irq_data;
	struct irq_domain *domain;

	irq_data = virq ? irq_get_irq_data(virq) : NULL;
	if (!irq_data)
		return;

	domain = irq_data->domain;
	if (WARN_ON(domain == NULL))
		return;

	if (irq_domain_is_hierarchy(domain)) {
		irq_domain_free_one_irq(domain, virq);
	} else {
		irq_domain_disassociate(domain, virq);
		irq_free_desc(virq);
	}
}
EXPORT_SYMBOL_GPL(irq_dispose_mapping);

/**
 * __irq_resolve_mapping() - Find a linux irq from a hw irq number.
 * @domain: domain owning this hardware interrupt
 * @hwirq: hardware irq number in that domain space
 * @irq: optional pointer to return the Linux irq if required
 *
 * Returns the interrupt descriptor.
 * 用于从中断域（irq_domain）和硬件中断号（hwirq）获取对应的中断描述符（irq_desc）。
 */
struct irq_desc *__irq_resolve_mapping(struct irq_domain *domain,
				       irq_hw_number_t hwirq,
				       unsigned int *irq)
{
	struct irq_desc *desc = NULL;
	struct irq_data *data;

	/* Look for default domain if necessary */
	if (domain == NULL)//如果没有提供中断域，使用默认的中断域
		domain = irq_default_domain;
	if (domain == NULL)
		return desc;//如果仍没有有效的中断域，直接返回 NULL

	if (irq_domain_is_nomap(domain)) {//如果中断域是 nomap 类型（不映射），则直接查找 hwirq 是否在域的最大范围内
		if (hwirq < domain->hwirq_max) {// 检查硬件中断号是否小于中断域的最大值
			data = irq_domain_get_irq_data(domain, hwirq);// 获取与硬件中断号对应的 IRQ 数据
			if (data && data->hwirq == hwirq)// 确保获取的数据有效并且匹配
				desc = irq_data_to_desc(data);//将 IRQ 数据转换为 IRQ 描述符
			if (irq && desc)//如果传入了 irq 指针且找到描述符
				*irq = hwirq;//将硬件中断号赋值给 irq
		}

		return desc;//返回中断描述符（可能是 NULL）
	}

	rcu_read_lock();//获取 RCU 读锁，以确保在并发访问中安全地读取 revmap 数据
	/* Check if the hwirq is in the linear revmap. */
	if (hwirq < domain->revmap_size)//检查 hwirq 是否在线性 revmap 中
		data = rcu_dereference(domain->revmap[hwirq]);//使用 RCU 获取 revmap 中的 IRQ 数据
	else
		data = radix_tree_lookup(&domain->revmap_tree, hwirq);//如果不在线性 revmap 中，则在 radix 树中查找

	if (likely(data)) {//如果成功找到了 IRQ 数据，则获取中断描述符
		desc = irq_data_to_desc(data);// 将 IRQ 数据转换为中断描述符
		if (irq)//如果传入了 irq 指针
			*irq = data->irq;//将对应的虚拟 IRQ 号赋值给 irq
	}

	rcu_read_unlock();//释放 RCU 读锁
	return desc;
}
EXPORT_SYMBOL_GPL(__irq_resolve_mapping);

/**
 * irq_domain_xlate_onecell() - Generic xlate for direct one cell bindings
 *
 * Device Tree IRQ specifier translation function which works with one cell
 * bindings where the cell value maps directly to the hwirq number.
 */
int irq_domain_xlate_onecell(struct irq_domain *d, struct device_node *ctrlr,
			     const u32 *intspec, unsigned int intsize,
			     unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onecell);

/**
 * irq_domain_xlate_twocell() - Generic xlate for direct two cell bindings
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
int irq_domain_xlate_twocell(struct irq_domain *d, struct device_node *ctrlr,
			const u32 *intspec, unsigned int intsize,
			irq_hw_number_t *out_hwirq, unsigned int *out_type)
{
	struct irq_fwspec fwspec;

	of_phandle_args_to_fwspec(ctrlr, intspec, intsize, &fwspec);
	return irq_domain_translate_twocell(d, &fwspec, out_hwirq, out_type);
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_twocell);

/**
 * irq_domain_xlate_onetwocell() - Generic xlate for one or two cell bindings
 *
 * Device Tree IRQ specifier translation function which works with either one
 * or two cell bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 *
 * Note: don't use this function unless your interrupt controller explicitly
 * supports both one and two cell bindings.  For the majority of controllers
 * the _onecell() or _twocell() variants above should be used.
 */
int irq_domain_xlate_onetwocell(struct irq_domain *d,
				struct device_node *ctrlr,
				const u32 *intspec, unsigned int intsize,
				unsigned long *out_hwirq, unsigned int *out_type)
{
	if (WARN_ON(intsize < 1))
		return -EINVAL;
	*out_hwirq = intspec[0];
	if (intsize > 1)
		*out_type = intspec[1] & IRQ_TYPE_SENSE_MASK;
	else
		*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_xlate_onetwocell);

const struct irq_domain_ops irq_domain_simple_ops = {
	.xlate = irq_domain_xlate_onetwocell,
};
EXPORT_SYMBOL_GPL(irq_domain_simple_ops);

/**
 * irq_domain_translate_onecell() - Generic translate for direct one cell
 * bindings
 */
int irq_domain_translate_onecell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 1))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = IRQ_TYPE_NONE;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_onecell);

/**
 * irq_domain_translate_twocell() - Generic translate for direct two cell
 * bindings
 *
 * Device Tree IRQ specifier translation function which works with two cell
 * bindings where the cell values map directly to the hwirq number
 * and linux irq flags.
 */
int irq_domain_translate_twocell(struct irq_domain *d,
				 struct irq_fwspec *fwspec,
				 unsigned long *out_hwirq,
				 unsigned int *out_type)
{
	if (WARN_ON(fwspec->param_count < 2))
		return -EINVAL;
	*out_hwirq = fwspec->param[0];
	*out_type = fwspec->param[1] & IRQ_TYPE_SENSE_MASK;
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_translate_twocell);

int irq_domain_alloc_descs(int virq, unsigned int cnt, irq_hw_number_t hwirq,
			   int node, const struct irq_affinity_desc *affinity)
{
	unsigned int hint;// 定义用于选择虚拟中断号的提示值

	if (virq >= 0) {//如果指定了虚拟中断号，则直接尝试从给定的虚拟中断号开始分配
		virq = __irq_alloc_descs(virq, virq, cnt, node, THIS_MODULE,
					 affinity);
	} else {//如果未指定虚拟中断号，根据硬件中断号计算提示值
		hint = hwirq % nr_irqs;//使用硬件中断号对系统支持的最大中断数取模来计算提示值
		if (hint == 0)//如果提示值为 0，将其设置为 1，避免分配到无效的中断号
			hint++;
		virq = __irq_alloc_descs(-1, hint, cnt, node, THIS_MODULE,
					 affinity);//从计算出的提示值开始尝试分配虚拟中断号
		if (virq <= 0 && hint > 1) {//如果分配失败并且提示值大于 1，则从 1 开始再次尝试分配
			virq = __irq_alloc_descs(-1, 1, cnt, node, THIS_MODULE,
						 affinity);
		}
	}

	return virq;//返回分配到的虚拟中断号，或者失败的标志（<=0）
}

/**
 * irq_domain_reset_irq_data - Clear hwirq, chip and chip_data in @irq_data
 * @irq_data:	The pointer to irq_data
 */
void irq_domain_reset_irq_data(struct irq_data *irq_data)
{
	irq_data->hwirq = 0;
	irq_data->chip = &no_irq_chip;
	irq_data->chip_data = NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_reset_irq_data);

#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
/**
 * irq_domain_create_hierarchy - Add a irqdomain into the hierarchy
 * @parent:	Parent irq domain to associate with the new domain
 * @flags:	Irq domain flags associated to the domain
 * @size:	Size of the domain. See below
 * @fwnode:	Optional fwnode of the interrupt controller
 * @ops:	Pointer to the interrupt domain callbacks
 * @host_data:	Controller private data pointer
 *
 * If @size is 0 a tree domain is created, otherwise a linear domain.
 *
 * If successful the parent is associated to the new domain and the
 * domain flags are set.
 * Returns pointer to IRQ domain, or NULL on failure.
 */
struct irq_domain *irq_domain_create_hierarchy(struct irq_domain *parent,
					    unsigned int flags,
					    unsigned int size,
					    struct fwnode_handle *fwnode,
					    const struct irq_domain_ops *ops,
					    void *host_data)
{
	struct irq_domain *domain;

	if (size)
		domain = __irq_domain_create(fwnode, size, size, 0, ops, host_data);
	else
		domain = __irq_domain_create(fwnode, 0, ~0, 0, ops, host_data);

	if (domain) {
		if (parent)
			domain->root = parent->root;
		domain->parent = parent;
		domain->flags |= flags;

		__irq_domain_publish(domain);
	}

	return domain;
}
EXPORT_SYMBOL_GPL(irq_domain_create_hierarchy);

static void irq_domain_insert_irq(int virq)
{
	struct irq_data *data;

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;

		domain->mapcount++;
		irq_domain_set_mapping(domain, data->hwirq, data);
	}

	irq_clear_status_flags(virq, IRQ_NOREQUEST);
}

static void irq_domain_remove_irq(int virq)
{
	struct irq_data *data;

	irq_set_status_flags(virq, IRQ_NOREQUEST);
	irq_set_chip_and_handler(virq, NULL, NULL);
	synchronize_irq(virq);
	smp_mb();

	for (data = irq_get_irq_data(virq); data; data = data->parent_data) {
		struct irq_domain *domain = data->domain;
		irq_hw_number_t hwirq = data->hwirq;

		domain->mapcount--;
		irq_domain_clear_mapping(domain, hwirq);
	}
}

static struct irq_data *irq_domain_insert_irq_data(struct irq_domain *domain,
						   struct irq_data *child)
{
	struct irq_data *irq_data;

	irq_data = kzalloc_node(sizeof(*irq_data), GFP_KERNEL,
				irq_data_get_node(child));
	if (irq_data) {
		child->parent_data = irq_data;
		irq_data->irq = child->irq;
		irq_data->common = child->common;
		irq_data->domain = domain;
	}

	return irq_data;
}

static void __irq_domain_free_hierarchy(struct irq_data *irq_data)
{
	struct irq_data *tmp;

	while (irq_data) {
		tmp = irq_data;
		irq_data = irq_data->parent_data;
		kfree(tmp);
	}
}

static void irq_domain_free_irq_data(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data, *tmp;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		tmp = irq_data->parent_data;
		irq_data->parent_data = NULL;
		irq_data->domain = NULL;

		__irq_domain_free_hierarchy(tmp);
	}
}

/**
 * irq_domain_disconnect_hierarchy - Mark the first unused level of a hierarchy
 * @domain:	IRQ domain from which the hierarchy is to be disconnected
 * @virq:	IRQ number where the hierarchy is to be trimmed
 *
 * Marks the @virq level belonging to @domain as disconnected.
 * Returns -EINVAL if @virq doesn't have a valid irq_data pointing
 * to @domain.
 *
 * Its only use is to be able to trim levels of hierarchy that do not
 * have any real meaning for this interrupt, and that the driver marks
 * as such from its .alloc() callback.
 */
int irq_domain_disconnect_hierarchy(struct irq_domain *domain,
				    unsigned int virq)
{
	struct irq_data *irqd;

	irqd = irq_domain_get_irq_data(domain, virq);
	if (!irqd)
		return -EINVAL;

	irqd->chip = ERR_PTR(-ENOTCONN);
	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_disconnect_hierarchy);

static int irq_domain_trim_hierarchy(unsigned int virq)
{
	struct irq_data *tail, *irqd, *irq_data;

	irq_data = irq_get_irq_data(virq);
	tail = NULL;

	/* The first entry must have a valid irqchip */
	if (!irq_data->chip || IS_ERR(irq_data->chip))
		return -EINVAL;

	/*
	 * Validate that the irq_data chain is sane in the presence of
	 * a hierarchy trimming marker.
	 */
	for (irqd = irq_data->parent_data; irqd; irq_data = irqd, irqd = irqd->parent_data) {
		/* Can't have a valid irqchip after a trim marker */
		if (irqd->chip && tail)
			return -EINVAL;

		/* Can't have an empty irqchip before a trim marker */
		if (!irqd->chip && !tail)
			return -EINVAL;

		if (IS_ERR(irqd->chip)) {
			/* Only -ENOTCONN is a valid trim marker */
			if (PTR_ERR(irqd->chip) != -ENOTCONN)
				return -EINVAL;

			tail = irq_data;
		}
	}

	/* No trim marker, nothing to do */
	if (!tail)
		return 0;

	pr_info("IRQ%d: trimming hierarchy from %s\n",
		virq, tail->parent_data->domain->name);

	/* Sever the inner part of the hierarchy...  */
	irqd = tail;
	tail = tail->parent_data;
	irqd->parent_data = NULL;
	__irq_domain_free_hierarchy(tail);

	return 0;
}

static int irq_domain_alloc_irq_data(struct irq_domain *domain,
				     unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	struct irq_domain *parent;
	int i;

	/* The outermost irq_data is embedded in struct irq_desc */
	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_get_irq_data(virq + i);
		irq_data->domain = domain;

		for (parent = domain->parent; parent; parent = parent->parent) {
			irq_data = irq_domain_insert_irq_data(parent, irq_data);
			if (!irq_data) {
				irq_domain_free_irq_data(virq, i + 1);
				return -ENOMEM;
			}
		}
	}

	return 0;
}

/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data;

	for (irq_data = irq_get_irq_data(virq); irq_data;
	     irq_data = irq_data->parent_data)
		if (irq_data->domain == domain)
			return irq_data;

	return NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/**
 * irq_domain_set_hwirq_and_chip - Set hwirq and irqchip of @virq at @domain
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number
 * @hwirq:	The hwirq number
 * @chip:	The associated interrupt chip
 * @chip_data:	The associated chip data
 * 用于设置给定虚拟中断号（virq）在指定中断域（domain）中的硬件中断号（hwirq）以
 * 及其关联的中断控制器（irq_chip）和控制器数据（chip_data)
 */
int irq_domain_set_hwirq_and_chip(struct irq_domain *domain, unsigned int virq,
				  irq_hw_number_t hwirq,
				  const struct irq_chip *chip,
				  void *chip_data)
{
	struct irq_data *irq_data = irq_domain_get_irq_data(domain, virq);//获取中断域中的虚拟中断号对应的 irq_data 结构体

	if (!irq_data)
		return -ENOENT;

	irq_data->hwirq = hwirq;//设置 irq_data 中的硬件中断号
	irq_data->chip = (struct irq_chip *)(chip ? chip : &no_irq_chip);//设置 irq_data 中的中断控制器。如果 chip 为空，设置为 no_irq_chip，表示没有实际的中断控制器
	irq_data->chip_data = chip_data;//设置 irq_data 中的中断控制器相关数据

	return 0;//返回 0 表示成功完成设置
}
EXPORT_SYMBOL_GPL(irq_domain_set_hwirq_and_chip);

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_domain_set_hwirq_and_chip(domain, virq, hwirq, chip, chip_data);//设置中断描述符中的硬件中断号和中断芯片相关信息.设置完相关信息后表示硬中断号和软中断号已经管理成功
	__irq_set_handler(virq, handler, 0, handler_name);//设置中断的处理函数和处理函数的名称
	irq_set_handler_data(virq, handler_data);//设置处理函数的相关数据，供处理函数在处理中使用
}
EXPORT_SYMBOL(irq_domain_set_info);

/**
 * irq_domain_free_irqs_common - Clear irq_data and free the parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
void irq_domain_free_irqs_common(struct irq_domain *domain, unsigned int virq,
				 unsigned int nr_irqs)
{
	struct irq_data *irq_data;
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_data = irq_domain_get_irq_data(domain, virq + i);
		if (irq_data)
			irq_domain_reset_irq_data(irq_data);
	}
	irq_domain_free_irqs_parent(domain, virq, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_common);

/**
 * irq_domain_free_irqs_top - Clear handler and handler data, clear irqdata and free parent
 * @domain:	Interrupt domain to match
 * @virq:	IRQ number to start with
 * @nr_irqs:	The number of irqs to free
 */
void irq_domain_free_irqs_top(struct irq_domain *domain, unsigned int virq,
			      unsigned int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++) {
		irq_set_handler_data(virq + i, NULL);
		irq_set_handler(virq + i, NULL);
	}
	irq_domain_free_irqs_common(domain, virq, nr_irqs);
}

static void irq_domain_free_irqs_hierarchy(struct irq_domain *domain,
					   unsigned int irq_base,
					   unsigned int nr_irqs)
{
	unsigned int i;

	if (!domain->ops->free)
		return;

	for (i = 0; i < nr_irqs; i++) {
		if (irq_domain_get_irq_data(domain, irq_base + i))
			domain->ops->free(domain, irq_base + i, 1);
	}
}

int irq_domain_alloc_irqs_hierarchy(struct irq_domain *domain,
				    unsigned int irq_base,
				    unsigned int nr_irqs, void *arg)
{
	if (!domain->ops->alloc) {
		pr_debug("domain->ops->alloc() is NULL\n");
		return -ENOSYS;
	}

	return domain->ops->alloc(domain, irq_base, nr_irqs, arg);
}

static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	int i, ret, virq;

	if (realloc && irq_base >= 0) {
		virq = irq_base;
	} else {
		virq = irq_domain_alloc_descs(irq_base, nr_irqs, 0, node,
					      affinity);
		if (virq < 0) {
			pr_debug("cannot allocate IRQ(base %d, count %d)\n",
				 irq_base, nr_irqs);
			return virq;
		}
	}

	if (irq_domain_alloc_irq_data(domain, virq, nr_irqs)) {
		pr_debug("cannot allocate memory for IRQ%d\n", virq);
		ret = -ENOMEM;
		goto out_free_desc;
	}

	ret = irq_domain_alloc_irqs_hierarchy(domain, virq, nr_irqs, arg);
	if (ret < 0)
		goto out_free_irq_data;

	for (i = 0; i < nr_irqs; i++) {
		ret = irq_domain_trim_hierarchy(virq + i);
		if (ret)
			goto out_free_irq_data;
	}

	for (i = 0; i < nr_irqs; i++)
		irq_domain_insert_irq(virq + i);

	return virq;

out_free_irq_data:
	irq_domain_free_irq_data(virq, nr_irqs);
out_free_desc:
	irq_free_descs(virq, nr_irqs);
	return ret;
}

/**
 * __irq_domain_alloc_irqs - Allocate IRQs from domain
 * @domain:	domain to allocate from
 * @irq_base:	allocate specified IRQ number if irq_base >= 0
 * @nr_irqs:	number of IRQs to allocate
 * @node:	NUMA node id for memory allocation
 * @arg:	domain specific argument
 * @realloc:	IRQ descriptors have already been allocated if true
 * @affinity:	Optional irq affinity mask for multiqueue devices
 *
 * Allocate IRQ numbers and initialized all data structures to support
 * hierarchy IRQ domains.
 * Parameter @realloc is mainly to support legacy IRQs.
 * Returns error code or allocated IRQ number
 *
 * The whole process to setup an IRQ has been split into two steps.
 * The first step, __irq_domain_alloc_irqs(), is to allocate IRQ
 * descriptor and required hardware resources. The second step,
 * irq_domain_activate_irq(), is to program the hardware with preallocated
 * resources. In this way, it's easier to rollback when failing to
 * allocate resources.
 */
int __irq_domain_alloc_irqs(struct irq_domain *domain, int irq_base,
			    unsigned int nr_irqs, int node, void *arg,
			    bool realloc, const struct irq_affinity_desc *affinity)
{
	int ret;

	if (domain == NULL) {
		domain = irq_default_domain;
		if (WARN(!domain, "domain is NULL; cannot allocate IRQ\n"))
			return -EINVAL;
	}

	mutex_lock(&domain->root->mutex);
	ret = irq_domain_alloc_irqs_locked(domain, irq_base, nr_irqs, node, arg,
					   realloc, affinity);
	mutex_unlock(&domain->root->mutex);

	return ret;
}
EXPORT_SYMBOL_GPL(__irq_domain_alloc_irqs);

/* The irq_data was moved, fix the revmap to refer to the new location */
static void irq_domain_fix_revmap(struct irq_data *d)
{
	void __rcu **slot;

	lockdep_assert_held(&d->domain->root->mutex);

	if (irq_domain_is_nomap(d->domain))
		return;

	/* Fix up the revmap. */
	if (d->hwirq < d->domain->revmap_size) {
		/* Not using radix tree */
		rcu_assign_pointer(d->domain->revmap[d->hwirq], d);
	} else {
		slot = radix_tree_lookup_slot(&d->domain->revmap_tree, d->hwirq);
		if (slot)
			radix_tree_replace_slot(&d->domain->revmap_tree, slot, d);
	}
}

/**
 * irq_domain_push_irq() - Push a domain in to the top of a hierarchy.
 * @domain:	Domain to push.
 * @virq:	Irq to push the domain in to.
 * @arg:	Passed to the irq_domain_ops alloc() function.
 *
 * For an already existing irqdomain hierarchy, as might be obtained
 * via a call to pci_enable_msix(), add an additional domain to the
 * head of the processing chain.  Must be called before request_irq()
 * has been called.
 */
int irq_domain_push_irq(struct irq_domain *domain, int virq, void *arg)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	struct irq_data *parent_irq_data;
	struct irq_desc *desc;
	int rv = 0;

	/*
	 * Check that no action has been set, which indicates the virq
	 * is in a state where this function doesn't have to deal with
	 * races between interrupt handling and maintaining the
	 * hierarchy.  This will catch gross misuse.  Attempting to
	 * make the check race free would require holding locks across
	 * calls to struct irq_domain_ops->alloc(), which could lead
	 * to deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (WARN_ON(!irq_domain_is_hierarchy(domain)))
		return -EINVAL;

	if (!irq_data)
		return -EINVAL;

	if (domain->parent != irq_data->domain)
		return -EINVAL;

	parent_irq_data = kzalloc_node(sizeof(*parent_irq_data), GFP_KERNEL,
				       irq_data_get_node(irq_data));
	if (!parent_irq_data)
		return -ENOMEM;

	mutex_lock(&domain->root->mutex);

	/* Copy the original irq_data. */
	*parent_irq_data = *irq_data;

	/*
	 * Overwrite the irq_data, which is embedded in struct irq_desc, with
	 * values for this domain.
	 */
	irq_data->parent_data = parent_irq_data;
	irq_data->domain = domain;
	irq_data->mask = 0;
	irq_data->hwirq = 0;
	irq_data->chip = NULL;
	irq_data->chip_data = NULL;

	/* May (probably does) set hwirq, chip, etc. */
	rv = irq_domain_alloc_irqs_hierarchy(domain, virq, 1, arg);
	if (rv) {
		/* Restore the original irq_data. */
		*irq_data = *parent_irq_data;
		kfree(parent_irq_data);
		goto error;
	}

	irq_domain_fix_revmap(parent_irq_data);
	irq_domain_set_mapping(domain, irq_data->hwirq, irq_data);
error:
	mutex_unlock(&domain->root->mutex);

	return rv;
}
EXPORT_SYMBOL_GPL(irq_domain_push_irq);

/**
 * irq_domain_pop_irq() - Remove a domain from the top of a hierarchy.
 * @domain:	Domain to remove.
 * @virq:	Irq to remove the domain from.
 *
 * Undo the effects of a call to irq_domain_push_irq().  Must be
 * called either before request_irq() or after free_irq().
 */
int irq_domain_pop_irq(struct irq_domain *domain, int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);
	struct irq_data *parent_irq_data;
	struct irq_data *tmp_irq_data;
	struct irq_desc *desc;

	/*
	 * Check that no action is set, which indicates the virq is in
	 * a state where this function doesn't have to deal with races
	 * between interrupt handling and maintaining the hierarchy.
	 * This will catch gross misuse.  Attempting to make the check
	 * race free would require holding locks across calls to
	 * struct irq_domain_ops->free(), which could lead to
	 * deadlock, so we just do a simple check before starting.
	 */
	desc = irq_to_desc(virq);
	if (!desc)
		return -EINVAL;
	if (WARN_ON(desc->action))
		return -EBUSY;

	if (domain == NULL)
		return -EINVAL;

	if (!irq_data)
		return -EINVAL;

	tmp_irq_data = irq_domain_get_irq_data(domain, virq);

	/* We can only "pop" if this domain is at the top of the list */
	if (WARN_ON(irq_data != tmp_irq_data))
		return -EINVAL;

	if (WARN_ON(irq_data->domain != domain))
		return -EINVAL;

	parent_irq_data = irq_data->parent_data;
	if (WARN_ON(!parent_irq_data))
		return -EINVAL;

	mutex_lock(&domain->root->mutex);

	irq_data->parent_data = NULL;

	irq_domain_clear_mapping(domain, irq_data->hwirq);
	irq_domain_free_irqs_hierarchy(domain, virq, 1);

	/* Restore the original irq_data. */
	*irq_data = *parent_irq_data;

	irq_domain_fix_revmap(irq_data);

	mutex_unlock(&domain->root->mutex);

	kfree(parent_irq_data);

	return 0;
}
EXPORT_SYMBOL_GPL(irq_domain_pop_irq);

/**
 * irq_domain_free_irqs - Free IRQ number and associated data structures
 * @virq:	base IRQ number
 * @nr_irqs:	number of IRQs to free
 */
void irq_domain_free_irqs(unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *data = irq_get_irq_data(virq);
	struct irq_domain *domain;
	int i;

	if (WARN(!data || !data->domain || !data->domain->ops->free,
		 "NULL pointer, cannot free irq\n"))
		return;

	domain = data->domain;

	mutex_lock(&domain->root->mutex);
	for (i = 0; i < nr_irqs; i++)
		irq_domain_remove_irq(virq + i);
	irq_domain_free_irqs_hierarchy(domain, virq, nr_irqs);
	mutex_unlock(&domain->root->mutex);

	irq_domain_free_irq_data(virq, nr_irqs);
	irq_free_descs(virq, nr_irqs);
}

static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq)
{
	if (irq_domain_is_msi_device(domain))
		msi_device_domain_free_wired(domain, virq);
	else
		irq_domain_free_irqs(virq, 1);
}

/**
 * irq_domain_alloc_irqs_parent - Allocate interrupts from parent domain
 * @domain:	Domain below which interrupts must be allocated
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to allocate
 * @arg:	Allocation data (arch/domain specific)
 */
int irq_domain_alloc_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs,
				 void *arg)
{
	if (!domain->parent)
		return -ENOSYS;

	return irq_domain_alloc_irqs_hierarchy(domain->parent, irq_base,
					       nr_irqs, arg);
}
EXPORT_SYMBOL_GPL(irq_domain_alloc_irqs_parent);

/**
 * irq_domain_free_irqs_parent - Free interrupts from parent domain
 * @domain:	Domain below which interrupts must be freed
 * @irq_base:	Base IRQ number
 * @nr_irqs:	Number of IRQs to free
 */
void irq_domain_free_irqs_parent(struct irq_domain *domain,
				 unsigned int irq_base, unsigned int nr_irqs)
{
	if (!domain->parent)
		return;

	irq_domain_free_irqs_hierarchy(domain->parent, irq_base, nr_irqs);
}
EXPORT_SYMBOL_GPL(irq_domain_free_irqs_parent);

static void __irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irq_data && irq_data->domain) {
		struct irq_domain *domain = irq_data->domain;

		if (domain->ops->deactivate)
			domain->ops->deactivate(domain, irq_data);
		if (irq_data->parent_data)
			__irq_domain_deactivate_irq(irq_data->parent_data);
	}
}

static int __irq_domain_activate_irq(struct irq_data *irqd, bool reserve)
{
	int ret = 0;

	if (irqd && irqd->domain) {
		struct irq_domain *domain = irqd->domain;

		if (irqd->parent_data)
			ret = __irq_domain_activate_irq(irqd->parent_data,
							reserve);
		if (!ret && domain->ops->activate) {
			ret = domain->ops->activate(domain, irqd, reserve);
			/* Rollback in case of error */
			if (ret && irqd->parent_data)
				__irq_domain_deactivate_irq(irqd->parent_data);
		}
	}
	return ret;
}

/**
 * irq_domain_activate_irq - Call domain_ops->activate recursively to activate
 *			     interrupt
 * @irq_data:	Outermost irq_data associated with interrupt
 * @reserve:	If set only reserve an interrupt vector instead of assigning one
 *
 * This is the second step to call domain_ops->activate to program interrupt
 * controllers, so the interrupt could actually get delivered.
 */
int irq_domain_activate_irq(struct irq_data *irq_data, bool reserve)
{
	int ret = 0;

	if (!irqd_is_activated(irq_data))
		ret = __irq_domain_activate_irq(irq_data, reserve);
	if (!ret)
		irqd_set_activated(irq_data);
	return ret;
}

/**
 * irq_domain_deactivate_irq - Call domain_ops->deactivate recursively to
 *			       deactivate interrupt
 * @irq_data: outermost irq_data associated with interrupt
 *
 * It calls domain_ops->deactivate to program interrupt controllers to disable
 * interrupt delivery.
 */
void irq_domain_deactivate_irq(struct irq_data *irq_data)
{
	if (irqd_is_activated(irq_data)) {
		__irq_domain_deactivate_irq(irq_data);
		irqd_clr_activated(irq_data);
	}
}

static void irq_domain_check_hierarchy(struct irq_domain *domain)
{
	/* Hierarchy irq_domains must implement callback alloc() */
	if (domain->ops->alloc)
		domain->flags |= IRQ_DOMAIN_FLAG_HIERARCHY;
}
#else	/* CONFIG_IRQ_DOMAIN_HIERARCHY */
/**
 * irq_domain_get_irq_data - Get irq_data associated with @virq and @domain
 * @domain:	domain to match
 * @virq:	IRQ number to get irq_data
 */
struct irq_data *irq_domain_get_irq_data(struct irq_domain *domain,
					 unsigned int virq)
{
	struct irq_data *irq_data = irq_get_irq_data(virq);

	return (irq_data && irq_data->domain == domain) ? irq_data : NULL;
}
EXPORT_SYMBOL_GPL(irq_domain_get_irq_data);

/**
 * irq_domain_set_info - Set the complete data for a @virq in @domain
 * @domain:		Interrupt domain to match
 * @virq:		IRQ number
 * @hwirq:		The hardware interrupt number
 * @chip:		The associated interrupt chip
 * @chip_data:		The associated interrupt chip data
 * @handler:		The interrupt flow handler
 * @handler_data:	The interrupt flow handler data
 * @handler_name:	The interrupt handler name
 */
void irq_domain_set_info(struct irq_domain *domain, unsigned int virq,
			 irq_hw_number_t hwirq, const struct irq_chip *chip,
			 void *chip_data, irq_flow_handler_t handler,
			 void *handler_data, const char *handler_name)
{
	irq_set_chip_and_handler_name(virq, chip, handler, handler_name);
	irq_set_chip_data(virq, chip_data);
	irq_set_handler_data(virq, handler_data);
}

static int irq_domain_alloc_irqs_locked(struct irq_domain *domain, int irq_base,
					unsigned int nr_irqs, int node, void *arg,
					bool realloc, const struct irq_affinity_desc *affinity)
{
	return -EINVAL;
}

static void irq_domain_check_hierarchy(struct irq_domain *domain) { }
static void irq_domain_free_one_irq(struct irq_domain *domain, unsigned int virq) { }

#endif	/* CONFIG_IRQ_DOMAIN_HIERARCHY */

#ifdef CONFIG_GENERIC_IRQ_DEBUGFS
#include "internals.h"

static struct dentry *domain_dir;

static void
irq_domain_debug_show_one(struct seq_file *m, struct irq_domain *d, int ind)
{
	seq_printf(m, "%*sname:   %s\n", ind, "", d->name);
	seq_printf(m, "%*ssize:   %u\n", ind + 1, "", d->revmap_size);
	seq_printf(m, "%*smapped: %u\n", ind + 1, "", d->mapcount);
	seq_printf(m, "%*sflags:  0x%08x\n", ind +1 , "", d->flags);
	if (d->ops && d->ops->debug_show)
		d->ops->debug_show(m, d, NULL, ind + 1);
#ifdef	CONFIG_IRQ_DOMAIN_HIERARCHY
	if (!d->parent)
		return;
	seq_printf(m, "%*sparent: %s\n", ind + 1, "", d->parent->name);
	irq_domain_debug_show_one(m, d->parent, ind + 4);
#endif
}

static int irq_domain_debug_show(struct seq_file *m, void *p)
{
	struct irq_domain *d = m->private;

	/* Default domain? Might be NULL */
	if (!d) {
		if (!irq_default_domain)
			return 0;
		d = irq_default_domain;
	}
	irq_domain_debug_show_one(m, d, 0);
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(irq_domain_debug);

static void debugfs_add_domain_dir(struct irq_domain *d)
{
	if (!d->name || !domain_dir)
		return;
	debugfs_create_file(d->name, 0444, domain_dir, d,
			    &irq_domain_debug_fops);
}

static void debugfs_remove_domain_dir(struct irq_domain *d)
{
	debugfs_lookup_and_remove(d->name, domain_dir);
}

void __init irq_domain_debugfs_init(struct dentry *root)
{
	struct irq_domain *d;

	domain_dir = debugfs_create_dir("domains", root);

	debugfs_create_file("default", 0444, domain_dir, NULL,
			    &irq_domain_debug_fops);
	mutex_lock(&irq_domain_mutex);
	list_for_each_entry(d, &irq_domain_list, link)
		debugfs_add_domain_dir(d);
	mutex_unlock(&irq_domain_mutex);
}
#endif
