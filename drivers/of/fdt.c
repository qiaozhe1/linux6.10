// SPDX-License-Identifier: GPL-2.0
/*
 * Functions for working with the Flattened Device Tree data format
 *
 * Copyright 2009 Benjamin Herrenschmidt, IBM Corp
 * benh@kernel.crashing.org
 */

#define pr_fmt(fmt)	"OF: fdt: " fmt

#include <linux/acpi.h>
#include <linux/crash_dump.h>
#include <linux/crc32.h>
#include <linux/kernel.h>
#include <linux/initrd.h>
#include <linux/memblock.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/of_fdt.h>
#include <linux/sizes.h>
#include <linux/string.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/libfdt.h>
#include <linux/debugfs.h>
#include <linux/serial_core.h>
#include <linux/sysfs.h>
#include <linux/random.h>

#include <asm/setup.h>  /* for COMMAND_LINE_SIZE */
#include <asm/page.h>

#include "of_private.h"

/*
 * __dtb_empty_root_begin[] and __dtb_empty_root_end[] magically created by
 * cmd_dt_S_dtb in scripts/Makefile.lib
 */
extern uint8_t __dtb_empty_root_begin[];
extern uint8_t __dtb_empty_root_end[];

/*
 * of_fdt_limit_memory - limit the number of regions in the /memory node
 * @limit: maximum entries
 *
 * Adjust the flattened device tree to have at most 'limit' number of
 * memory entries in the /memory node. This function may be called
 * any time after initial_boot_param is set.
 */
void __init of_fdt_limit_memory(int limit)
{
	int memory;
	int len;
	const void *val;
	int nr_address_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;
	int nr_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;
	const __be32 *addr_prop;
	const __be32 *size_prop;
	int root_offset;
	int cell_size;

	root_offset = fdt_path_offset(initial_boot_params, "/");
	if (root_offset < 0)
		return;

	addr_prop = fdt_getprop(initial_boot_params, root_offset,
				"#address-cells", NULL);
	if (addr_prop)
		nr_address_cells = fdt32_to_cpu(*addr_prop);

	size_prop = fdt_getprop(initial_boot_params, root_offset,
				"#size-cells", NULL);
	if (size_prop)
		nr_size_cells = fdt32_to_cpu(*size_prop);

	cell_size = sizeof(uint32_t)*(nr_address_cells + nr_size_cells);

	memory = fdt_path_offset(initial_boot_params, "/memory");
	if (memory > 0) {
		val = fdt_getprop(initial_boot_params, memory, "reg", &len);
		if (len > limit*cell_size) {
			len = limit*cell_size;
			pr_debug("Limiting number of entries to %d\n", limit);
			fdt_setprop(initial_boot_params, memory, "reg", val,
					len);
		}
	}
}

bool of_fdt_device_is_available(const void *blob, unsigned long node)//用于检查设备树中指定设备的可用性
{
	const char *status = fdt_getprop(blob, node, "status", NULL);//从设备树中获取指定节点的"status"属性

	if (!status)//如果没有找到"status"属性，默认认为设备可用
		return true;

	if (!strcmp(status, "ok") || !strcmp(status, "okay"))//检查"status"属性的值是否为"ok"或"okay"
		return true;//如果状态是"ok"或"okay"，设备可用

	return false;//其他情况下，设备不可用
}

static void *unflatten_dt_alloc(void **mem, unsigned long size,
				       unsigned long align)
{
	void *res;

	*mem = PTR_ALIGN(*mem, align);
	res = *mem;
	*mem += size;

	return res;
}

static void populate_properties(const void *blob,
				int offset,
				void **mem,
				struct device_node *np,
				const char *nodename,
				bool dryrun)
{
	struct property *pp, **pprev = NULL;
	int cur;
	bool has_name = false;

	pprev = &np->properties;
	for (cur = fdt_first_property_offset(blob, offset);
	     cur >= 0;
	     cur = fdt_next_property_offset(blob, cur)) {
		const __be32 *val;
		const char *pname;
		u32 sz;

		val = fdt_getprop_by_offset(blob, cur, &pname, &sz);
		if (!val) {
			pr_warn("Cannot locate property at 0x%x\n", cur);
			continue;
		}

		if (!pname) {
			pr_warn("Cannot find property name at 0x%x\n", cur);
			continue;
		}

		if (!strcmp(pname, "name"))
			has_name = true;

		pp = unflatten_dt_alloc(mem, sizeof(struct property),
					__alignof__(struct property));
		if (dryrun)
			continue;

		/* We accept flattened tree phandles either in
		 * ePAPR-style "phandle" properties, or the
		 * legacy "linux,phandle" properties.  If both
		 * appear and have different values, things
		 * will get weird. Don't do that.
		 */
		if (!strcmp(pname, "phandle") ||
		    !strcmp(pname, "linux,phandle")) {
			if (!np->phandle)
				np->phandle = be32_to_cpup(val);
		}

		/* And we process the "ibm,phandle" property
		 * used in pSeries dynamic device tree
		 * stuff
		 */
		if (!strcmp(pname, "ibm,phandle"))
			np->phandle = be32_to_cpup(val);

		pp->name   = (char *)pname;
		pp->length = sz;
		pp->value  = (__be32 *)val;
		*pprev     = pp;
		pprev      = &pp->next;
	}

	/* With version 0x10 we may not have the name property,
	 * recreate it here from the unit name if absent
	 */
	if (!has_name) {
		const char *p = nodename, *ps = p, *pa = NULL;
		int len;

		while (*p) {
			if ((*p) == '@')
				pa = p;
			else if ((*p) == '/')
				ps = p + 1;
			p++;
		}

		if (pa < ps)
			pa = p;
		len = (pa - ps) + 1;
		pp = unflatten_dt_alloc(mem, sizeof(struct property) + len,
					__alignof__(struct property));
		if (!dryrun) {
			pp->name   = "name";
			pp->length = len;
			pp->value  = pp + 1;
			*pprev     = pp;
			memcpy(pp->value, ps, len - 1);
			((char *)pp->value)[len - 1] = 0;
			pr_debug("fixed up name for %s -> %s\n",
				 nodename, (char *)pp->value);
		}
	}
}

static int populate_node(const void *blob,
			  int offset,
			  void **mem,
			  struct device_node *dad,
			  struct device_node **pnp,
			  bool dryrun)
{
	struct device_node *np;
	const char *pathp;
	int len;

	pathp = fdt_get_name(blob, offset, &len);
	if (!pathp) {
		*pnp = NULL;
		return len;
	}

	len++;

	np = unflatten_dt_alloc(mem, sizeof(struct device_node) + len,
				__alignof__(struct device_node));
	if (!dryrun) {
		char *fn;
		of_node_init(np);
		np->full_name = fn = ((char *)np) + sizeof(*np);

		memcpy(fn, pathp, len);

		if (dad != NULL) {
			np->parent = dad;
			np->sibling = dad->child;
			dad->child = np;
		}
	}

	populate_properties(blob, offset, mem, np, pathp, dryrun);
	if (!dryrun) {
		np->name = of_get_property(np, "name", NULL);
		if (!np->name)
			np->name = "<NULL>";
	}

	*pnp = np;
	return 0;
}

static void reverse_nodes(struct device_node *parent)
{
	struct device_node *child, *next;

	/* In-depth first */
	child = parent->child;
	while (child) {
		reverse_nodes(child);

		child = child->sibling;
	}

	/* Reverse the nodes in the child list */
	child = parent->child;
	parent->child = NULL;
	while (child) {
		next = child->sibling;

		child->sibling = parent->child;
		parent->child = child;
		child = next;
	}
}

/**
 * unflatten_dt_nodes - Alloc and populate a device_node from the flat tree
 * @blob: The parent device tree blob
 * @mem: Memory chunk to use for allocating device nodes and properties
 * @dad: Parent struct device_node
 * @nodepp: The device_node tree created by the call
 *
 * Return: The size of unflattened device tree or error code
 */
static int unflatten_dt_nodes(const void *blob,
			      void *mem,
			      struct device_node *dad,
			      struct device_node **nodepp)
{
	struct device_node *root;
	int offset = 0, depth = 0, initial_depth = 0;
#define FDT_MAX_DEPTH	64
	struct device_node *nps[FDT_MAX_DEPTH];
	void *base = mem;
	bool dryrun = !base;
	int ret;

	if (nodepp)
		*nodepp = NULL;

	/*
	 * We're unflattening device sub-tree if @dad is valid. There are
	 * possibly multiple nodes in the first level of depth. We need
	 * set @depth to 1 to make fdt_next_node() happy as it bails
	 * immediately when negative @depth is found. Otherwise, the device
	 * nodes except the first one won't be unflattened successfully.
	 */
	if (dad)
		depth = initial_depth = 1;

	root = dad;
	nps[depth] = dad;

	for (offset = 0;
	     offset >= 0 && depth >= initial_depth;
	     offset = fdt_next_node(blob, offset, &depth)) {
		if (WARN_ON_ONCE(depth >= FDT_MAX_DEPTH - 1))
			continue;

		if (!IS_ENABLED(CONFIG_OF_KOBJ) &&
		    !of_fdt_device_is_available(blob, offset))
			continue;

		ret = populate_node(blob, offset, &mem, nps[depth],
				   &nps[depth+1], dryrun);
		if (ret < 0)
			return ret;

		if (!dryrun && nodepp && !*nodepp)
			*nodepp = nps[depth+1];
		if (!dryrun && !root)
			root = nps[depth+1];
	}

	if (offset < 0 && offset != -FDT_ERR_NOTFOUND) {
		pr_err("Error %d processing FDT\n", offset);
		return -EINVAL;
	}

	/*
	 * Reverse the child list. Some drivers assumes node order matches .dts
	 * node order
	 */
	if (!dryrun)
		reverse_nodes(root);

	return mem - base;
}

/**
 * __unflatten_device_tree - create tree of device_nodes from flat blob
 * @blob: The blob to expand
 * @dad: Parent device node
 * @mynodes: The device_node tree created by the call
 * @dt_alloc: An allocator that provides a virtual address to memory
 * for the resulting tree
 * @detached: if true set OF_DETACHED on @mynodes
 *
 * unflattens a device-tree, creating the tree of struct device_node. It also
 * fills the "name" and "type" pointers of the nodes so the normal device-tree
 * walking functions can be used.
 *
 * Return: NULL on failure or the memory chunk containing the unflattened
 * device tree on success.
 */
void *__unflatten_device_tree(const void *blob,
			      struct device_node *dad,
			      struct device_node **mynodes,
			      void *(*dt_alloc)(u64 size, u64 align),
			      bool detached)
{
	int size;
	void *mem;
	int ret;

	if (mynodes)
		*mynodes = NULL;

	pr_debug(" -> unflatten_device_tree()\n");

	if (!blob) {
		pr_debug("No device tree pointer\n");
		return NULL;
	}

	pr_debug("Unflattening device tree:\n");
	pr_debug("magic: %08x\n", fdt_magic(blob));
	pr_debug("size: %08x\n", fdt_totalsize(blob));
	pr_debug("version: %08x\n", fdt_version(blob));

	if (fdt_check_header(blob)) {
		pr_err("Invalid device tree blob header\n");
		return NULL;
	}

	/* First pass, scan for size */
	size = unflatten_dt_nodes(blob, NULL, dad, NULL);
	if (size <= 0)
		return NULL;

	size = ALIGN(size, 4);
	pr_debug("  size is %d, allocating...\n", size);

	/* Allocate memory for the expanded device tree */
	mem = dt_alloc(size + 4, __alignof__(struct device_node));
	if (!mem)
		return NULL;

	memset(mem, 0, size);

	*(__be32 *)(mem + size) = cpu_to_be32(0xdeadbeef);

	pr_debug("  unflattening %p...\n", mem);

	/* Second pass, do actual unflattening */
	ret = unflatten_dt_nodes(blob, mem, dad, mynodes);

	if (be32_to_cpup(mem + size) != 0xdeadbeef)
		pr_warn("End of tree marker overwritten: %08x\n",
			be32_to_cpup(mem + size));

	if (ret <= 0)
		return NULL;

	if (detached && mynodes && *mynodes) {
		of_node_set_flag(*mynodes, OF_DETACHED);
		pr_debug("unflattened tree is detached\n");
	}

	pr_debug(" <- unflatten_device_tree()\n");
	return mem;
}

static void *kernel_tree_alloc(u64 size, u64 align)
{
	return kzalloc(size, GFP_KERNEL);
}

static DEFINE_MUTEX(of_fdt_unflatten_mutex);

/**
 * of_fdt_unflatten_tree - create tree of device_nodes from flat blob
 * @blob: Flat device tree blob
 * @dad: Parent device node
 * @mynodes: The device tree created by the call
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 *
 * Return: NULL on failure or the memory chunk containing the unflattened
 * device tree on success.
 */
void *of_fdt_unflatten_tree(const unsigned long *blob,
			    struct device_node *dad,
			    struct device_node **mynodes)
{
	void *mem;

	mutex_lock(&of_fdt_unflatten_mutex);
	mem = __unflatten_device_tree(blob, dad, mynodes, &kernel_tree_alloc,
				      true);
	mutex_unlock(&of_fdt_unflatten_mutex);

	return mem;
}
EXPORT_SYMBOL_GPL(of_fdt_unflatten_tree);

/* Everything below here references initial_boot_params directly. */
int __initdata dt_root_addr_cells;
int __initdata dt_root_size_cells;

void *initial_boot_params __ro_after_init;

#ifdef CONFIG_OF_EARLY_FLATTREE

static u32 of_fdt_crc32;

/*
 * fdt_reserve_elfcorehdr() - reserves memory for elf core header
 *
 * This function reserves the memory occupied by an elf core header
 * described in the device tree. This region contains all the
 * information about primary kernel's core image and is used by a dump
 * capture kernel to access the system memory on primary kernel.
 */
static void __init fdt_reserve_elfcorehdr(void)
{
	if (!IS_ENABLED(CONFIG_CRASH_DUMP) || !elfcorehdr_size)
		return;

	if (memblock_is_region_reserved(elfcorehdr_addr, elfcorehdr_size)) {
		pr_warn("elfcorehdr is overlapped\n");
		return;
	}

	memblock_reserve(elfcorehdr_addr, elfcorehdr_size);

	pr_info("Reserving %llu KiB of memory at 0x%llx for elfcorehdr\n",
		elfcorehdr_size >> 10, elfcorehdr_addr);
}

/**
 * early_init_fdt_scan_reserved_mem() - create reserved memory regions
 *
 * This function grabs memory from early allocator for device exclusive use
 * defined in device tree structures. It should be called by arch specific code
 * once the early allocator (i.e. memblock) has been fully activated.
 * 用于在系统初始化早期阶段处理设备树（FDT, Flattened Device Tree）中定义的保留内存区域
 */
void __init early_init_fdt_scan_reserved_mem(void)
{
	int n;
	u64 base, size;

	if (!initial_boot_params)//引导参数未设置，则意味着没有设备树信息可供处理，函数直接返回。
		return;

	fdt_scan_reserved_mem();//扫描并处理设备树中指定的保留内存区域
	fdt_reserve_elfcorehdr();//保留 elfcorehdr (内核崩溃后的内存转储区域)

	/*
	 * 设备树中可能会有全局的 /memreserve/ 字段，明确指定内存中不应被操作系统使用的区域。
	 * 通过调用fdt_get_mem_rsv函数获取每个保留区域的基地址和大小，然后调用 
	 * memblock_reserve 保留这些区域。
	 */
	for (n = 0; ; n++) {
		fdt_get_mem_rsv(initial_boot_params, n, &base, &size);//从设备树中获取保留内存区域的信息
		if (!size)
			break;
		memblock_reserve(base, size);
	}

	fdt_init_reserved_mem();//初始化并处理在设备树中定义的其他保留内存区域
}

/**
 * early_init_fdt_reserve_self() - reserve the memory used by the FDT blob
 */
void __init early_init_fdt_reserve_self(void)
{
	if (!initial_boot_params)
		return;

	/* Reserve the dtb region */
	memblock_reserve(__pa(initial_boot_params),
			 fdt_totalsize(initial_boot_params));
}

/**
 * of_scan_flat_dt - scan flattened tree blob and call callback on each.
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan the flattened device-tree, it is
 * used to extract the memory information at boot before we can
 * unflatten the tree
 */
int __init of_scan_flat_dt(int (*it)(unsigned long node,
				     const char *uname, int depth,
				     void *data),
			   void *data)
{
	const void *blob = initial_boot_params;
	const char *pathp;
	int offset, rc = 0, depth = -1;

	if (!blob)
		return 0;

	for (offset = fdt_next_node(blob, -1, &depth);
	     offset >= 0 && depth >= 0 && !rc;
	     offset = fdt_next_node(blob, offset, &depth)) {

		pathp = fdt_get_name(blob, offset, NULL);
		rc = it(offset, pathp, depth, data);
	}
	return rc;
}

/**
 * of_scan_flat_dt_subnodes - scan sub-nodes of a node call callback on each.
 * @parent: parent node
 * @it: callback function
 * @data: context data pointer
 *
 * This function is used to scan sub-nodes of a node.
 */
int __init of_scan_flat_dt_subnodes(unsigned long parent,
				    int (*it)(unsigned long node,
					      const char *uname,
					      void *data),
				    void *data)
{
	const void *blob = initial_boot_params;
	int node;

	fdt_for_each_subnode(node, blob, parent) {
		const char *pathp;
		int rc;

		pathp = fdt_get_name(blob, node, NULL);
		rc = it(node, pathp, data);
		if (rc)
			return rc;
	}
	return 0;
}

/**
 * of_get_flat_dt_subnode_by_name - get the subnode by given name
 *
 * @node: the parent node
 * @uname: the name of subnode
 * @return offset of the subnode, or -FDT_ERR_NOTFOUND if there is none
 */

int __init of_get_flat_dt_subnode_by_name(unsigned long node, const char *uname)
{
	return fdt_subnode_offset(initial_boot_params, node, uname);
}

/*
 * of_get_flat_dt_root - find the root node in the flat blob
 */
unsigned long __init of_get_flat_dt_root(void)
{
	return 0;
}

/*
 * of_get_flat_dt_prop - Given a node in the flat blob, return the property ptr
 *
 * This function can be used within scan_flattened_dt callback to get
 * access to properties
 */
const void *__init of_get_flat_dt_prop(unsigned long node, const char *name,
				       int *size)
{
	return fdt_getprop(initial_boot_params, node, name, size);
}

/**
 * of_fdt_is_compatible - Return true if given node from the given blob has
 * compat in its compatible list
 * @blob: A device tree blob
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 *
 * Return: a non-zero value on match with smaller values returned for more
 * specific compatible values.
 */
static int of_fdt_is_compatible(const void *blob,
		      unsigned long node, const char *compat)
{
	const char *cp;
	int cplen;
	unsigned long l, score = 0;

	cp = fdt_getprop(blob, node, "compatible", &cplen);
	if (cp == NULL)
		return 0;
	while (cplen > 0) {
		score++;
		if (of_compat_cmp(cp, compat, strlen(compat)) == 0)
			return score;
		l = strlen(cp) + 1;
		cp += l;
		cplen -= l;
	}

	return 0;
}

/**
 * of_flat_dt_is_compatible - Return true if given node has compat in compatible list
 * @node: node to test
 * @compat: compatible string to compare with compatible list.
 */
int __init of_flat_dt_is_compatible(unsigned long node, const char *compat)
{
	return of_fdt_is_compatible(initial_boot_params, node, compat);
}

/*
 * of_flat_dt_match - Return true if node matches a list of compatible values
 */
static int __init of_flat_dt_match(unsigned long node, const char *const *compat)
{
	unsigned int tmp, score = 0;

	if (!compat)
		return 0;

	while (*compat) {
		tmp = of_fdt_is_compatible(initial_boot_params, node, *compat);
		if (tmp && (score == 0 || (tmp < score)))
			score = tmp;
		compat++;
	}

	return score;
}

/*
 * of_get_flat_dt_phandle - Given a node in the flat blob, return the phandle
 */
uint32_t __init of_get_flat_dt_phandle(unsigned long node)
{
	return fdt_get_phandle(initial_boot_params, node);
}

const char * __init of_flat_dt_get_machine_name(void)
{
	const char *name;
	unsigned long dt_root = of_get_flat_dt_root();

	name = of_get_flat_dt_prop(dt_root, "model", NULL);
	if (!name)
		name = of_get_flat_dt_prop(dt_root, "compatible", NULL);
	return name;
}

/**
 * of_flat_dt_match_machine - Iterate match tables to find matching machine.
 *
 * @default_match: A machine specific ptr to return in case of no match.
 * @get_next_compat: callback function to return next compatible match table.
 *
 * Iterate through machine match tables to find the best match for the machine
 * compatible string in the FDT.
 */
const void * __init of_flat_dt_match_machine(const void *default_match,
		const void * (*get_next_compat)(const char * const**))
{
	const void *data = NULL;
	const void *best_data = default_match;
	const char *const *compat;
	unsigned long dt_root;
	unsigned int best_score = ~1, score = 0;

	dt_root = of_get_flat_dt_root();
	while ((data = get_next_compat(&compat))) {
		score = of_flat_dt_match(dt_root, compat);
		if (score > 0 && score < best_score) {
			best_data = data;
			best_score = score;
		}
	}
	if (!best_data) {
		const char *prop;
		int size;

		pr_err("\n unrecognized device tree list:\n[ ");

		prop = of_get_flat_dt_prop(dt_root, "compatible", &size);
		if (prop) {
			while (size > 0) {
				printk("'%s' ", prop);
				size -= strlen(prop) + 1;
				prop += strlen(prop) + 1;
			}
		}
		printk("]\n\n");
		return NULL;
	}

	pr_info("Machine model: %s\n", of_flat_dt_get_machine_name());

	return best_data;
}

static void __early_init_dt_declare_initrd(unsigned long start,
					   unsigned long end)
{
	/*
	 * __va() is not yet available this early on some platforms. In that
	 * case, the platform uses phys_initrd_start/phys_initrd_size instead
	 * and does the VA conversion itself.
	 */
	if (!IS_ENABLED(CONFIG_ARM64) &&
	    !(IS_ENABLED(CONFIG_RISCV) && IS_ENABLED(CONFIG_64BIT))) {
		initrd_start = (unsigned long)__va(start);
		initrd_end = (unsigned long)__va(end);
		initrd_below_start_ok = 1;
	}
}

/**
 * early_init_dt_check_for_initrd - Decode initrd location from flat tree
 * @node: reference to node containing initrd location ('chosen')
 */
static void __init early_init_dt_check_for_initrd(unsigned long node)
{
	u64 start, end;
	int len;
	const __be32 *prop;

	if (!IS_ENABLED(CONFIG_BLK_DEV_INITRD))
		return;

	pr_debug("Looking for initrd properties... ");

	prop = of_get_flat_dt_prop(node, "linux,initrd-start", &len);
	if (!prop)
		return;
	start = of_read_number(prop, len/4);

	prop = of_get_flat_dt_prop(node, "linux,initrd-end", &len);
	if (!prop)
		return;
	end = of_read_number(prop, len/4);
	if (start > end)
		return;

	__early_init_dt_declare_initrd(start, end);
	phys_initrd_start = start;
	phys_initrd_size = end - start;

	pr_debug("initrd_start=0x%llx  initrd_end=0x%llx\n", start, end);
}

/**
 * early_init_dt_check_for_elfcorehdr - Decode elfcorehdr location from flat
 * tree
 * @node: reference to node containing elfcorehdr location ('chosen')
 */
static void __init early_init_dt_check_for_elfcorehdr(unsigned long node)
{
	const __be32 *prop;
	int len;

	if (!IS_ENABLED(CONFIG_CRASH_DUMP))
		return;

	pr_debug("Looking for elfcorehdr property... ");

	prop = of_get_flat_dt_prop(node, "linux,elfcorehdr", &len);
	if (!prop || (len < (dt_root_addr_cells + dt_root_size_cells)))
		return;

	elfcorehdr_addr = dt_mem_next_cell(dt_root_addr_cells, &prop);
	elfcorehdr_size = dt_mem_next_cell(dt_root_size_cells, &prop);

	pr_debug("elfcorehdr_start=0x%llx elfcorehdr_size=0x%llx\n",
		 elfcorehdr_addr, elfcorehdr_size);
}

static unsigned long chosen_node_offset = -FDT_ERR_NOTFOUND;

/*
 * The main usage of linux,usable-memory-range is for crash dump kernel.
 * Originally, the number of usable-memory regions is one. Now there may
 * be two regions, low region and high region.
 * To make compatibility with existing user-space and older kdump, the low
 * region is always the last range of linux,usable-memory-range if exist.
 */
#define MAX_USABLE_RANGES		2

/**
 * early_init_dt_check_for_usable_mem_range - Decode usable memory range
 * location from flat tree
 */
void __init early_init_dt_check_for_usable_mem_range(void)
{
	struct memblock_region rgn[MAX_USABLE_RANGES] = {0};//用于存储可用内存范围的数组
	const __be32 *prop, *endp;//指向设备树属性的指针
	int len, i;
	unsigned long node = chosen_node_offset;//指向设备树中 "chosen" 节点的偏移量

	if ((long)node < 0)//如果 "chosen" 节点不存在
		return;

	pr_debug("Looking for usable-memory-range property... ");//打印调试信息

	prop = of_get_flat_dt_prop(node, "linux,usable-memory-range", &len);//获取 "linux,usable-memory-range" 属性
	if (!prop || (len % (dt_root_addr_cells + dt_root_size_cells)))//如果属性不存在或长度不符合预期
		return;

	endp = prop + (len / sizeof(__be32));//计算属性数据的结束指针
	for (i = 0; i < MAX_USABLE_RANGES && prop < endp; i++) {//遍历属性值并提取内存区域（DTS文件中一个reg可能存在多个内存区域）
		rgn[i].base = dt_mem_next_cell(dt_root_addr_cells, &prop);//获取内存区域的基地址
		rgn[i].size = dt_mem_next_cell(dt_root_size_cells, &prop);//获取内存区域的大小

		pr_debug("cap_mem_regions[%d]: base=%pa, size=%pa\n",
			 i, &rgn[i].base, &rgn[i].size);//打印提取的内存区域
	}

	memblock_cap_memory_range(rgn[0].base, rgn[0].size);//限制系统使用第一个可用内存范围
	for (i = 1; i < MAX_USABLE_RANGES && rgn[i].size; i++)//如果有多个可用内存范围
		memblock_add(rgn[i].base, rgn[i].size);//将剩余的可用内存范围添加到memblock管理中
}

#ifdef CONFIG_SERIAL_EARLYCON

int __init early_init_dt_scan_chosen_stdout(void)
{
	int offset;
	const char *p, *q, *options = NULL;
	int l;
	const struct earlycon_id *match;
	const void *fdt = initial_boot_params;
	int ret;

	offset = fdt_path_offset(fdt, "/chosen");
	if (offset < 0)
		offset = fdt_path_offset(fdt, "/chosen@0");
	if (offset < 0)
		return -ENOENT;

	p = fdt_getprop(fdt, offset, "stdout-path", &l);
	if (!p)
		p = fdt_getprop(fdt, offset, "linux,stdout-path", &l);
	if (!p || !l)
		return -ENOENT;

	q = strchrnul(p, ':');
	if (*q != '\0')
		options = q + 1;
	l = q - p;

	/* Get the node specified by stdout-path */
	offset = fdt_path_offset_namelen(fdt, p, l);
	if (offset < 0) {
		pr_warn("earlycon: stdout-path %.*s not found\n", l, p);
		return 0;
	}

	for (match = __earlycon_table; match < __earlycon_table_end; match++) {
		if (!match->compatible[0])
			continue;

		if (fdt_node_check_compatible(fdt, offset, match->compatible))
			continue;

		ret = of_setup_earlycon(match, offset, options);
		if (!ret || ret == -EALREADY)
			return 0;
	}
	return -ENODEV;
}
#endif

/*
 * early_init_dt_scan_root - fetch the top level address and size cells
 */
int __init early_init_dt_scan_root(void)
{
	const __be32 *prop;
	const void *fdt = initial_boot_params;
	int node = fdt_path_offset(fdt, "/");//获取设备树中根节点的偏移量。

	if (node < 0)//如果根节点偏移量无效，则返回错误。
		return -ENODEV;

	dt_root_size_cells = OF_ROOT_NODE_SIZE_CELLS_DEFAULT;// 初始化根节点的 size-cells，默认为 2。
	dt_root_addr_cells = OF_ROOT_NODE_ADDR_CELLS_DEFAULT;//初始化根节点的 address-cells，默认为 2

	prop = of_get_flat_dt_prop(node, "#size-cells", NULL);//从根节点获取 "#size-cells" 属性
	if (prop)
		dt_root_size_cells = be32_to_cpup(prop);//如果获取到该属性，转换为 CPU 可读格式并赋值给 dt_root_size_cells。
	pr_debug("dt_root_size_cells = %x\n", dt_root_size_cells);//打印获取到的 size-cells 信息。

	prop = of_get_flat_dt_prop(node, "#address-cells", NULL);//从根节点获取 "#address-cells" 属性。
	if (prop)
		dt_root_addr_cells = be32_to_cpup(prop);//如果获取到该属性，转换为 CPU 可读格式并赋值给 dt_root_addr_cells
	pr_debug("dt_root_addr_cells = %x\n", dt_root_addr_cells);//打印获取到的 address-cells 信息。

	return 0;
}

u64 __init dt_mem_next_cell(int s, const __be32 **cellp)
{
	const __be32 *p = *cellp;

	*cellp = p + s;
	return of_read_number(p, s);
}

/*
 * early_init_dt_scan_memory - Look for and parse memory nodes
 * 扫描设备树中的内存节点，并将其添加到memblock子系统中
 */
int __init early_init_dt_scan_memory(void)
{
	int node, found_memory = 0;//node 用于迭代节点，found_memory 标记是否找到内存节点
	const void *fdt = initial_boot_params;//获取设备树的初始引导参数

	fdt_for_each_subnode(node, fdt, 0) {//遍历设备树的所有子节点
		const char *type = of_get_flat_dt_prop(node, "device_type", NULL);//获取节点的 "device_type" 属性
		const __be32 *reg, *endp;//reg 指向节点的 "reg" 属性，endp 用于指向 "reg" 属性的末尾
		int l;
		bool hotpluggable;//标记内存是否支持热插拔

		/* We are scanning "memory" nodes only */
		if (type == NULL || strcmp(type, "memory") != 0)//仅处理 "memory" 类型的节点
			continue;

		if (!of_fdt_device_is_available(fdt, node))//检查节点是否可用
			continue;//如果不可用，继续下一个节点

		reg = of_get_flat_dt_prop(node, "linux,usable-memory", &l);//尝试获取 "linux,usable-memory" 属性
		if (reg == NULL)
			reg = of_get_flat_dt_prop(node, "reg", &l);//如果 "linux,usable-memory" 不存在，获取 "reg" 属性
		if (reg == NULL)
			continue;//如果没有 "reg" 属性，跳过此节点

		endp = reg + (l / sizeof(__be32));//计算 "reg" 属性的结束位置
		hotpluggable = of_get_flat_dt_prop(node, "hotpluggable", NULL);//检查节点是否支持热插拔

		pr_debug("memory scan node %s, reg size %d,\n",
			 fdt_get_name(fdt, node, NULL), l);//打印节点信息和 "reg" 属性大小

		while ((endp - reg) >= (dt_root_addr_cells + dt_root_size_cells)) {//遍历 "reg" 属性中的地址和大小对
			u64 base, size;

			base = dt_mem_next_cell(dt_root_addr_cells, &reg);//获取内存块基地址
			size = dt_mem_next_cell(dt_root_size_cells, &reg);//获取内存大小

			if (size == 0)//如果内存大小为 0，跳过该内存块
				continue;
			pr_debug(" - %llx, %llx\n", base, size);//打印内存块的基地址和大小

			early_init_dt_add_memory_arch(base, size);//将内存块添加到系统的memblock中

			found_memory = 1;//标记为找到内存节点

			if (!hotpluggable)//如果不支持热插拔，跳过热插拔处理
				continue;

			if (memblock_mark_hotplug(base, size))//如果支持热插拔，标记内存块为热插拔内存
				pr_warn("failed to mark hotplug range 0x%llx - 0x%llx\n",
					base, base + size);//如果标记失败，打印警告信息
		}
	}
	return found_memory;//返回是否找到内存节点的标志
}
/*主要用于在系统启动时初始化设备树（Device Tree）中 "/chosen" 节点，提取与系统引导相关的命令行参数，并设置默认值。*/
int __init early_init_dt_scan_chosen(char *cmdline)
{
	int l, node;//定义整型变量 l 用于存储属性长度，node 用于存储节点偏移
	const char *p;
	const void *rng_seed;//定义常量无指针 rng_seed 用于存储随机数种子
	const void *fdt = initial_boot_params;//获取设备树的初始引导参数

	node = fdt_path_offset(fdt, "/chosen");//获取 "/chosen" 节点的偏移
	if (node < 0)
		node = fdt_path_offset(fdt, "/chosen@0");//如果无效，尝试获取 "/chosen@0" 节点的偏移
	if (node < 0)
		/* 处理命令行配置选项，即使没有 "/chosen" 节点 */
		goto handle_cmdline;// 跳转到处理命令行的部分

	chosen_node_offset = node;//保存找到的节点偏移

	early_init_dt_check_for_initrd(node);//检查设备树中是否有 initrd
	early_init_dt_check_for_elfcorehdr(node);//检查设备树中是否有 ELF 核心头

	rng_seed = of_get_flat_dt_prop(node, "rng-seed", &l);//获取 "rng-seed" 属性，返回长度
	if (rng_seed && l > 0) {//如果成功获取到随机数种子且长度有效
		add_bootloader_randomness(rng_seed, l);// 将随机数种子添加到引导加载器随机性中

		/* 尝试清除种子，以免被找到 */
		fdt_nop_property(initial_boot_params, node, "rng-seed");//从设备树中删除 "rng-seed" 属性

		/* 更新 CRC 校验值 */
		of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));//计算设备树的 CRC 校验，使用初始引导参数的大小
	}

	/* 获取命令行参数 */
	p = of_get_flat_dt_prop(node, "bootargs", &l);//从节点中获取 "bootargs" 属性
	if (p != NULL && l > 0)//如果命令行参数有效
		strscpy(cmdline, p, min(l, COMMAND_LINE_SIZE));//将命令行参数复制到 cmdline 中

handle_cmdline://处理命令行部分
	/*
	 * CONFIG_CMDLINE is meant to be a default in case nothing else
	 * managed to set the command line, unless CONFIG_CMDLINE_FORCE
	 * is set in which case we override whatever was found earlier.
	 * 如果没有其他地方设置命令行，使用 CONFIG_CMDLINE 作为默认值
	 * 除非设置了 CONFIG_CMDLINE_FORCE，否则覆盖之前找到的值
	 */
#ifdef CONFIG_CMDLINE//如果配置了命令行
#if defined(CONFIG_CMDLINE_EXTEND)//如果配置了命令行扩展
	strlcat(cmdline, " ", COMMAND_LINE_SIZE);//在命令行后添加空格
	strlcat(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);//将配置的命令行附加到 cmdline
#elif defined(CONFIG_CMDLINE_FORCE)//如果配置强制命令行
	strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);//直接复制配置的命令行到 cmdline
#else
	/* 如果没有来自引导加载程序的参数，则使用内核的命令行 */
	if (!((char *)cmdline)[0])//如果 cmdline 为空
		strscpy(cmdline, CONFIG_CMDLINE, COMMAND_LINE_SIZE);//复制配置的命令行
#endif
#endif /* CONFIG_CMDLINE */

	pr_debug("Command line is: %s\n", (char *)cmdline);//输出调试信息，显示最终的命令行
  1102

	return 0;//返回 0 表示成功
}

#ifndef MIN_MEMBLOCK_ADDR
#define MIN_MEMBLOCK_ADDR	__pa(PAGE_OFFSET)
#endif
#ifndef MAX_MEMBLOCK_ADDR
#define MAX_MEMBLOCK_ADDR	((phys_addr_t)~0)
#endif

void __init __weak early_init_dt_add_memory_arch(u64 base, u64 size)
{
	const u64 phys_offset = MIN_MEMBLOCK_ADDR;//系统支持的最小物理地址

	if (size < PAGE_SIZE - (base & ~PAGE_MASK)) {//如果内存块的大小不足以映射一页
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);//打印警告信息并忽略该内存块
		return;
	}

	if (!PAGE_ALIGNED(base)) {//如果内存块起始地址未对齐到页边界
		size -= PAGE_SIZE - (base & ~PAGE_MASK);//调整内存块大小以对齐到页边界
		base = PAGE_ALIGN(base);//调整内存块的起始地址到最近的页边界
	}
	size &= PAGE_MASK;//确保内存块大小也是页对齐的

	if (base > MAX_MEMBLOCK_ADDR) {// 如果内存块起始地址超出系统支持的最大物理地址
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);//打印警告信息并忽略该内存块
		return;
	}

	if (base + size - 1 > MAX_MEMBLOCK_ADDR) {//如果内存块的结束地址超出系统支持的最大物理地址
		pr_warn("Ignoring memory range 0x%llx - 0x%llx\n",
			((u64)MAX_MEMBLOCK_ADDR) + 1, base + size);//打印警告信息并调整内存块大小
		size = MAX_MEMBLOCK_ADDR - base + 1;//调整内存块的大小以适应系统的最大物理地址范围
	}

	if (base + size < phys_offset) {//如果内存块的结束地址小于系统支持的最小物理地址
		pr_warn("Ignoring memory block 0x%llx - 0x%llx\n",
			base, base + size);//打印警告信息并忽略该内存块
		return;
	}
	if (base < phys_offset) {//如果内存块的起始地址小于系统支持的最小物理地址
		pr_warn("Ignoring memory range 0x%llx - 0x%llx\n",
			base, phys_offset);//打印警告信息并调整内存块的起始地址
		size -= phys_offset - base;//调整内存块大小以适应系统的最小物理地址范围
		base = phys_offset;//将内存块的起始地址设置为系统支持的最小物理地址
	}
	memblock_add(base, size);//将调整后的内存块添加到内存块管理系统中
}

static void * __init early_init_dt_alloc_memory_arch(u64 size, u64 align)
{
	void *ptr = memblock_alloc(size, align);

	if (!ptr)
		panic("%s: Failed to allocate %llu bytes align=0x%llx\n",
		      __func__, size, align);

	return ptr;
}

bool __init early_init_dt_verify(void *params)//用于验证传入的设备树参数的有效性
{
	if (!params)//检查传入的参数是否为NULL
		return false;

	/* check device tree validity */
	if (fdt_check_header(params))//检查设备树头部的有效性
		return false;

	/* Setup flat device-tree pointer */
	initial_boot_params = params;// 保存设备树的指针
	of_fdt_crc32 = crc32_be(~0, initial_boot_params,
				fdt_totalsize(initial_boot_params));//计算设备树的CRC32校验值
	return true;//如果检查通过，返回true
}


void __init early_init_dt_scan_nodes(void)
{
	int rc;

	/* Initialize {size,address}-cells info */
	early_init_dt_scan_root();//扫描设备树根节点，初始化 size-cells 和 address-cells 信息

	/* Retrieve various information from the /chosen node */
	rc = early_init_dt_scan_chosen(boot_command_line);//从设备树的 /chosen 节点中获取启动相关信息，如命令行参数
	if (rc)//如果未能找到 /chosen 节点
		pr_warn("No chosen node found, continuing without\n");//打印警告信息，但继续初始化过程

	/* Setup memory, calling early_init_dt_add_memory_arch */
	early_init_dt_scan_memory();//扫描设备树中的内存节点，设置系统内存布局

	/* Handle linux,usable-memory-range property */
	early_init_dt_check_for_usable_mem_range();//检查和处理可用内存范围的属性
}

bool __init early_init_dt_scan(void *params)
{
	bool status;

	status = early_init_dt_verify(params);//调用验证函数验证传入的设备树
	if (!status)
		return false;//表示验证失败

	early_init_dt_scan_nodes();//验证成功后，扫描设备树中的各个节点
	return true;
}

static void *__init copy_device_tree(void *fdt)
{
	int size;
	void *dt;

	size = fdt_totalsize(fdt);
	dt = early_init_dt_alloc_memory_arch(size,
					     roundup_pow_of_two(FDT_V17_SIZE));

	if (dt)
		memcpy(dt, fdt, size);

	return dt;
}

/**
 * unflatten_device_tree - create tree of device_nodes from flat blob
 *
 * unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used.
 * 用于将设备树从扁平结构（FDT）展开为内核使用的层次化数据结构，以便内
 * 核可以访问和管理硬件资源。该过程通常在系统启动时完成。
 */
void __init unflatten_device_tree(void)
{
	void *fdt = initial_boot_params;//初始化 FDT（设备树）指针，指向 bootloader 提供的设备树

	/* Don't use the bootloader provided DTB if ACPI is enabled */
	if (!acpi_disabled)//如果 ACPI 没有禁用（即启用了 ACPI）
		fdt = NULL;//则不使用 bootloader 提供的设备树。

	/*
	 * Populate an empty root node when ACPI is enabled or bootloader
	 * doesn't provide one.
	 */
	if (!fdt) {//如果没有有效的 FDT（即使用 ACPI 或 bootloader 没有提供设备树）。
		fdt = (void *) __dtb_empty_root_begin;//使用内核提供的空根节点设备树
		/* fdt_totalsize() will be used for copy size */
		if (fdt_totalsize(fdt) >
		    __dtb_empty_root_end - __dtb_empty_root_begin) {//如果设备树的大小不合法，打印错误信息。
			pr_err("invalid size in dtb_empty_root\n");
			return;
		}
		of_fdt_crc32 = crc32_be(~0, fdt, fdt_totalsize(fdt));//计算设备树的校验和，用于后续验证
		fdt = copy_device_tree(fdt);//复制设备树数据以供使用
	}

	__unflatten_device_tree(fdt, NULL, &of_root,
				early_init_dt_alloc_memory_arch, false);//调用内部函数展开设备树，构建内核使用的设备树结构。

	/* Get pointer to "/chosen" and "/aliases" nodes for use everywhere */
	of_alias_scan(early_init_dt_alloc_memory_arch);//在设备树展开后扫描设备树，找到 "/chosen" 和 "/aliases" 节点。这些节点通常包含重要的启动参数和别名映射，用于设备的初始化和配置。

	unittest_unflatten_overlay_base();//执行设备树展开的单元测试，确保正确性
}

/**
 * unflatten_and_copy_device_tree - copy and create tree of device_nodes from flat blob
 *
 * Copies and unflattens the device-tree passed by the firmware, creating the
 * tree of struct device_node. It also fills the "name" and "type"
 * pointers of the nodes so the normal device-tree walking functions
 * can be used. This should only be used when the FDT memory has not been
 * reserved such is the case when the FDT is built-in to the kernel init
 * section. If the FDT memory is reserved already then unflatten_device_tree
 * should be used instead.
 */
void __init unflatten_and_copy_device_tree(void)
{
	if (initial_boot_params)
		initial_boot_params = copy_device_tree(initial_boot_params);

	unflatten_device_tree();
}

#ifdef CONFIG_SYSFS
static ssize_t of_fdt_raw_read(struct file *filp, struct kobject *kobj,
			       struct bin_attribute *bin_attr,
			       char *buf, loff_t off, size_t count)
{
	memcpy(buf, initial_boot_params + off, count);
	return count;
}

static int __init of_fdt_raw_init(void)
{
	static struct bin_attribute of_fdt_raw_attr =
		__BIN_ATTR(fdt, S_IRUSR, of_fdt_raw_read, NULL, 0);

	if (!initial_boot_params)
		return 0;

	if (of_fdt_crc32 != crc32_be(~0, initial_boot_params,
				     fdt_totalsize(initial_boot_params))) {
		pr_warn("not creating '/sys/firmware/fdt': CRC check failed\n");
		return 0;
	}
	of_fdt_raw_attr.size = fdt_totalsize(initial_boot_params);
	return sysfs_create_bin_file(firmware_kobj, &of_fdt_raw_attr);
}
late_initcall(of_fdt_raw_init);
#endif

#endif /* CONFIG_OF_EARLY_FLATTREE */
