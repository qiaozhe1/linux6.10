// SPDX-License-Identifier: GPL-2.0+
/*
 *  Derived from arch/i386/kernel/irq.c
 *    Copyright (C) 1992 Linus Torvalds
 *  Adapted from arch/i386 by Gary Thomas
 *    Copyright (C) 1995-1996 Gary Thomas (gdt@linuxppc.org)
 *  Updated and modified by Cort Dougan <cort@fsmlabs.com>
 *    Copyright (C) 1996-2001 Cort Dougan
 *  Adapted for Power Macintosh by Paul Mackerras
 *    Copyright (C) 1996 Paul Mackerras (paulus@cs.anu.edu.au)
 *
 * This file contains the code used to make IRQ descriptions in the
 * device tree to actual irq numbers on an interrupt controller
 * driver.
 */

#define pr_fmt(fmt)	"OF: " fmt

#include <linux/device.h>
#include <linux/errno.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/of_irq.h>
#include <linux/string.h>
#include <linux/slab.h>

#include "of_private.h"

/**
 * irq_of_parse_and_map - Parse and map an interrupt into linux virq space
 * @dev: Device node of the device whose interrupt is to be mapped
 * @index: Index of the interrupt to map
 *
 * This function is a wrapper that chains of_irq_parse_one() and
 * irq_create_of_mapping() to make things easier to callers
 */
unsigned int irq_of_parse_and_map(struct device_node *dev, int index)
{
	struct of_phandle_args oirq;

	if (of_irq_parse_one(dev, index, &oirq))
		return 0;

	return irq_create_of_mapping(&oirq);
}
EXPORT_SYMBOL_GPL(irq_of_parse_and_map);

/**
 * of_irq_find_parent - Given a device node, find its interrupt parent node
 * @child: pointer to device node
 *
 * Return: A pointer to the interrupt parent node, or NULL if the interrupt
 * parent could not be determined.
 */
struct device_node *of_irq_find_parent(struct device_node *child)
{
	struct device_node *p;//声明一个指针，用于存储当前节点的父节点
	phandle parent;

	if (!of_node_get(child))//尝试增加子节点的引用计数，如果失败，返回 NULL
		return NULL;

	do {
		if (of_property_read_u32(child, "interrupt-parent", &parent)) {//尝试从子节点读取 "interrupt-parent" 属性以获取父节点的句柄
			p = of_get_parent(child);//如果没有 "interrupt-parent" 属性，则获取子节点的直接父节点
		} else	{
			if (of_irq_workarounds & OF_IMAP_NO_PHANDLE)// 如果存在 "interrupt-parent" 属性
				p = of_node_get(of_irq_dflt_pic);// 如果当前工作模式要求使用默认中断控制器，获取该控制器的节点
			else
				p = of_find_node_by_phandle(parent);//否则根据读取到的父节点句柄查找对应的设备节点
		}
		of_node_put(child);//释放当前子节点的引用，避免内存泄漏
		child = p;//将 child 更新为找到的父节点，准备下一次迭代
	} while (p && of_get_property(p, "#interrupt-cells", NULL) == NULL);//循环条件：继续查找直到找到一个有效的父节点且该节点有 "#interrupt-cells" 属性

	return p;//返回找到的父节点指针，如果未找到则为 NULL
}
EXPORT_SYMBOL_GPL(of_irq_find_parent);

/*
 * These interrupt controllers abuse interrupt-map for unspeakable
 * reasons and rely on the core code to *ignore* it (the drivers do
 * their own parsing of the property).
 *
 * If you think of adding to the list for something *new*, think
 * again. There is a high chance that you will be sent back to the
 * drawing board.
 */
static const char * const of_irq_imap_abusers[] = {
	"CBEA,platform-spider-pic",
	"sti,platform-spider-pic",
	"realtek,rtl-intc",
	"fsl,ls1021a-extirq",
	"fsl,ls1043a-extirq",
	"fsl,ls1088a-extirq",
	"renesas,rza1-irqc",
	NULL,
};

const __be32 *of_irq_parse_imap_parent(const __be32 *imap, int len, struct of_phandle_args *out_irq)
{
	u32 intsize, addrsize;
	struct device_node *np;

	/* Get the interrupt parent */
	if (of_irq_workarounds & OF_IMAP_NO_PHANDLE)
		np = of_node_get(of_irq_dflt_pic);
	else
		np = of_find_node_by_phandle(be32_to_cpup(imap));
	imap++;

	/* Check if not found */
	if (!np) {
		pr_debug(" -> imap parent not found !\n");
		return NULL;
	}

	/* Get #interrupt-cells and #address-cells of new parent */
	if (of_property_read_u32(np, "#interrupt-cells",
					&intsize)) {
		pr_debug(" -> parent lacks #interrupt-cells!\n");
		of_node_put(np);
		return NULL;
	}
	if (of_property_read_u32(np, "#address-cells",
					&addrsize))
		addrsize = 0;

	pr_debug(" -> intsize=%d, addrsize=%d\n",
		intsize, addrsize);

	/* Check for malformed properties */
	if (WARN_ON(addrsize + intsize > MAX_PHANDLE_ARGS)
		|| (len < (addrsize + intsize))) {
		of_node_put(np);
		return NULL;
	}

	pr_debug(" -> imaplen=%d\n", len);

	imap += addrsize + intsize;

	out_irq->np = np;
	for (int i = 0; i < intsize; i++)
		out_irq->args[i] = be32_to_cpup(imap - intsize + i);
	out_irq->args_count = intsize;

	return imap;
}

/**
 * of_irq_parse_raw - Low level interrupt tree parsing
 * @addr:	address specifier (start of "reg" property of the device) in be32 format
 * @out_irq:	structure of_phandle_args updated by this function
 *
 * This function is a low-level interrupt tree walking function. It
 * can be used to do a partial walk with synthetized reg and interrupts
 * properties, for example when resolving PCI interrupts when no device
 * node exist for the parent. It takes an interrupt specifier structure as
 * input, walks the tree looking for any interrupt-map properties, translates
 * the specifier for each map, and then returns the translated map.
 *
 * Return: 0 on success and a negative number on error
 */
int of_irq_parse_raw(const __be32 *addr, struct of_phandle_args *out_irq)
{
	struct device_node *ipar, *tnode, *old = NULL;
	__be32 initial_match_array[MAX_PHANDLE_ARGS];
	const __be32 *match_array = initial_match_array;
	const __be32 *tmp, dummy_imask[] = { [0 ... MAX_PHANDLE_ARGS] = cpu_to_be32(~0) };
	u32 intsize = 1, addrsize;
	int i, rc = -EINVAL;

#ifdef DEBUG
	of_print_phandle_args("of_irq_parse_raw: ", out_irq);
#endif

	ipar = of_node_get(out_irq->np);

	/* First get the #interrupt-cells property of the current cursor
	 * that tells us how to interpret the passed-in intspec. If there
	 * is none, we are nice and just walk up the tree
	 */
	do {
		if (!of_property_read_u32(ipar, "#interrupt-cells", &intsize))
			break;
		tnode = ipar;
		ipar = of_irq_find_parent(ipar);
		of_node_put(tnode);
	} while (ipar);
	if (ipar == NULL) {
		pr_debug(" -> no parent found !\n");
		goto fail;
	}

	pr_debug("of_irq_parse_raw: ipar=%pOF, size=%d\n", ipar, intsize);

	if (out_irq->args_count != intsize)
		goto fail;

	/* Look for this #address-cells. We have to implement the old linux
	 * trick of looking for the parent here as some device-trees rely on it
	 */
	old = of_node_get(ipar);
	do {
		tmp = of_get_property(old, "#address-cells", NULL);
		tnode = of_get_parent(old);
		of_node_put(old);
		old = tnode;
	} while (old && tmp == NULL);
	of_node_put(old);
	old = NULL;
	addrsize = (tmp == NULL) ? 2 : be32_to_cpu(*tmp);

	pr_debug(" -> addrsize=%d\n", addrsize);

	/* Range check so that the temporary buffer doesn't overflow */
	if (WARN_ON(addrsize + intsize > MAX_PHANDLE_ARGS)) {
		rc = -EFAULT;
		goto fail;
	}

	/* Precalculate the match array - this simplifies match loop */
	for (i = 0; i < addrsize; i++)
		initial_match_array[i] = addr ? addr[i] : 0;
	for (i = 0; i < intsize; i++)
		initial_match_array[addrsize + i] = cpu_to_be32(out_irq->args[i]);

	/* Now start the actual "proper" walk of the interrupt tree */
	while (ipar != NULL) {
		int imaplen, match;
		const __be32 *imap, *oldimap, *imask;
		struct device_node *newpar;
		/*
		 * Now check if cursor is an interrupt-controller and
		 * if it is then we are done, unless there is an
		 * interrupt-map which takes precedence except on one
		 * of these broken platforms that want to parse
		 * interrupt-map themselves for $reason.
		 */
		bool intc = of_property_read_bool(ipar, "interrupt-controller");

		imap = of_get_property(ipar, "interrupt-map", &imaplen);
		if (intc &&
		    (!imap || of_device_compatible_match(ipar, of_irq_imap_abusers))) {
			pr_debug(" -> got it !\n");
			return 0;
		}

		/*
		 * interrupt-map parsing does not work without a reg
		 * property when #address-cells != 0
		 */
		if (addrsize && !addr) {
			pr_debug(" -> no reg passed in when needed !\n");
			goto fail;
		}

		/* No interrupt map, check for an interrupt parent */
		if (imap == NULL) {
			pr_debug(" -> no map, getting parent\n");
			newpar = of_irq_find_parent(ipar);
			goto skiplevel;
		}
		imaplen /= sizeof(u32);

		/* Look for a mask */
		imask = of_get_property(ipar, "interrupt-map-mask", NULL);
		if (!imask)
			imask = dummy_imask;

		/* Parse interrupt-map */
		match = 0;
		while (imaplen > (addrsize + intsize + 1)) {
			/* Compare specifiers */
			match = 1;
			for (i = 0; i < (addrsize + intsize); i++, imaplen--)
				match &= !((match_array[i] ^ *imap++) & imask[i]);

			pr_debug(" -> match=%d (imaplen=%d)\n", match, imaplen);

			oldimap = imap;
			imap = of_irq_parse_imap_parent(oldimap, imaplen, out_irq);
			if (!imap)
				goto fail;

			match &= of_device_is_available(out_irq->np);
			if (match)
				break;

			of_node_put(out_irq->np);
			imaplen -= imap - oldimap;
			pr_debug(" -> imaplen=%d\n", imaplen);
		}
		if (!match) {
			if (intc) {
				/*
				 * The PASEMI Nemo is a known offender, so
				 * let's only warn for anyone else.
				 */
				WARN(!IS_ENABLED(CONFIG_PPC_PASEMI),
				     "%pOF interrupt-map failed, using interrupt-controller\n",
				     ipar);
				return 0;
			}

			goto fail;
		}

		/*
		 * Successfully parsed an interrupt-map translation; copy new
		 * interrupt specifier into the out_irq structure
		 */
		match_array = oldimap + 1;

		newpar = out_irq->np;
		intsize = out_irq->args_count;
		addrsize = (imap - match_array) - intsize;

		if (ipar == newpar) {
			pr_debug("%pOF interrupt-map entry to self\n", ipar);
			return 0;
		}

	skiplevel:
		/* Iterate again with new parent */
		pr_debug(" -> new parent: %pOF\n", newpar);
		of_node_put(ipar);
		ipar = newpar;
		newpar = NULL;
	}
	rc = -ENOENT; /* No interrupt-map found */

 fail:
	of_node_put(ipar);

	return rc;
}
EXPORT_SYMBOL_GPL(of_irq_parse_raw);

/**
 * of_irq_parse_one - Resolve an interrupt for a device
 * @device: the device whose interrupt is to be resolved
 * @index: index of the interrupt to resolve
 * @out_irq: structure of_phandle_args filled by this function
 *
 * This function resolves an interrupt for a node by walking the interrupt tree,
 * finding which interrupt controller node it is attached to, and returning the
 * interrupt specifier that can be used to retrieve a Linux IRQ number.
 */
int of_irq_parse_one(struct device_node *device, int index, struct of_phandle_args *out_irq)
{
	struct device_node *p;
	const __be32 *addr;
	u32 intsize;
	int i, res;

	pr_debug("of_irq_parse_one: dev=%pOF, index=%d\n", device, index);

	/* OldWorld mac stuff is "special", handle out of line */
	if (of_irq_workarounds & OF_IMAP_OLDWORLD_MAC)
		return of_irq_parse_oldworld(device, index, out_irq);

	/* Get the reg property (if any) */
	addr = of_get_property(device, "reg", NULL);

	/* Try the new-style interrupts-extended first */
	res = of_parse_phandle_with_args(device, "interrupts-extended",
					"#interrupt-cells", index, out_irq);
	if (!res)
		return of_irq_parse_raw(addr, out_irq);

	/* Look for the interrupt parent. */
	p = of_irq_find_parent(device);
	if (p == NULL)
		return -EINVAL;

	/* Get size of interrupt specifier */
	if (of_property_read_u32(p, "#interrupt-cells", &intsize)) {
		res = -EINVAL;
		goto out;
	}

	pr_debug(" parent=%pOF, intsize=%d\n", p, intsize);

	/* Copy intspec into irq structure */
	out_irq->np = p;
	out_irq->args_count = intsize;
	for (i = 0; i < intsize; i++) {
		res = of_property_read_u32_index(device, "interrupts",
						 (index * intsize) + i,
						 out_irq->args + i);
		if (res)
			goto out;
	}

	pr_debug(" intspec=%d\n", *out_irq->args);


	/* Check if there are any interrupt-map translations to process */
	res = of_irq_parse_raw(addr, out_irq);
 out:
	of_node_put(p);
	return res;
}
EXPORT_SYMBOL_GPL(of_irq_parse_one);

/**
 * of_irq_to_resource - Decode a node's IRQ and return it as a resource
 * @dev: pointer to device tree node
 * @index: zero-based index of the irq
 * @r: pointer to resource structure to return result into.
 */
int of_irq_to_resource(struct device_node *dev, int index, struct resource *r)
{
	int irq = of_irq_get(dev, index);

	if (irq < 0)
		return irq;

	/* Only dereference the resource if both the
	 * resource and the irq are valid. */
	if (r && irq) {
		const char *name = NULL;

		memset(r, 0, sizeof(*r));
		/*
		 * Get optional "interrupt-names" property to add a name
		 * to the resource.
		 */
		of_property_read_string_index(dev, "interrupt-names", index,
					      &name);

		r->start = r->end = irq;
		r->flags = IORESOURCE_IRQ | irqd_get_trigger_type(irq_get_irq_data(irq));
		r->name = name ? name : of_node_full_name(dev);
	}

	return irq;
}
EXPORT_SYMBOL_GPL(of_irq_to_resource);

/**
 * of_irq_get - Decode a node's IRQ and return it as a Linux IRQ number
 * @dev: pointer to device tree node
 * @index: zero-based index of the IRQ
 *
 * Return: Linux IRQ number on success, or 0 on the IRQ mapping failure, or
 * -EPROBE_DEFER if the IRQ domain is not yet created, or error code in case
 * of any other failure.
 */
int of_irq_get(struct device_node *dev, int index)
{
	int rc;
	struct of_phandle_args oirq;
	struct irq_domain *domain;

	rc = of_irq_parse_one(dev, index, &oirq);
	if (rc)
		return rc;

	domain = irq_find_host(oirq.np);
	if (!domain) {
		rc = -EPROBE_DEFER;
		goto out;
	}

	rc = irq_create_of_mapping(&oirq);
out:
	of_node_put(oirq.np);

	return rc;
}
EXPORT_SYMBOL_GPL(of_irq_get);

/**
 * of_irq_get_byname - Decode a node's IRQ and return it as a Linux IRQ number
 * @dev: pointer to device tree node
 * @name: IRQ name
 *
 * Return: Linux IRQ number on success, or 0 on the IRQ mapping failure, or
 * -EPROBE_DEFER if the IRQ domain is not yet created, or error code in case
 * of any other failure.
 */
int of_irq_get_byname(struct device_node *dev, const char *name)
{
	int index;

	if (unlikely(!name))
		return -EINVAL;

	index = of_property_match_string(dev, "interrupt-names", name);
	if (index < 0)
		return index;

	return of_irq_get(dev, index);
}
EXPORT_SYMBOL_GPL(of_irq_get_byname);

/**
 * of_irq_count - Count the number of IRQs a node uses
 * @dev: pointer to device tree node
 */
int of_irq_count(struct device_node *dev)
{
	struct of_phandle_args irq;
	int nr = 0;

	while (of_irq_parse_one(dev, nr, &irq) == 0)
		nr++;

	return nr;
}

/**
 * of_irq_to_resource_table - Fill in resource table with node's IRQ info
 * @dev: pointer to device tree node
 * @res: array of resources to fill in
 * @nr_irqs: the number of IRQs (and upper bound for num of @res elements)
 *
 * Return: The size of the filled in table (up to @nr_irqs).
 */
int of_irq_to_resource_table(struct device_node *dev, struct resource *res,
		int nr_irqs)
{
	int i;

	for (i = 0; i < nr_irqs; i++, res++)
		if (of_irq_to_resource(dev, i, res) <= 0)
			break;

	return i;
}
EXPORT_SYMBOL_GPL(of_irq_to_resource_table);

struct of_intc_desc {//描述设备树中断控制器的结构体
	struct list_head	list;//链表头，用于将多个中断控制器描述结构体连接在一起
	of_irq_init_cb_t	irq_init_cb;// 中断初始化回调函数，用于对中断控制器进行初始化
	struct device_node	*dev;//指向设备树节点的指针，表示中断控制器设备节点
	struct device_node	*interrupt_parent;//指向中断父节点的指针，表示该中断控制器的上一级父中断控制器
};

/**
 * of_irq_init - Scan and init matching interrupt controllers in DT
 * @matches: 0 terminated array of nodes to match and init function to call
 *
 * This function scans the device tree for matching interrupt controller nodes,
 * and calls their initialization functions in order with parents first.
 */
void __init of_irq_init(const struct of_device_id *matches)
{
	const struct of_device_id *match;//用于存储当前匹配的设备ID
	struct device_node *np, *parent = NULL;//np 指向当前设备节点，parent指向父设备节点
	struct of_intc_desc *desc, *temp_desc;//中断控制器描述符指针，用于描述中断控制器的信息
	struct list_head intc_desc_list, intc_parent_list;//链表用于存储中断控制器描述符和父节点描述符

	INIT_LIST_HEAD(&intc_desc_list);//初始化中断控制器描述符链表
	INIT_LIST_HEAD(&intc_parent_list);//初始化父链表

	for_each_matching_node_and_match(np, matches, &match) {//遍历所有与提供的 matches 参数匹配的设备节点
		if (!of_property_read_bool(np, "interrupt-controller") ||
				!of_device_is_available(np))//检查当前设备节点是否是中断控制器，并且是否可用
			continue;//如果不是中断控制器或者不可用，则跳过

		if (WARN(!match->data, "of_irq_init: no init function for %s\n",
			 match->compatible))//检查匹配项是否有对应的初始化函数，如果没有则发出警告
			continue;//没有初始化函数则跳过

		/*
		 * Here, we allocate and populate an of_intc_desc with the node
		 * pointer, interrupt-parent device_node etc.
		 * 为当前设备节点分配一个 of_intc_desc 描述符，并初始化相关信息。
		 *  kzalloc 会分配内存并清零。
		 */
		desc = kzalloc(sizeof(*desc), GFP_KERNEL);//分配内存
		if (!desc) {//如果内存分配失败
			of_node_put(np);//释放当前设备节点引用
			goto err;//跳转到错误处理
		}

		desc->irq_init_cb = match->data;//设置初始化回调函数，来自匹配项(riscv_intc_init)
		desc->dev = of_node_get(np);//获取当前设备节点的引用，增加引用计数
		/*
		 * interrupts-extended can reference multiple parent domains.
		 * Arbitrarily pick the first one; assume any other parents
		 * are the same distance away from the root irq controller.
		 * 处理 "interrupts-extended" 属性，以获取可能的多个父节点。
		 * 这里假设只取第一个父节点，假设其他父节点的结构与根中断控制器相同。
		 */
		desc->interrupt_parent = of_parse_phandle(np, "interrupts-extended", 0);//尝试解析扩展中断
		if (!desc->interrupt_parent)//如果没有找到父节点
			desc->interrupt_parent = of_irq_find_parent(np);//查找直接父节点
		if (desc->interrupt_parent == np) {//如果找到的父节点是当前节点，则说明没有有效的父节点
			of_node_put(desc->interrupt_parent);//释放父节点引用
			desc->interrupt_parent = NULL;//设置为NULL
		}
		list_add_tail(&desc->list, &intc_desc_list);//将当前描述符添加到中断控制器描述符链表中.(在链表尾部添加描述符)
	}

	/*
	 * The root irq controller is the one without an interrupt-parent.
	 * That one goes first, followed by the controllers that reference it,
	 * followed by the ones that reference the 2nd level controllers, etc.
	 * 处理没有中断父节点的根中断控制器. 这个循环会处理所有中断控制器，
	 * 优先处理根控制器及其子节点。
	 */
	while (!list_empty(&intc_desc_list)) {
		/*
		 * Process all controllers with the current 'parent'.
		 * First pass will be looking for NULL as the parent.
		 * The assumption is that NULL parent means a root controller.
		 * 遍历当前链表中所有的中断控制器描述符。
		 *  第一次遍历会寻找父节点为空的中断控制器。
		 */
		list_for_each_entry_safe(desc, temp_desc, &intc_desc_list, list) {
			int ret;//用于存储初始化回调的返回值

			if (desc->interrupt_parent != parent)//检查当前描述符的父节点是否与当前处理的父节点相同
				continue;//如果不相同，则继续遍历下一个描述符

			list_del(&desc->list);//从链表中删除当前描述符

			of_node_set_flag(desc->dev, OF_POPULATED);//标记当前设备节点为已填充，表示已经被处理过

			pr_debug("of_irq_init: init %pOF (%p), parent %p\n",
				 desc->dev,
				 desc->dev, desc->interrupt_parent);//调试输出
			ret = desc->irq_init_cb(desc->dev,
						desc->interrupt_parent);//调用中断初始化回调函数(riscv_intc_init)，传入设备节点和父节点
			if (ret) {//如果初始化失败
				pr_err("%s: Failed to init %pOF (%p), parent %p\n",
				       __func__, desc->dev, desc->dev,
				       desc->interrupt_parent);//输出错误信息，说明初始化失败
				of_node_clear_flag(desc->dev, OF_POPULATED);//清除填充标志
				kfree(desc);//释放描述符内存
				continue;//继续下一个描述符
			}

			/*
			 * This one is now set up; add it to the parent list so
			 * its children can get processed in a subsequent pass.
			 * 当前描述符初始化成功，添加到父链表中，以便处理其子节点
			 */
			list_add_tail(&desc->list, &intc_parent_list);//将描述符添加到父链表
		}

		/* Get the next pending parent that might have children */
		desc = list_first_entry_or_null(&intc_parent_list,
						typeof(*desc), list);//获取下一个可能有子节点的父节点
		if (!desc) {//如果没有找到下一个父节点
			pr_err("of_irq_init: children remain, but no parents\n");// 输出错误信息
			break;//跳出循环
		}
		list_del(&desc->list);//从父链表中删除该父节点
		parent = desc->dev;//更新当前父节点
		kfree(desc);//释放该父节点的描述符
	}

	list_for_each_entry_safe(desc, temp_desc, &intc_parent_list, list) {//清理父链表中的所有描述符，释放内存
		list_del(&desc->list);//从链表中删除描述符
		kfree(desc);//释放描述符内存
	}
err:
	list_for_each_entry_safe(desc, temp_desc, &intc_desc_list, list) {//错误处理部分：清理中断控制器描述符链表中的所有描述符
		list_del(&desc->list);//从链表中删除描述符
		of_node_put(desc->dev);//释放设备节点引用，减少引用计数
		kfree(desc);//释放描述符内存
	}
}

static u32 __of_msi_map_id(struct device *dev, struct device_node **np,
			    u32 id_in)
{
	struct device *parent_dev;
	u32 id_out = id_in;

	/*
	 * Walk up the device parent links looking for one with a
	 * "msi-map" property.
	 */
	for (parent_dev = dev; parent_dev; parent_dev = parent_dev->parent)
		if (!of_map_id(parent_dev->of_node, id_in, "msi-map",
				"msi-map-mask", np, &id_out))
			break;
	return id_out;
}

/**
 * of_msi_map_id - Map a MSI ID for a device.
 * @dev: device for which the mapping is to be done.
 * @msi_np: device node of the expected msi controller.
 * @id_in: unmapped MSI ID for the device.
 *
 * Walk up the device hierarchy looking for devices with a "msi-map"
 * property.  If found, apply the mapping to @id_in.
 *
 * Return: The mapped MSI ID.
 */
u32 of_msi_map_id(struct device *dev, struct device_node *msi_np, u32 id_in)
{
	return __of_msi_map_id(dev, &msi_np, id_in);
}

/**
 * of_msi_map_get_device_domain - Use msi-map to find the relevant MSI domain
 * @dev: device for which the mapping is to be done.
 * @id: Device ID.
 * @bus_token: Bus token
 *
 * Walk up the device hierarchy looking for devices with a "msi-map"
 * property.
 *
 * Returns: the MSI domain for this device (or NULL on failure)
 */
struct irq_domain *of_msi_map_get_device_domain(struct device *dev, u32 id,
						u32 bus_token)
{
	struct device_node *np = NULL;

	__of_msi_map_id(dev, &np, id);
	return irq_find_matching_host(np, bus_token);
}

/**
 * of_msi_get_domain - Use msi-parent to find the relevant MSI domain
 * @dev: device for which the domain is requested
 * @np: device node for @dev
 * @token: bus type for this domain
 *
 * Parse the msi-parent property (both the simple and the complex
 * versions), and returns the corresponding MSI domain.
 *
 * Returns: the MSI domain for this device (or NULL on failure).
 */
struct irq_domain *of_msi_get_domain(struct device *dev,
				     struct device_node *np,
				     enum irq_domain_bus_token token)
{
	struct device_node *msi_np;
	struct irq_domain *d;

	/* Check for a single msi-parent property */
	msi_np = of_parse_phandle(np, "msi-parent", 0);
	if (msi_np && !of_property_read_bool(msi_np, "#msi-cells")) {
		d = irq_find_matching_host(msi_np, token);
		if (!d)
			of_node_put(msi_np);
		return d;
	}

	if (token == DOMAIN_BUS_PLATFORM_MSI) {
		/* Check for the complex msi-parent version */
		struct of_phandle_args args;
		int index = 0;

		while (!of_parse_phandle_with_args(np, "msi-parent",
						   "#msi-cells",
						   index, &args)) {
			d = irq_find_matching_host(args.np, token);
			if (d)
				return d;

			of_node_put(args.np);
			index++;
		}
	}

	return NULL;
}
EXPORT_SYMBOL_GPL(of_msi_get_domain);

/**
 * of_msi_configure - Set the msi_domain field of a device
 * @dev: device structure to associate with an MSI irq domain
 * @np: device node for that device
 */
void of_msi_configure(struct device *dev, struct device_node *np)
{
	dev_set_msi_domain(dev,
			   of_msi_get_domain(dev, np, DOMAIN_BUS_PLATFORM_MSI));
}
EXPORT_SYMBOL_GPL(of_msi_configure);
