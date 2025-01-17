// SPDX-License-Identifier: GPL-2.0-only
/*
 * mm_init.c - Memory initialisation verification and debugging
 *
 * Copyright 2008 IBM Corporation, 2008
 * Author Mel Gorman <mel@csn.ul.ie>
 *
 */
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/kobject.h>
#include <linux/export.h>
#include <linux/memory.h>
#include <linux/notifier.h>
#include <linux/sched.h>
#include <linux/mman.h>
#include <linux/memblock.h>
#include <linux/page-isolation.h>
#include <linux/padata.h>
#include <linux/nmi.h>
#include <linux/buffer_head.h>
#include <linux/kmemleak.h>
#include <linux/kfence.h>
#include <linux/page_ext.h>
#include <linux/pti.h>
#include <linux/pgtable.h>
#include <linux/stackdepot.h>
#include <linux/swap.h>
#include <linux/cma.h>
#include <linux/crash_dump.h>
#include <linux/execmem.h>
#include "internal.h"
#include "slab.h"
#include "shuffle.h"

#include <asm/setup.h>

#ifdef CONFIG_DEBUG_MEMORY_INIT
int __meminitdata mminit_loglevel;

/* The zonelists are simply reported, validation is manual. */
void __init mminit_verify_zonelist(void)
{
	int nid;

	if (mminit_loglevel < MMINIT_VERIFY)
		return;

	for_each_online_node(nid) {
		pg_data_t *pgdat = NODE_DATA(nid);
		struct zone *zone;
		struct zoneref *z;
		struct zonelist *zonelist;
		int i, listid, zoneid;

		BUILD_BUG_ON(MAX_ZONELISTS > 2);
		for (i = 0; i < MAX_ZONELISTS * MAX_NR_ZONES; i++) {

			/* Identify the zone and nodelist */
			zoneid = i % MAX_NR_ZONES;
			listid = i / MAX_NR_ZONES;
			zonelist = &pgdat->node_zonelists[listid];
			zone = &pgdat->node_zones[zoneid];
			if (!populated_zone(zone))
				continue;

			/* Print information about the zonelist */
			printk(KERN_DEBUG "mminit::zonelist %s %d:%s = ",
				listid > 0 ? "thisnode" : "general", nid,
				zone->name);

			/* Iterate the zonelist */
			for_each_zone_zonelist(zone, z, zonelist, zoneid)
				pr_cont("%d:%s ", zone_to_nid(zone), zone->name);
			pr_cont("\n");
		}
	}
}

void __init mminit_verify_pageflags_layout(void)
{
	int shift, width;
	unsigned long or_mask, add_mask;

	shift = BITS_PER_LONG;
	width = shift - SECTIONS_WIDTH - NODES_WIDTH - ZONES_WIDTH
		- LAST_CPUPID_SHIFT - KASAN_TAG_WIDTH - LRU_GEN_WIDTH - LRU_REFS_WIDTH;
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_widths",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d Gen %d Tier %d Flags %d\n",
		SECTIONS_WIDTH,
		NODES_WIDTH,
		ZONES_WIDTH,
		LAST_CPUPID_WIDTH,
		KASAN_TAG_WIDTH,
		LRU_GEN_WIDTH,
		LRU_REFS_WIDTH,
		NR_PAGEFLAGS);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_shifts",
		"Section %d Node %d Zone %d Lastcpupid %d Kasantag %d\n",
		SECTIONS_SHIFT,
		NODES_SHIFT,
		ZONES_SHIFT,
		LAST_CPUPID_SHIFT,
		KASAN_TAG_WIDTH);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_pgshifts",
		"Section %lu Node %lu Zone %lu Lastcpupid %lu Kasantag %lu\n",
		(unsigned long)SECTIONS_PGSHIFT,
		(unsigned long)NODES_PGSHIFT,
		(unsigned long)ZONES_PGSHIFT,
		(unsigned long)LAST_CPUPID_PGSHIFT,
		(unsigned long)KASAN_TAG_PGSHIFT);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodezoneid",
		"Node/Zone ID: %lu -> %lu\n",
		(unsigned long)(ZONEID_PGOFF + ZONEID_SHIFT),
		(unsigned long)ZONEID_PGOFF);
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_usage",
		"location: %d -> %d layout %d -> %d unused %d -> %d page-flags\n",
		shift, width, width, NR_PAGEFLAGS, NR_PAGEFLAGS, 0);
#ifdef NODE_NOT_IN_PAGE_FLAGS
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Node not in page flags");
#endif
#ifdef LAST_CPUPID_NOT_IN_PAGE_FLAGS
	mminit_dprintk(MMINIT_TRACE, "pageflags_layout_nodeflags",
		"Last cpupid not in page flags");
#endif

	if (SECTIONS_WIDTH) {
		shift -= SECTIONS_WIDTH;
		BUG_ON(shift != SECTIONS_PGSHIFT);
	}
	if (NODES_WIDTH) {
		shift -= NODES_WIDTH;
		BUG_ON(shift != NODES_PGSHIFT);
	}
	if (ZONES_WIDTH) {
		shift -= ZONES_WIDTH;
		BUG_ON(shift != ZONES_PGSHIFT);
	}

	/* Check for bitmask overlaps */
	or_mask = (ZONES_MASK << ZONES_PGSHIFT) |
			(NODES_MASK << NODES_PGSHIFT) |
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	add_mask = (ZONES_MASK << ZONES_PGSHIFT) +
			(NODES_MASK << NODES_PGSHIFT) +
			(SECTIONS_MASK << SECTIONS_PGSHIFT);
	BUG_ON(or_mask != add_mask);
}

static __init int set_mminit_loglevel(char *str)
{
	get_option(&str, &mminit_loglevel);
	return 0;
}
early_param("mminit_loglevel", set_mminit_loglevel);
#endif /* CONFIG_DEBUG_MEMORY_INIT */

struct kobject *mm_kobj;

#ifdef CONFIG_SMP
s32 vm_committed_as_batch = 32;

void mm_compute_batch(int overcommit_policy)
{
	u64 memsized_batch;
	s32 nr = num_present_cpus();
	s32 batch = max_t(s32, nr*2, 32);
	unsigned long ram_pages = totalram_pages();

	/*
	 * For policy OVERCOMMIT_NEVER, set batch size to 0.4% of
	 * (total memory/#cpus), and lift it to 25% for other policies
	 * to easy the possible lock contention for percpu_counter
	 * vm_committed_as, while the max limit is INT_MAX
	 */
	if (overcommit_policy == OVERCOMMIT_NEVER)
		memsized_batch = min_t(u64, ram_pages/nr/256, INT_MAX);
	else
		memsized_batch = min_t(u64, ram_pages/nr/4, INT_MAX);

	vm_committed_as_batch = max_t(s32, memsized_batch, batch);
}

static int __meminit mm_compute_batch_notifier(struct notifier_block *self,
					unsigned long action, void *arg)
{
	switch (action) {
	case MEM_ONLINE:
	case MEM_OFFLINE:
		mm_compute_batch(sysctl_overcommit_memory);
		break;
	default:
		break;
	}
	return NOTIFY_OK;
}

static int __init mm_compute_batch_init(void)
{
	mm_compute_batch(sysctl_overcommit_memory);
	hotplug_memory_notifier(mm_compute_batch_notifier, MM_COMPUTE_BATCH_PRI);
	return 0;
}

__initcall(mm_compute_batch_init);

#endif

static int __init mm_sysfs_init(void)
{
	mm_kobj = kobject_create_and_add("mm", kernel_kobj);
	if (!mm_kobj)
		return -ENOMEM;

	return 0;
}
postcore_initcall(mm_sysfs_init);

static unsigned long arch_zone_lowest_possible_pfn[MAX_NR_ZONES] __initdata;
static unsigned long arch_zone_highest_possible_pfn[MAX_NR_ZONES] __initdata;
static unsigned long zone_movable_pfn[MAX_NUMNODES] __initdata;

static unsigned long required_kernelcore __initdata;
static unsigned long required_kernelcore_percent __initdata;
static unsigned long required_movablecore __initdata;
static unsigned long required_movablecore_percent __initdata;

static unsigned long nr_kernel_pages __initdata;
static unsigned long nr_all_pages __initdata;

static bool deferred_struct_pages __meminitdata;

static DEFINE_PER_CPU(struct per_cpu_nodestat, boot_nodestats);

static int __init cmdline_parse_core(char *p, unsigned long *core,
				     unsigned long *percent)
{
	unsigned long long coremem;
	char *endptr;

	if (!p)
		return -EINVAL;

	/* Value may be a percentage of total memory, otherwise bytes */
	coremem = simple_strtoull(p, &endptr, 0);
	if (*endptr == '%') {
		/* Paranoid check for percent values greater than 100 */
		WARN_ON(coremem > 100);

		*percent = coremem;
	} else {
		coremem = memparse(p, &p);
		/* Paranoid check that UL is enough for the coremem value */
		WARN_ON((coremem >> PAGE_SHIFT) > ULONG_MAX);

		*core = coremem >> PAGE_SHIFT;
		*percent = 0UL;
	}
	return 0;
}

bool mirrored_kernelcore __initdata_memblock;

/*
 * kernelcore=size sets the amount of memory for use for allocations that
 * cannot be reclaimed or migrated.
 */
static int __init cmdline_parse_kernelcore(char *p)
{
	/* parse kernelcore=mirror */
	if (parse_option_str(p, "mirror")) {
		mirrored_kernelcore = true;
		return 0;
	}

	return cmdline_parse_core(p, &required_kernelcore,
				  &required_kernelcore_percent);
}
early_param("kernelcore", cmdline_parse_kernelcore);

/*
 * movablecore=size sets the amount of memory for use for allocations that
 * can be reclaimed or migrated.
 */
static int __init cmdline_parse_movablecore(char *p)
{
	return cmdline_parse_core(p, &required_movablecore,
				  &required_movablecore_percent);
}
early_param("movablecore", cmdline_parse_movablecore);

/*
 * early_calculate_totalpages()
 * Sum pages in active regions for movable zone.
 * Populate N_MEMORY for calculating usable_nodes.
 */
static unsigned long __init early_calculate_totalpages(void)
{
	unsigned long totalpages = 0;
	unsigned long start_pfn, end_pfn;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {
		unsigned long pages = end_pfn - start_pfn;

		totalpages += pages;
		if (pages)
			node_set_state(nid, N_MEMORY);
	}
	return totalpages;
}

/*
 * This finds a zone that can be used for ZONE_MOVABLE pages. The
 * assumption is made that zones within a node are ordered in monotonic
 * increasing memory addresses so that the "highest" populated zone is used
 */
static void __init find_usable_zone_for_movable(void)
{
	int zone_index;
	for (zone_index = MAX_NR_ZONES - 1; zone_index >= 0; zone_index--) {
		if (zone_index == ZONE_MOVABLE)
			continue;

		if (arch_zone_highest_possible_pfn[zone_index] >
				arch_zone_lowest_possible_pfn[zone_index])
			break;
	}

	VM_BUG_ON(zone_index == -1);
	movable_zone = zone_index;
}

/*
 * Find the PFN the Movable zone begins in each node. Kernel memory
 * is spread evenly between nodes as long as the nodes have enough
 * memory. When they don't, some nodes will have more kernelcore than
 * others
 * 用于计算和分配系统中各节点的可移动内存区域
 */
static void __init find_zone_movable_pfns_for_nodes(void)
{
	int i, nid;//定义用于循环的变量 i 和内存节点标识 nid
	unsigned long usable_startpfn;//定义变量用于存储可用的起始页帧号
	unsigned long kernelcore_node, kernelcore_remaining;//每个节点用于内核的核心内存和剩余内核核心内存
	/* save the state before borrow the nodemask */
	nodemask_t saved_node_state = node_states[N_MEMORY];// 保存当前节点的状态，便于后续恢复
	unsigned long totalpages = early_calculate_totalpages();//计算系统中所有内存页的总数
	int usable_nodes = nodes_weight(node_states[N_MEMORY]);//获取当前系统中有内存的节点数量
	struct memblock_region *r;//定义内存块区域的指针

	/* Need to find movable_zone earlier when movable_node is specified. */
	find_usable_zone_for_movable();//需要找到可用的 ZONE_MOVABLE 区域（主要用于热插拔的内存）

	/*
	 * If movable_node is specified, ignore kernelcore and movablecore
	 * options.
	 * 如果启用了 movable_node，则忽略 kernelcore 和 movablecore 选项
	 */
	if (movable_node_is_enabled()) {
		for_each_mem_region(r) {//遍历所有内存块区域
			if (!memblock_is_hotpluggable(r))//如果该内存块不可热插拔，则跳过
				continue;

			nid = memblock_get_region_node(r);//获取当前内存块所在的节点ID

			usable_startpfn = PFN_DOWN(r->base);//获取内存块的起始页帧号（向下取整）
			zone_movable_pfn[nid] = zone_movable_pfn[nid] ?
				min(usable_startpfn, zone_movable_pfn[nid]) :
				usable_startpfn;//更新节点的 ZONE_MOVABLE 起始页帧号，取较小的那个，或初始化为起始页帧号
		}

		goto out2;//跳到 out2 标签，进行后续处理
	}

	/*
	 * If kernelcore=mirror is specified, ignore movablecore option
	 * 处理 kernelcore=mirror 的情况，忽略 movablecore 选项
	 */
	if (mirrored_kernelcore) {
		bool mem_below_4gb_not_mirrored = false;//标记是否存在未镜像的 4GB 以下内存

		if (!memblock_has_mirror()) {//检查系统是否存在镜像内存
			pr_warn("The system has no mirror memory, ignore kernelcore=mirror.\n");
			goto out;//如果没有镜像内存，跳到 out 标签
		}

		if (is_kdump_kernel()) {//检查系统是否处于 kdump 模式
			pr_warn("The system is under kdump, ignore kernelcore=mirror.\n");
			goto out;//如果是 kdump 内核，则忽略并跳到 out 标签
		}

		for_each_mem_region(r) {//遍历所有内存块区域
			if (memblock_is_mirror(r))//如果该内存块是镜像内存，则跳过
				continue;

			nid = memblock_get_region_node(r);//获取当前内存块所在的节点 ID

			usable_startpfn = memblock_region_memory_base_pfn(r);// 获取内存块的起始页帧号

			if (usable_startpfn < PHYS_PFN(SZ_4G)) {//检查内存块是否位于 4GB 以下
				mem_below_4gb_not_mirrored = true;//设置未镜像标记
				continue;//跳过此内存块
			}

			zone_movable_pfn[nid] = zone_movable_pfn[nid] ?
				min(usable_startpfn, zone_movable_pfn[nid]) :
				usable_startpfn;//更新节点的 ZONE_MOVABLE 起始页帧号，取较小的那个，或初始化为起始页帧号
		}

		if (mem_below_4gb_not_mirrored)//如果有未镜像的 4GB 以下内存块，打印警告
			pr_warn("This configuration results in unmirrored kernel memory.\n");

		goto out2;
	}

	/*
	 * If kernelcore=nn% or movablecore=nn% was specified, calculate the
	 * amount of necessary memory.
	 * 如果指定了 kernelcore 或 movablecore 的百分比，则计算相应的内存量
	 */
	if (required_kernelcore_percent)
		required_kernelcore = (totalpages * 100 * required_kernelcore_percent) /
				       10000UL;
	if (required_movablecore_percent)
		required_movablecore = (totalpages * 100 * required_movablecore_percent) /
					10000UL;

	/*
	 * If movablecore= was specified, calculate what size of
	 * kernelcore that corresponds so that memory usable for
	 * any allocation type is evenly spread. If both kernelcore
	 * and movablecore are specified, then the value of kernelcore
	 * will be used for required_kernelcore if it's greater than
	 * what movablecore would have allowed.
	 */
	if (required_movablecore) {//如果指定了 movablecore，则计算相应的 kernelcore 大小
		unsigned long corepages;//核心内存的页面数

		/*
		 * Round-up so that ZONE_MOVABLE is at least as large as what
		 * was requested by the user
		 */
		required_movablecore =
			roundup(required_movablecore, MAX_ORDER_NR_PAGES);//对齐页数
		required_movablecore = min(totalpages, required_movablecore);//不能超过总页数
		corepages = totalpages - required_movablecore;//计算内核核心页面数

		required_kernelcore = max(required_kernelcore, corepages);//确保内核核心页数满足要求
	}

	/*
	 * If kernelcore was not specified or kernelcore size is larger
	 * than totalpages, there is no ZONE_MOVABLE.
	 * 如果未指定 kernelcore 或者 kernelcore 大于等于总页数，则没有 ZONE_MOVABLE
	 */
	if (!required_kernelcore || required_kernelcore >= totalpages)
		goto out;

	/* usable_startpfn is the lowest possible pfn ZONE_MOVABLE can be at */
	usable_startpfn = arch_zone_lowest_possible_pfn[movable_zone];// 获取 ZONE_MOVABLE 的最低可能起始页帧号

restart:
	/* Spread kernelcore memory as evenly as possible throughout nodes */
	kernelcore_node = required_kernelcore / usable_nodes;//将内核核心页数均匀分配到每个节点
	for_each_node_state(nid, N_MEMORY) { //遍历所有有内存的节点
		unsigned long start_pfn, end_pfn;//定义变量用于存储节点内的起始和结束页帧号

		/*
		 * Recalculate kernelcore_node if the division per node
		 * now exceeds what is necessary to satisfy the requested
		 * amount of memory for the kernel
		 */
		if (required_kernelcore < kernelcore_node)//如果每个节点的 kernelcore 分配超过需要的内存，重新计算
			kernelcore_node = required_kernelcore / usable_nodes;

		/*
		 * As the map is walked, we track how much memory is usable
		 * by the kernel using kernelcore_remaining. When it is
		 * 0, the rest of the node is usable by ZONE_MOVABLE
		 */
		kernelcore_remaining = kernelcore_node;//设置当前节点剩余的内核核心内存

		/* Go through each range of PFNs within this node */
		for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {//遍历节点中的每个内存页帧范围
			unsigned long size_pages;//定义变量存储内核核心页数

			start_pfn = max(start_pfn, zone_movable_pfn[nid]);//获取当前可用的起始页帧号
			if (start_pfn >= end_pfn)//如果起始帧超过结束帧，跳过
				continue;

			/* Account for what is only usable for kernelcore */
			if (start_pfn < usable_startpfn) {//如果内核可用区域位于可用起始页帧之前，计算内核可用页数
				unsigned long kernel_pages;
				kernel_pages = min(end_pfn, usable_startpfn)
								- start_pfn;//计算在当前范围内的可用内核页数

				kernelcore_remaining -= min(kernel_pages,
							kernelcore_remaining);//从剩余的内核核心页数中减去当前计算的可用页数，确保不会超过实际需求
				required_kernelcore -= min(kernel_pages,
							required_kernelcore);//从所需内核核心页数中减去当前计算的可用页数，防止超额计算

				/* Continue if range is now fully accounted */
				if (end_pfn <= usable_startpfn) {// 如果内核页面数已经满足要求，更新 ZONE_MOVABLE 的起始页帧号

					/*
					 * Push zone_movable_pfn to the end so
					 * that if we have to rebalance
					 * kernelcore across nodes, we will
					 * not double account here
					 */
					zone_movable_pfn[nid] = end_pfn;//更新节点的 ZONE_MOVABLE 起始页帧号
					continue;//跳过剩余部分
				}
				start_pfn = usable_startpfn;// 更新起始页帧号为可用的最小页帧号
			}

			/*
			 * The usable PFN range for ZONE_MOVABLE is from
			 * start_pfn->end_pfn. Calculate size_pages as the
			 * number of pages used as kernelcore
			 */
			size_pages = end_pfn - start_pfn;//计算 ZONE_MOVABLE 的页面数
			if (size_pages > kernelcore_remaining)// 如果页面数超过剩余内核核心页面，调整页面数
				size_pages = kernelcore_remaining;
			zone_movable_pfn[nid] = start_pfn + size_pages;// 更新 ZONE_MOVABLE 的起始页帧号

			/*
			 * Some kernelcore has been met, update counts and
			 * break if the kernelcore for this node has been
			 * satisfied
			 */
			required_kernelcore -= min(required_kernelcore,
								size_pages);//减少所需内核核心页数
			kernelcore_remaining -= size_pages;//减少剩余内核核心页数
			if (!kernelcore_remaining)//如果剩余内核核心页数为0，退出循环
				break;
		}
	}

	/*
	 * If there is still required_kernelcore, we do another pass with one
	 * less node in the count. This will push zone_movable_pfn[nid] further
	 * along on the nodes that still have memory until kernelcore is
	 * satisfied
	 */
	usable_nodes--;//如果剩余的 kernelcore 数量还没有分配完，减少可用节点数量并重新开始
	if (usable_nodes && required_kernelcore > usable_nodes)
		goto restart;

out2:
	/* Align start of ZONE_MOVABLE on all nids to MAX_ORDER_NR_PAGES */
	for (nid = 0; nid < MAX_NUMNODES; nid++) {//将所有节点的 ZONE_MOVABLE 起始位置对齐到 MAX_ORDER_NR_PAGES
		unsigned long start_pfn, end_pfn;

		zone_movable_pfn[nid] =
			roundup(zone_movable_pfn[nid], MAX_ORDER_NR_PAGES);//对齐起始页帧号

		get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);//获取节点的起始和结束页帧号
		if (zone_movable_pfn[nid] >= end_pfn)//如果起始页帧超过节点的结束页帧，置为 0
			zone_movable_pfn[nid] = 0;
	}

out:
	/* restore the node_state */
	node_states[N_MEMORY] = saved_node_state;//恢复节点状态
}
/*用于初始化单个内存页面的结构体（struct page）*/
void __meminit __init_single_page(struct page *page, unsigned long pfn,
				unsigned long zone, int nid)
{
	mm_zero_struct_page(page);//将页面结构体初始化为零
	set_page_links(page, zone, nid, pfn);//设置页面的链接，包括区域、节点和页帧号
	init_page_count(page);//初始化页面引用计数
	page_mapcount_reset(page);//重置页面映射计数
	page_cpupid_reset_last(page);//重置页面的最后 CPUID（用于进程跟踪）
	page_kasan_tag_reset(page);//重置 KASAN（Kernel Address Sanitizer）标记

	INIT_LIST_HEAD(&page->lru);//初始化页面的 LRU 链表头
#ifdef WANT_PAGE_VIRTUAL
	/* The shift won't overflow because ZONE_NORMAL is below 4G. */
	if (!is_highmem_idx(zone))//如果该页面不在高端内存区域，将其虚拟地址设置为对应的物理地址
		set_page_address(page, __va(pfn << PAGE_SHIFT));
#endif
}

#ifdef CONFIG_NUMA
/*
 * During memory init memblocks map pfns to nids. The search is expensive and
 * this caches recent lookups. The implementation of __early_pfn_to_nid
 * treats start/end as pfns.
 */
struct mminit_pfnnid_cache {
	unsigned long last_start;
	unsigned long last_end;
	int last_nid;
};

static struct mminit_pfnnid_cache early_pfnnid_cache __meminitdata;

/*
 * Required by SPARSEMEM. Given a PFN, return what node the PFN is on.
 */
static int __meminit __early_pfn_to_nid(unsigned long pfn,
					struct mminit_pfnnid_cache *state)
{
	unsigned long start_pfn, end_pfn;
	int nid;

	if (state->last_start <= pfn && pfn < state->last_end)
		return state->last_nid;

	nid = memblock_search_pfn_nid(pfn, &start_pfn, &end_pfn);
	if (nid != NUMA_NO_NODE) {
		state->last_start = start_pfn;
		state->last_end = end_pfn;
		state->last_nid = nid;
	}

	return nid;
}

int __meminit early_pfn_to_nid(unsigned long pfn)
{
	static DEFINE_SPINLOCK(early_pfn_lock);
	int nid;

	spin_lock(&early_pfn_lock);
	nid = __early_pfn_to_nid(pfn, &early_pfnnid_cache);
	if (nid < 0)
		nid = first_online_node;
	spin_unlock(&early_pfn_lock);

	return nid;
}

int hashdist = HASHDIST_DEFAULT;

static int __init set_hashdist(char *str)
{
	if (!str)
		return 0;
	hashdist = simple_strtoul(str, &str, 0);
	return 1;
}
__setup("hashdist=", set_hashdist);

static inline void fixup_hashdist(void)
{
	if (num_node_state(N_MEMORY) == 1)
		hashdist = 0;
}
#else
static inline void fixup_hashdist(void) {}
#endif /* CONFIG_NUMA */

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
static inline void pgdat_set_deferred_range(pg_data_t *pgdat)
{
	pgdat->first_deferred_pfn = ULONG_MAX;
}

/* Returns true if the struct page for the pfn is initialised */
static inline bool __meminit early_page_initialised(unsigned long pfn, int nid)
{
	if (node_online(nid) && pfn >= NODE_DATA(nid)->first_deferred_pfn)
		return false;

	return true;
}

/*
 * Returns true when the remaining initialisation should be deferred until
 * later in the boot cycle when it can be parallelised.
 */
static bool __meminit
defer_init(int nid, unsigned long pfn, unsigned long end_pfn)
{
	static unsigned long prev_end_pfn, nr_initialised;

	if (early_page_ext_enabled())
		return false;
	/*
	 * prev_end_pfn static that contains the end of previous zone
	 * No need to protect because called very early in boot before smp_init.
	 */
	if (prev_end_pfn != end_pfn) {
		prev_end_pfn = end_pfn;
		nr_initialised = 0;
	}

	/* Always populate low zones for address-constrained allocations */
	if (end_pfn < pgdat_end_pfn(NODE_DATA(nid)))
		return false;

	if (NODE_DATA(nid)->first_deferred_pfn != ULONG_MAX)
		return true;
	/*
	 * We start only with one section of pages, more pages are added as
	 * needed until the rest of deferred pages are initialized.
	 */
	nr_initialised++;
	if ((nr_initialised > PAGES_PER_SECTION) &&
	    (pfn & (PAGES_PER_SECTION - 1)) == 0) {
		NODE_DATA(nid)->first_deferred_pfn = pfn;
		return true;
	}
	return false;
}

static void __meminit init_reserved_page(unsigned long pfn, int nid)
{
	pg_data_t *pgdat;
	int zid;

	if (early_page_initialised(pfn, nid))
		return;

	pgdat = NODE_DATA(nid);

	for (zid = 0; zid < MAX_NR_ZONES; zid++) {
		struct zone *zone = &pgdat->node_zones[zid];

		if (zone_spans_pfn(zone, pfn))
			break;
	}
	__init_single_page(pfn_to_page(pfn), pfn, zid, nid);
}
#else
static inline void pgdat_set_deferred_range(pg_data_t *pgdat) {}

static inline bool early_page_initialised(unsigned long pfn, int nid)
{
	return true;
}

static inline bool defer_init(int nid, unsigned long pfn, unsigned long end_pfn)
{
	return false;
}

static inline void init_reserved_page(unsigned long pfn, int nid)
{
}
#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

/*
 * Initialised pages do not have PageReserved set. This function is
 * called for each range allocated by the bootmem allocator and
 * marks the pages PageReserved. The remaining valid pages are later
 * sent to the buddy page allocator.
 */
void __meminit reserve_bootmem_region(phys_addr_t start,
				      phys_addr_t end, int nid)
{
	unsigned long start_pfn = PFN_DOWN(start);
	unsigned long end_pfn = PFN_UP(end);

	for (; start_pfn < end_pfn; start_pfn++) {
		if (pfn_valid(start_pfn)) {
			struct page *page = pfn_to_page(start_pfn);

			init_reserved_page(start_pfn, nid);

			/* Avoid false-positive PageTail() */
			INIT_LIST_HEAD(&page->lru);

			/*
			 * no need for atomic set_bit because the struct
			 * page is not visible yet so nobody should
			 * access it yet.
			 */
			__SetPageReserved(page);
		}
	}
}

/* If zone is ZONE_MOVABLE but memory is mirrored, it is an overlapped init */
static bool __meminit
overlap_memmap_init(unsigned long zone, unsigned long *pfn)
{
	static struct memblock_region *r;

	if (mirrored_kernelcore && zone == ZONE_MOVABLE) {
		if (!r || *pfn >= memblock_region_memory_end_pfn(r)) {
			for_each_mem_region(r) {
				if (*pfn < memblock_region_memory_end_pfn(r))
					break;
			}
		}
		if (*pfn >= memblock_region_memory_base_pfn(r) &&
		    memblock_is_mirror(r)) {
			*pfn = memblock_region_memory_end_pfn(r);
			return true;
		}
	}
	return false;
}

/*
 * Only struct pages that correspond to ranges defined by memblock.memory
 * are zeroed and initialized by going through __init_single_page() during
 * memmap_init_zone_range().
 *
 * But, there could be struct pages that correspond to holes in
 * memblock.memory. This can happen because of the following reasons:
 * - physical memory bank size is not necessarily the exact multiple of the
 *   arbitrary section size
 * - early reserved memory may not be listed in memblock.memory
 * - non-memory regions covered by the contigious flatmem mapping
 * - memory layouts defined with memmap= kernel parameter may not align
 *   nicely with memmap sections
 *
 * Explicitly initialize those struct pages so that:
 * - PG_Reserved is set
 * - zone and node links point to zone and node that span the page if the
 *   hole is in the middle of a zone
 * - zone and node links point to adjacent zone/node if the hole falls on
 *   the zone boundary; the pages in such holes will be prepended to the
 *   zone/node above the hole except for the trailing pages in the last
 *   section that will be appended to the zone/node below.
 */
static void __init init_unavailable_range(unsigned long spfn,
					  unsigned long epfn,
					  int zone, int node)
{
	unsigned long pfn;
	u64 pgcnt = 0;

	for (pfn = spfn; pfn < epfn; pfn++) {
		if (!pfn_valid(pageblock_start_pfn(pfn))) {
			pfn = pageblock_end_pfn(pfn) - 1;
			continue;
		}
		__init_single_page(pfn_to_page(pfn), pfn, zone, node);
		__SetPageReserved(pfn_to_page(pfn));
		pgcnt++;
	}

	if (pgcnt)
		pr_info("On node %d, zone %s: %lld pages in unavailable ranges\n",
			node, zone_names[zone], pgcnt);
}

/*
 * Initially all pages are reserved - free ones are freed
 * up by memblock_free_all() once the early boot process is
 * done. Non-atomic initialization, single-pass.
 *
 * All aligned pageblocks are initialized to the specified migratetype
 * (usually MIGRATE_MOVABLE). Besides setting the migratetype, no related
 * zone stats (e.g., nr_isolate_pageblock) are touched.
 */
void __meminit memmap_init_range(unsigned long size, int nid, unsigned long zone,
		unsigned long start_pfn, unsigned long zone_end_pfn,
		enum meminit_context context,
		struct vmem_altmap *altmap, int migratetype)
{
	unsigned long pfn, end_pfn = start_pfn + size;
	struct page *page;

	if (highest_memmap_pfn < end_pfn - 1)
		highest_memmap_pfn = end_pfn - 1;

#ifdef CONFIG_ZONE_DEVICE
	/*
	 * Honor reservation requested by the driver for this ZONE_DEVICE
	 * memory. We limit the total number of pages to initialize to just
	 * those that might contain the memory mapping. We will defer the
	 * ZONE_DEVICE page initialization until after we have released
	 * the hotplug lock.
	 */
	if (zone == ZONE_DEVICE) {
		if (!altmap)
			return;

		if (start_pfn == altmap->base_pfn)
			start_pfn += altmap->reserve;
		end_pfn = altmap->base_pfn + vmem_altmap_offset(altmap);
	}
#endif

	for (pfn = start_pfn; pfn < end_pfn; ) {
		/*
		 * There can be holes in boot-time mem_map[]s handed to this
		 * function.  They do not exist on hotplugged memory.
		 */
		if (context == MEMINIT_EARLY) {
			if (overlap_memmap_init(zone, &pfn))
				continue;
			if (defer_init(nid, pfn, zone_end_pfn)) {
				deferred_struct_pages = true;
				break;
			}
		}

		page = pfn_to_page(pfn);
		__init_single_page(page, pfn, zone, nid);
		if (context == MEMINIT_HOTPLUG)
			__SetPageReserved(page);

		/*
		 * Usually, we want to mark the pageblock MIGRATE_MOVABLE,
		 * such that unmovable allocations won't be scattered all
		 * over the place during system boot.
		 */
		if (pageblock_aligned(pfn)) {
			set_pageblock_migratetype(page, migratetype);
			cond_resched();
		}
		pfn++;
	}
}
/*用于初始化指定内存区域的页表结构，并处理内存空洞。*/
static void __init memmap_init_zone_range(struct zone *zone,
					  unsigned long start_pfn,
					  unsigned long end_pfn,
					  unsigned long *hole_pfn)
{
	unsigned long zone_start_pfn = zone->zone_start_pfn;//获取该内存区域的起始页帧号
	unsigned long zone_end_pfn = zone_start_pfn + zone->spanned_pages;// 计算该内存区域的结束页帧号
	int nid = zone_to_nid(zone), zone_id = zone_idx(zone);// 获取该内存区域所属的节点 ID 和区域 ID

	start_pfn = clamp(start_pfn, zone_start_pfn, zone_end_pfn);//将传入的起始和结束页帧号限制在该区域的页帧号范围内
	end_pfn = clamp(end_pfn, zone_start_pfn, zone_end_pfn);

	if (start_pfn >= end_pfn)//如果起始页帧号大于等于结束页帧号，说明没有有效内存块需要初始化，直接返回
		return;

	memmap_init_range(end_pfn - start_pfn, nid, zone_id, start_pfn,
			  zone_end_pfn, MEMINIT_EARLY, NULL, MIGRATE_MOVABLE);//初始化该内存区域的page结构，处理从 start_pfn 到 end_pfn 的页帧

	if (*hole_pfn < start_pfn)
		init_unavailable_range(*hole_pfn, start_pfn, zone_id, nid);// 如果当前 hole_pfn 小于 start_pfn，说明存在内存空洞，需初始化不可用的内存范围

	*hole_pfn = end_pfn;//更新 hole_pfn 为该区域的结束页帧号，表示处理到的最远位置
}
/*用于初始化系统中的内存映射表，即为每个内存页分配和设置对应的页结构（struct page）。*/
static void __init memmap_init(void)
{
	unsigned long start_pfn, end_pfn;// 定义起始和结束页帧号
	unsigned long hole_pfn = 0;//用于记录内存空洞的起始页帧号，初始化为 0
	int i, j, zone_id = 0, nid;//定义循环变量 i 和 j，zone_id 用于记录区域 ID，nid 记录节点 ID

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {//遍历所有内存区域，获取每个区域的起始和结束页帧号及对应的节点 ID
		struct pglist_data *node = NODE_DATA(nid);//获取节点的数据结构 pglist_data

		for (j = 0; j < MAX_NR_ZONES; j++) {//遍历当前节点中的所有内存区域（zone）
			struct zone *zone = node->node_zones + j;//获取当前节点的第 j 个内存区域

			if (!populated_zone(zone))//如果该区域没有分配人口（即没有有效页面），跳过该区域
				continue;

			memmap_init_zone_range(zone, start_pfn, end_pfn,
					       &hole_pfn);//初始化该内存区域内存的page结构，处理从 start_pfn 到 end_pfn 之间的页帧
			zone_id = j;//记录当前区域 ID
		}
	}

#ifdef CONFIG_SPARSEMEM
	/*
	 * Initialize the memory map for hole in the range [memory_end,
	 * section_end].
	 * Append the pages in this hole to the highest zone in the last
	 * node.
	 * The call to init_unavailable_range() is outside the ifdef to
	 * silence the compiler warining about zone_id set but not used;
	 * for FLATMEM it is a nop anyway
	 */
	end_pfn = round_up(end_pfn, PAGES_PER_SECTION);// 将结束页帧号向上对齐到段的边界（PAGES_PER_SECTION 的倍数）
	if (hole_pfn < end_pfn)//如果 hole_pfn 小于对齐后的 end_pfn，则说明还有内存空洞需要处理
#endif
		init_unavailable_range(hole_pfn, end_pfn, zone_id, nid);// 初始化不可用内存范围，处理从 hole_pfn 到 end_pfn 的内存空洞
}

#ifdef CONFIG_ZONE_DEVICE
static void __ref __init_zone_device_page(struct page *page, unsigned long pfn,
					  unsigned long zone_idx, int nid,
					  struct dev_pagemap *pgmap)
{

	__init_single_page(page, pfn, zone_idx, nid);

	/*
	 * Mark page reserved as it will need to wait for onlining
	 * phase for it to be fully associated with a zone.
	 *
	 * We can use the non-atomic __set_bit operation for setting
	 * the flag as we are still initializing the pages.
	 */
	__SetPageReserved(page);

	/*
	 * ZONE_DEVICE pages union ->lru with a ->pgmap back pointer
	 * and zone_device_data.  It is a bug if a ZONE_DEVICE page is
	 * ever freed or placed on a driver-private list.
	 */
	page->pgmap = pgmap;
	page->zone_device_data = NULL;

	/*
	 * Mark the block movable so that blocks are reserved for
	 * movable at startup. This will force kernel allocations
	 * to reserve their blocks rather than leaking throughout
	 * the address space during boot when many long-lived
	 * kernel allocations are made.
	 *
	 * Please note that MEMINIT_HOTPLUG path doesn't clear memmap
	 * because this is done early in section_activate()
	 */
	if (pageblock_aligned(pfn)) {
		set_pageblock_migratetype(page, MIGRATE_MOVABLE);
		cond_resched();
	}

	/*
	 * ZONE_DEVICE pages are released directly to the driver page allocator
	 * which will set the page count to 1 when allocating the page.
	 */
	if (pgmap->type == MEMORY_DEVICE_PRIVATE ||
	    pgmap->type == MEMORY_DEVICE_COHERENT)
		set_page_count(page, 0);
}

/*
 * With compound page geometry and when struct pages are stored in ram most
 * tail pages are reused. Consequently, the amount of unique struct pages to
 * initialize is a lot smaller that the total amount of struct pages being
 * mapped. This is a paired / mild layering violation with explicit knowledge
 * of how the sparse_vmemmap internals handle compound pages in the lack
 * of an altmap. See vmemmap_populate_compound_pages().
 */
static inline unsigned long compound_nr_pages(struct vmem_altmap *altmap,
					      struct dev_pagemap *pgmap)
{
	if (!vmemmap_can_optimize(altmap, pgmap))
		return pgmap_vmemmap_nr(pgmap);

	return VMEMMAP_RESERVE_NR * (PAGE_SIZE / sizeof(struct page));
}

static void __ref memmap_init_compound(struct page *head,
				       unsigned long head_pfn,
				       unsigned long zone_idx, int nid,
				       struct dev_pagemap *pgmap,
				       unsigned long nr_pages)
{
	unsigned long pfn, end_pfn = head_pfn + nr_pages;
	unsigned int order = pgmap->vmemmap_shift;

	__SetPageHead(head);
	for (pfn = head_pfn + 1; pfn < end_pfn; pfn++) {
		struct page *page = pfn_to_page(pfn);

		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);
		prep_compound_tail(head, pfn - head_pfn);
		set_page_count(page, 0);

		/*
		 * The first tail page stores important compound page info.
		 * Call prep_compound_head() after the first tail page has
		 * been initialized, to not have the data overwritten.
		 */
		if (pfn == head_pfn + 1)
			prep_compound_head(head, order);
	}
}

void __ref memmap_init_zone_device(struct zone *zone,
				   unsigned long start_pfn,
				   unsigned long nr_pages,
				   struct dev_pagemap *pgmap)
{
	unsigned long pfn, end_pfn = start_pfn + nr_pages;
	struct pglist_data *pgdat = zone->zone_pgdat;
	struct vmem_altmap *altmap = pgmap_altmap(pgmap);
	unsigned int pfns_per_compound = pgmap_vmemmap_nr(pgmap);
	unsigned long zone_idx = zone_idx(zone);
	unsigned long start = jiffies;
	int nid = pgdat->node_id;

	if (WARN_ON_ONCE(!pgmap || zone_idx != ZONE_DEVICE))
		return;

	/*
	 * The call to memmap_init should have already taken care
	 * of the pages reserved for the memmap, so we can just jump to
	 * the end of that region and start processing the device pages.
	 */
	if (altmap) {
		start_pfn = altmap->base_pfn + vmem_altmap_offset(altmap);
		nr_pages = end_pfn - start_pfn;
	}

	for (pfn = start_pfn; pfn < end_pfn; pfn += pfns_per_compound) {
		struct page *page = pfn_to_page(pfn);

		__init_zone_device_page(page, pfn, zone_idx, nid, pgmap);

		if (pfns_per_compound == 1)
			continue;

		memmap_init_compound(page, pfn, zone_idx, nid, pgmap,
				     compound_nr_pages(altmap, pgmap));
	}

	pr_debug("%s initialised %lu pages in %ums\n", __func__,
		nr_pages, jiffies_to_msecs(jiffies - start));
}
#endif

/*
 * The zone ranges provided by the architecture do not include ZONE_MOVABLE
 * because it is sized independent of architecture. Unlike the other zones,
 * the starting point for ZONE_MOVABLE is not fixed. It may be different
 * in each node depending on the size of each node and how evenly kernelcore
 * is distributed. This helper function adjusts the zone ranges
 * provided by the architecture for a given node by using the end of the
 * highest usable zone for ZONE_MOVABLE. This preserves the assumption that
 * zones within a node are in order of monotonic increases memory addresses
 */
static void __init adjust_zone_range_for_zone_movable(int nid,
					unsigned long zone_type,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	/* Only adjust if ZONE_MOVABLE is on this node */
	if (zone_movable_pfn[nid]) {
		/* Size ZONE_MOVABLE */
		if (zone_type == ZONE_MOVABLE) {
			*zone_start_pfn = zone_movable_pfn[nid];
			*zone_end_pfn = min(node_end_pfn,
				arch_zone_highest_possible_pfn[movable_zone]);

		/* Adjust for ZONE_MOVABLE starting within this range */
		} else if (!mirrored_kernelcore &&
			*zone_start_pfn < zone_movable_pfn[nid] &&
			*zone_end_pfn > zone_movable_pfn[nid]) {
			*zone_end_pfn = zone_movable_pfn[nid];

		/* Check if this whole range is within ZONE_MOVABLE */
		} else if (*zone_start_pfn >= zone_movable_pfn[nid])
			*zone_start_pfn = *zone_end_pfn;
	}
}

/*
 * Return the number of holes in a range on a node. If nid is MAX_NUMNODES,
 * then all holes in the requested range will be accounted for.
 */
static unsigned long __init __absent_pages_in_range(int nid,
				unsigned long range_start_pfn,
				unsigned long range_end_pfn)
{
	unsigned long nr_absent = range_end_pfn - range_start_pfn;
	unsigned long start_pfn, end_pfn;
	int i;

	for_each_mem_pfn_range(i, nid, &start_pfn, &end_pfn, NULL) {
		start_pfn = clamp(start_pfn, range_start_pfn, range_end_pfn);
		end_pfn = clamp(end_pfn, range_start_pfn, range_end_pfn);
		nr_absent -= end_pfn - start_pfn;
	}
	return nr_absent;
}

/**
 * absent_pages_in_range - Return number of page frames in holes within a range
 * @start_pfn: The start PFN to start searching for holes
 * @end_pfn: The end PFN to stop searching for holes
 *
 * Return: the number of pages frames in memory holes within a range.
 */
unsigned long __init absent_pages_in_range(unsigned long start_pfn,
							unsigned long end_pfn)
{
	return __absent_pages_in_range(MAX_NUMNODES, start_pfn, end_pfn);
}

/* Return the number of page frames in holes in a zone on a node */
static unsigned long __init zone_absent_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long zone_start_pfn,
					unsigned long zone_end_pfn)
{
	unsigned long nr_absent;

	/* zone is empty, we don't have any absent pages */
	if (zone_start_pfn == zone_end_pfn)
		return 0;

	nr_absent = __absent_pages_in_range(nid, zone_start_pfn, zone_end_pfn);

	/*
	 * ZONE_MOVABLE handling.
	 * Treat pages to be ZONE_MOVABLE in ZONE_NORMAL as absent pages
	 * and vice versa.
	 */
	if (mirrored_kernelcore && zone_movable_pfn[nid]) {
		unsigned long start_pfn, end_pfn;
		struct memblock_region *r;

		for_each_mem_region(r) {
			start_pfn = clamp(memblock_region_memory_base_pfn(r),
					  zone_start_pfn, zone_end_pfn);
			end_pfn = clamp(memblock_region_memory_end_pfn(r),
					zone_start_pfn, zone_end_pfn);

			if (zone_type == ZONE_MOVABLE &&
			    memblock_is_mirror(r))
				nr_absent += end_pfn - start_pfn;

			if (zone_type == ZONE_NORMAL &&
			    !memblock_is_mirror(r))
				nr_absent += end_pfn - start_pfn;
		}
	}

	return nr_absent;
}

/*
 * Return the number of pages a zone spans in a node, including holes
 * present_pages = zone_spanned_pages_in_node() - zone_absent_pages_in_node()
 */
static unsigned long __init zone_spanned_pages_in_node(int nid,
					unsigned long zone_type,
					unsigned long node_start_pfn,
					unsigned long node_end_pfn,
					unsigned long *zone_start_pfn,
					unsigned long *zone_end_pfn)
{
	unsigned long zone_low = arch_zone_lowest_possible_pfn[zone_type];
	unsigned long zone_high = arch_zone_highest_possible_pfn[zone_type];

	/* Get the start and end of the zone */
	*zone_start_pfn = clamp(node_start_pfn, zone_low, zone_high);
	*zone_end_pfn = clamp(node_end_pfn, zone_low, zone_high);
	adjust_zone_range_for_zone_movable(nid, zone_type, node_end_pfn,
					   zone_start_pfn, zone_end_pfn);

	/* Check that this node has pages within the zone's required range */
	if (*zone_end_pfn < node_start_pfn || *zone_start_pfn > node_end_pfn)
		return 0;

	/* Move the zone boundaries inside the node if necessary */
	*zone_end_pfn = min(*zone_end_pfn, node_end_pfn);
	*zone_start_pfn = max(*zone_start_pfn, node_start_pfn);

	/* Return the spanned pages */
	return *zone_end_pfn - *zone_start_pfn;
}

static void __init reset_memoryless_node_totalpages(struct pglist_data *pgdat)
{
	struct zone *z;

	for (z = pgdat->node_zones; z < pgdat->node_zones + MAX_NR_ZONES; z++) {
		z->zone_start_pfn = 0;
		z->spanned_pages = 0;
		z->present_pages = 0;
#if defined(CONFIG_MEMORY_HOTPLUG)
		z->present_early_pages = 0;
#endif
	}

	pgdat->node_spanned_pages = 0;
	pgdat->node_present_pages = 0;
	pr_debug("On node %d totalpages: 0\n", pgdat->node_id);
}
/*用于计算系统中用于内核的总页面数，以及系统中所有页面的总数*/
static void __init calc_nr_kernel_pages(void)
{
	unsigned long start_pfn, end_pfn;// 定义起始和结束页帧号
	phys_addr_t start_addr, end_addr;//定义物理地址的起始和结束值
	u64 u;
#ifdef CONFIG_HIGHMEM
	unsigned long high_zone_low = arch_zone_lowest_possible_pfn[ZONE_HIGHMEM];//用于获取高端内存区域的最低可用页帧号
#endif

	for_each_free_mem_range(u, NUMA_NO_NODE, MEMBLOCK_NONE, &start_addr, &end_addr, NULL) {//遍历系统中所有的空闲内存块，获取每个块的起始和结束地址
		start_pfn = PFN_UP(start_addr);//将起始地址向上对齐为页帧号
		end_pfn   = PFN_DOWN(end_addr);//将结束地址向下对齐为页帧号

		if (start_pfn < end_pfn) {//如果起始页帧号小于结束页帧号，说明内存区域有效
			nr_all_pages += end_pfn - start_pfn;//增加总页数，表示系统中所有的页面数量
#ifdef CONFIG_HIGHMEM
			start_pfn = clamp(start_pfn, 0, high_zone_low);//如果启用了高端内存，将起始和结束页帧号限制在高端内存以下
			end_pfn = clamp(end_pfn, 0, high_zone_low);
#endif
			nr_kernel_pages += end_pfn - start_pfn;//计算属于内核使用的页面数量，增加到 nr_kernel_pages 中
		}
	}
}

static void __init calculate_node_totalpages(struct pglist_data *pgdat,
						unsigned long node_start_pfn,
						unsigned long node_end_pfn)
{
	unsigned long realtotalpages = 0, totalpages = 0;
	enum zone_type i;

	for (i = 0; i < MAX_NR_ZONES; i++) {
		struct zone *zone = pgdat->node_zones + i;
		unsigned long zone_start_pfn, zone_end_pfn;
		unsigned long spanned, absent;
		unsigned long real_size;

		spanned = zone_spanned_pages_in_node(pgdat->node_id, i,
						     node_start_pfn,
						     node_end_pfn,
						     &zone_start_pfn,
						     &zone_end_pfn);
		absent = zone_absent_pages_in_node(pgdat->node_id, i,
						   zone_start_pfn,
						   zone_end_pfn);

		real_size = spanned - absent;

		if (spanned)
			zone->zone_start_pfn = zone_start_pfn;
		else
			zone->zone_start_pfn = 0;
		zone->spanned_pages = spanned;
		zone->present_pages = real_size;
#if defined(CONFIG_MEMORY_HOTPLUG)
		zone->present_early_pages = real_size;
#endif

		totalpages += spanned;
		realtotalpages += real_size;
	}

	pgdat->node_spanned_pages = totalpages;
	pgdat->node_present_pages = realtotalpages;
	pr_debug("On node %d totalpages: %lu\n", pgdat->node_id, realtotalpages);
}

#ifdef CONFIG_TRANSPARENT_HUGEPAGE
static void pgdat_init_split_queue(struct pglist_data *pgdat)
{
	struct deferred_split *ds_queue = &pgdat->deferred_split_queue;

	spin_lock_init(&ds_queue->split_queue_lock);
	INIT_LIST_HEAD(&ds_queue->split_queue);
	ds_queue->split_queue_len = 0;
}
#else
static void pgdat_init_split_queue(struct pglist_data *pgdat) {}
#endif

#ifdef CONFIG_COMPACTION
static void pgdat_init_kcompactd(struct pglist_data *pgdat)
{
	init_waitqueue_head(&pgdat->kcompactd_wait);
}
#else
static void pgdat_init_kcompactd(struct pglist_data *pgdat) {}
#endif
/*用于初始化 pglist_data 结构中的内部结构*/
static void __meminit pgdat_init_internals(struct pglist_data *pgdat)
{
	int i;

	pgdat_resize_init(pgdat);// 初始化节点的动态调整功能
	pgdat_kswapd_lock_init(pgdat);//初始化节点的 kswapd 相关锁

	pgdat_init_split_queue(pgdat);//初始化节点的页面分裂队列
	pgdat_init_kcompactd(pgdat);//初始化节点的内存压缩守护进程

	init_waitqueue_head(&pgdat->kswapd_wait);//初始化 kswapd 等待队列，用于页面回收的调度
	init_waitqueue_head(&pgdat->pfmemalloc_wait);//初始化 pfmemalloc 等待队列，用于内存分配失败时的等待

	for (i = 0; i < NR_VMSCAN_THROTTLE; i++)//遍历初始化内存回收等待队列，用于 vmscan 节流
		init_waitqueue_head(&pgdat->reclaim_wait[i]);

	pgdat_page_ext_init(pgdat);//初始化节点的页面扩展结构
	lruvec_init(&pgdat->__lruvec);//初始化节点的 LRU（Least Recently Used）向量
}

static void __meminit zone_init_internals(struct zone *zone, enum zone_type idx, int nid,
							unsigned long remaining_pages)
{
	atomic_long_set(&zone->managed_pages, remaining_pages);
	zone_set_nid(zone, nid);
	zone->name = zone_names[idx];
	zone->zone_pgdat = NODE_DATA(nid);
	spin_lock_init(&zone->lock);
	zone_seqlock_init(zone);
	zone_pcp_init(zone);
}

static void __meminit zone_init_free_lists(struct zone *zone)
{
	unsigned int order, t;
	for_each_migratetype_order(order, t) {
		INIT_LIST_HEAD(&zone->free_area[order].free_list[t]);//初始化指定阶数和迁移类型的空闲列表
		zone->free_area[order].nr_free = 0;//设置该阶数的空闲页面数量为 0
	}

#ifdef CONFIG_UNACCEPTED_MEMORY
	INIT_LIST_HEAD(&zone->unaccepted_pages);
#endif
}
/*用于初始化一个内存区域（zone）。*/
void __meminit init_currently_empty_zone(struct zone *zone,
					unsigned long zone_start_pfn,
					unsigned long size)
{
	struct pglist_data *pgdat = zone->zone_pgdat;//获取该内存区域所属的节点（pglist_data 结构）
	int zone_idx = zone_idx(zone) + 1;// 获取该内存区域的索引，并加 1

	if (zone_idx > pgdat->nr_zones)//如果当前内存区域的索引超过节点的总区域数，更新节点的区域数
		pgdat->nr_zones = zone_idx;

	zone->zone_start_pfn = zone_start_pfn;// 设置内存区域的起始页帧号

	mminit_dprintk(MMINIT_TRACE, "memmap_init",
			"Initialising map node %d zone %lu pfns %lu -> %lu\n",
			pgdat->node_id,
			(unsigned long)zone_idx(zone),
			zone_start_pfn, (zone_start_pfn + size));//打印调试信息，显示内存初始化的详细信息，包括节点 ID、区域索引、起始页帧号和大小

	zone_init_free_lists(zone);//初始化该区域的空闲列表，用于内存分配器的操作(伙伴系统)
	zone->initialized = 1;//将该区域标记为已初始化
}

#ifndef CONFIG_SPARSEMEM
/*
 * Calculate the size of the zone->blockflags rounded to an unsigned long
 * Start by making sure zonesize is a multiple of pageblock_order by rounding
 * up. Then use 1 NR_PAGEBLOCK_BITS worth of bits per pageblock, finally
 * round what is now in bits to nearest long in bits, then return it in
 * bytes.
 */
static unsigned long __init usemap_size(unsigned long zone_start_pfn, unsigned long zonesize)
{
	unsigned long usemapsize;//用于存储计算后的 usemap 大小

	zonesize += zone_start_pfn & (pageblock_nr_pages-1);// 将区域大小增加 zone_start_pfn 与 pageblock_nr_pages 的对齐偏移量
	usemapsize = roundup(zonesize, pageblock_nr_pages);// 将区域大小向上对齐到 pageblock_nr_pages 的倍数
	usemapsize = usemapsize >> pageblock_order;//计算区域中 pageblock 的数量
	usemapsize *= NR_PAGEBLOCK_BITS;// 计算 usemap 所需的总位数，乘以每个 pageblock 的标记位数
	usemapsize = roundup(usemapsize, BITS_PER_LONG);//将 usemap 大小向上对齐到长整型（BITS_PER_LONG）的倍数

	return usemapsize / BITS_PER_BYTE;//将 usemap 大小转换为字节数
}

static void __ref setup_usemap(struct zone *zone)
{
	unsigned long usemapsize = usemap_size(zone->zone_start_pfn,
					       zone->spanned_pages);// 计算 usemap 的大小，基于区域的起始页帧号和跨度页数
	zone->pageblock_flags = NULL;//初始化 pageblock_flags 为 NULL
	if (usemapsize) {//如果计算的 usemap 大小不为 0
		zone->pageblock_flags =
			memblock_alloc_node(usemapsize, SMP_CACHE_BYTES,
					    zone_to_nid(zone));//为 zone 分配 usemap 内存，按 SMP 缓存行大小对齐，并分配到对应的节点上
		if (!zone->pageblock_flags)//如果内存分配失败
			panic("Failed to allocate %ld bytes for zone %s pageblock flags on node %d\n",
			      usemapsize, zone->name, zone_to_nid(zone));// 触发内核 panic，提示分配失败信息
	}
}
#else
static inline void setup_usemap(struct zone *zone) {}
#endif /* CONFIG_SPARSEMEM */

#ifdef CONFIG_HUGETLB_PAGE_SIZE_VARIABLE

/* Initialise the number of pages represented by NR_PAGEBLOCK_BITS */
void __init set_pageblock_order(void)//用于设置内核中 pageblock 的阶数（order），控制内存分配的粒度。
{
	unsigned int order = MAX_PAGE_ORDER;// 初始化 order 为最大页面阶数

	/* Check that pageblock_nr_pages has not already been setup */
	if (pageblock_order)//检查 pageblock_order 是否已设置
		return;

	/* Don't let pageblocks exceed the maximum allocation granularity. */
	if (HPAGE_SHIFT > PAGE_SHIFT && HUGETLB_PAGE_ORDER < order)//检查大页是否存在且小于当前 order
		order = HUGETLB_PAGE_ORDER;//将 order 设置为大页的阶数

	/*
	 * Assume the largest contiguous order of interest is a huge page.
	 * This value may be variable depending on boot parameters on powerpc.
	 */
	pageblock_order = order;// 将 pageblock_order 设置为当前的 order 值
}
#else /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * When CONFIG_HUGETLB_PAGE_SIZE_VARIABLE is not set, set_pageblock_order()
 * is unused as pageblock_order is set at compile-time. See
 * include/linux/pageblock-flags.h for the values of pageblock_order based on
 * the kernel config
 */
void __init set_pageblock_order(void)
{
}

#endif /* CONFIG_HUGETLB_PAGE_SIZE_VARIABLE */

/*
 * Set up the zone data structures
 * - init pgdat internals
 * - init all zones belonging to this node
 *
 * NOTE: this function is only called during memory hotplug
 */
#ifdef CONFIG_MEMORY_HOTPLUG
void __ref free_area_init_core_hotplug(struct pglist_data *pgdat)
{
	int nid = pgdat->node_id;
	enum zone_type z;
	int cpu;

	pgdat_init_internals(pgdat);

	if (pgdat->per_cpu_nodestats == &boot_nodestats)
		pgdat->per_cpu_nodestats = alloc_percpu(struct per_cpu_nodestat);

	/*
	 * Reset the nr_zones, order and highest_zoneidx before reuse.
	 * Note that kswapd will init kswapd_highest_zoneidx properly
	 * when it starts in the near future.
	 */
	pgdat->nr_zones = 0;
	pgdat->kswapd_order = 0;
	pgdat->kswapd_highest_zoneidx = 0;
	pgdat->node_start_pfn = 0;
	pgdat->node_present_pages = 0;

	for_each_online_cpu(cpu) {
		struct per_cpu_nodestat *p;

		p = per_cpu_ptr(pgdat->per_cpu_nodestats, cpu);
		memset(p, 0, sizeof(*p));
	}

	/*
	 * When memory is hot-added, all the memory is in offline state. So
	 * clear all zones' present_pages and managed_pages because they will
	 * be updated in online_pages() and offline_pages().
	 */
	for (z = 0; z < MAX_NR_ZONES; z++) {
		struct zone *zone = pgdat->node_zones + z;

		zone->present_pages = 0;
		zone_init_internals(zone, z, nid, 0);
	}
}
#endif
/*用于初始化指定节点的内存区域（zone）*/
static void __init free_area_init_core(struct pglist_data *pgdat)
{
	enum zone_type j;
	int nid = pgdat->node_id;// 获取当前节点的 ID

	pgdat_init_internals(pgdat);//初始化 pgdat 的内部结构
	pgdat->per_cpu_nodestats = &boot_nodestats;//将节点的per-CPU统计数据指向初始化的nodestats

	for (j = 0; j < MAX_NR_ZONES; j++) {//遍历当前节点的所有内存区域（zone）
		struct zone *zone = pgdat->node_zones + j;//获取当前节点的第 j 个内存区域
		unsigned long size = zone->spanned_pages;// 获取当前内存区域的跨度页数

		/*
		 * Initialize zone->managed_pages as 0 , it will be reset
		 * when memblock allocator frees pages into buddy system.
		 */
		zone_init_internals(zone, j, nid, zone->present_pages);//初始化内存区域的内部结构，设置区域类型、节点ID和初始页数

		if (!size)//如果内存区域大小为 0，说明该区域没有页
			continue;

		setup_usemap(zone);//初始化内存区域的usemap，用于管理页面块的分配状态
		init_currently_empty_zone(zone, zone->zone_start_pfn, size);//继续初始化当前内存区域
	}
}

void __init *memmap_alloc(phys_addr_t size, phys_addr_t align,
			  phys_addr_t min_addr, int nid, bool exact_nid)
{
	void *ptr;

	if (exact_nid)
		ptr = memblock_alloc_exact_nid_raw(size, align, min_addr,
						   MEMBLOCK_ALLOC_ACCESSIBLE,
						   nid);
	else
		ptr = memblock_alloc_try_nid_raw(size, align, min_addr,
						 MEMBLOCK_ALLOC_ACCESSIBLE,
						 nid);

	if (ptr && size > 0)
		page_init_poison(ptr, size);

	return ptr;
}
/*为指定的节点（NUMA 或 FLATMEM）分配page结构映射。*/
#ifdef CONFIG_FLATMEM
static void __init alloc_node_mem_map(struct pglist_data *pgdat)
{
	unsigned long start, offset, size, end;//定义起始页帧号、偏移量、大小和结束页帧号
	struct page *map;// 定义指针用于存储页表映射

	/* Skip empty nodes */
	if (!pgdat->node_spanned_pages)//如果节点的跨度页数为 0，说明该节点无内存
		return;

	start = pgdat->node_start_pfn & ~(MAX_ORDER_NR_PAGES - 1);// 将节点的起始页帧号对齐到 MAX_ORDER_NR_PAGES 的边界
	offset = pgdat->node_start_pfn - start;// 计算起始页帧号与对齐后的起始页帧号之间的偏移量
	/*
		 * The zone's endpoints aren't required to be MAX_PAGE_ORDER
	 * aligned but the node_mem_map endpoints must be in order
	 * for the buddy allocator to function correctly.
	 */
	end = ALIGN(pgdat_end_pfn(pgdat), MAX_ORDER_NR_PAGES);// 计算节点的结束页帧号，并将其对齐到 MAX_ORDER_NR_PAGES 的边界
	size =  (end - start) * sizeof(struct page);//计算内存映射表的总大小，单位为 struct page 的大小
	map = memmap_alloc(size, SMP_CACHE_BYTES, MEMBLOCK_LOW_LIMIT,
			   pgdat->node_id, false);//分配内存映射表，要求按 SMP 缓存线大小对齐，并分配给低地址区域
	if (!map)// 如果内存映射表分配失败
		panic("Failed to allocate %ld bytes for node %d memory map\n",
		      size, pgdat->node_id);
	pgdat->node_mem_map = map + offset;//将节点的内存映射表指针设置为分配的 map 加上偏移量
	pr_debug("%s: node %d, pgdat %08lx, node_mem_map %08lx\n",
		 __func__, pgdat->node_id, (unsigned long)pgdat,
		 (unsigned long)pgdat->node_mem_map);//打印调试信息，显示节点的内存映射表地址
#ifndef CONFIG_NUMA
	/* the global mem_map is just set as node 0's */
	if (pgdat == NODE_DATA(0)) {//如果当前节点是节点 0
		mem_map = NODE_DATA(0)->node_mem_map;//将全局的 mem_map 设置为节点 0 的内存映射表
		if (page_to_pfn(mem_map) != pgdat->node_start_pfn)// 检查全局 mem_map 的页帧号是否匹配
			mem_map -= offset;// 如果不匹配，调整全局 mem_map 的指针
	}
#endif
}
#else
static inline void alloc_node_mem_map(struct pglist_data *pgdat) { }
#endif /* CONFIG_FLATMEM */

/**
 * get_pfn_range_for_nid - Return the start and end page frames for a node
 * @nid: The nid to return the range for. If MAX_NUMNODES, the min and max PFN are returned.
 * @start_pfn: Passed by reference. On return, it will have the node start_pfn.
 * @end_pfn: Passed by reference. On return, it will have the node end_pfn.
 *
 * It returns the start and end page frame of a node based on information
 * provided by memblock_set_node(). If called for a node
 * with no available memory, the start and end PFNs will be 0.
 * 用于获取指定节点（NUMA 节点）的起始和结束页帧号范围。
 */
void __init get_pfn_range_for_nid(unsigned int nid,
			unsigned long *start_pfn, unsigned long *end_pfn)
{
	unsigned long this_start_pfn, this_end_pfn;//定义变量用于存储当前内存块的起始和结束页帧号
	int i;

	*start_pfn = -1UL;//初始化起始页帧号为最大值，以便后续寻找最小值
	*end_pfn = 0;

	for_each_mem_pfn_range(i, nid, &this_start_pfn, &this_end_pfn, NULL) {//遍历指定节点 nid 的每个内存块，获取其起始和结束页帧号
		*start_pfn = min(*start_pfn, this_start_pfn);//更新 start_pfn 为最小的起始页帧号
		*end_pfn = max(*end_pfn, this_end_pfn);//更新 end_pfn 为最大的结束页帧号
	}

	if (*start_pfn == -1UL)
		*start_pfn = 0;
}
/*用于初始化指定节点的内存管理结构*/
static void __init free_area_init_node(int nid)
{
	pg_data_t *pgdat = NODE_DATA(nid);//获取与节点ID对应的pg_data_t结构
	unsigned long start_pfn = 0;//初始化起始页帧号为0
	unsigned long end_pfn = 0;//初始化结束页帧号为0

	/* pg_data_t should be reset to zero when it's allocated */
	WARN_ON(pgdat->nr_zones || pgdat->kswapd_highest_zoneidx);//警告：pgdat结构未重置

	get_pfn_range_for_nid(nid, &start_pfn, &end_pfn);//获取指定节点的起始和结束页帧号范围

	pgdat->node_id = nid;// 设置节点ID
	pgdat->node_start_pfn = start_pfn;//设置节点起始页帧号
	pgdat->per_cpu_nodestats = NULL;//初始化节点统计为 NULL

	if (start_pfn != end_pfn) {//如果节点有内存
		pr_info("Initmem setup node %d [mem %#018Lx-%#018Lx]\n", nid,
			(u64)start_pfn << PAGE_SHIFT,
			end_pfn ? ((u64)end_pfn << PAGE_SHIFT) - 1 : 0);// 打印初始化内存节点的信息，包括节点ID、起始和结束物理地址

		calculate_node_totalpages(pgdat, start_pfn, end_pfn);//计算节点的总页数
	} else {// 如果节点没有内存（memoryless）
		pr_info("Initmem setup node %d as memoryless\n", nid);//打印节点被设置为无内存的信息

		reset_memoryless_node_totalpages(pgdat);//重置无内存节点的总页数
	}

	alloc_node_mem_map(pgdat);//为节点分配内存映射数组(保存page结构体的内存空间)
	pgdat_set_deferred_range(pgdat);//设置节点的延迟范围

	free_area_init_core(pgdat);//初始化节点的内存区（zone）
	lru_gen_init_pgdat(pgdat);//初始化节点的LRU（Least Recently Used）页面生成器
}

/* Any regular or high memory on that node ? */
static void __init check_for_memory(pg_data_t *pgdat)
{
	enum zone_type zone_type;//定义变量用于表示内存区域类型

	for (zone_type = 0; zone_type <= ZONE_MOVABLE - 1; zone_type++) {//遍历从 ZONE_DMA 到 ZONE_NORMAL 之间的所有内存区域类型
		struct zone *zone = &pgdat->node_zones[zone_type];//获取当前节点的特定内存区域
		if (populated_zone(zone)) {//检查该内存区域是否有人口，即是否有有效页面
			if (IS_ENABLED(CONFIG_HIGHMEM))//如果启用了高端内存（CONFIG_HIGHMEM），设置节点状态为有高端内存
				node_set_state(pgdat->node_id, N_HIGH_MEMORY);
			if (zone_type <= ZONE_NORMAL)//如果当前区域是 ZONE_NORMAL 或更小的区域，设置节点状态为有普通内存
				node_set_state(pgdat->node_id, N_NORMAL_MEMORY);
			break;
		}
	}
}

#if MAX_NUMNODES > 1
/*
 * Figure out the number of possible node ids.
 * 用于设置系统中的节点数量，用于 NUMA（非统一内存访问）架构的初始化。
 */
void __init setup_nr_node_ids(void)
{
	unsigned int highest;//定义一个变量用于存储最高节点编号

	highest = find_last_bit(node_possible_map.bits, MAX_NUMNODES);//找到 node_possible_map 中可能节点的最高位位置.node_possible_map.bits是一个位图，表示系统中可能存在的节点。
	nr_node_ids = highest + 1;//设置 nr_node_ids 为最高节点编号加 1
}
#endif

/*
 * Some architectures, e.g. ARC may have ZONE_HIGHMEM below ZONE_NORMAL. For
 * such cases we allow max_zone_pfn sorted in the descending order
 */
static bool arch_has_descending_max_zone_pfns(void)
{
	return IS_ENABLED(CONFIG_ARC) && !IS_ENABLED(CONFIG_ARC_HAS_PAE40);
}

/**
 * free_area_init - Initialise all pg_data_t and zone data
 * @max_zone_pfn: an array of max PFNs for each zone
 *
 * This will call free_area_init_node() for each active node in the system.
 * Using the page ranges provided by memblock_set_node(), the size of each
 * zone in each node and their holes is calculated. If the maximum PFN
 * between two adjacent zones match, it is assumed that the zone is empty.
 * For example, if arch_max_dma_pfn == arch_max_dma32_pfn, it is assumed
 * that arch_max_dma32_pfn has no pages. It is also assumed that a zone
 * starts where the previous one ended. For example, ZONE_DMA32 starts
 * at arch_max_dma_pfn.
 * free_area_init - 初始化所有 pg_data_t 和区域数据
 * @max_zone_pfn: 每个区域的最大 PFN 数组
 *
 * 这将为系统中每个活动节点调用 free_area_init_node()。使用 memblock_set_node() 提
 * 供的页面范围，计算每个节点中每个区域的大小及其空洞。如果两个相邻区域之间的最大 
 * PFN 匹配，则假定该区域为空。例如，如果 arch_max_dma_pfn == arch_max_dma32_pfn，
 * 则假定 arch_max_dma32_pfn 没有页面。还假定一个区域从上一个区域结束的地方开始。
 * 例如，ZONE_DMA32 从arch_max_dma_pfn 开始。
 */
void __init free_area_init(unsigned long *max_zone_pfn)
{
	unsigned long start_pfn, end_pfn;//定义两个变量用于存储起始和结束的页框号
	int i, nid, zone;
	bool descending;//用于指示区域的页框号是否按降序排列

	/* Record where the zone boundaries are */
	memset(arch_zone_lowest_possible_pfn, 0,
				sizeof(arch_zone_lowest_possible_pfn));//初始化区域最低可能的页框号数组为0
	memset(arch_zone_highest_possible_pfn, 0,
				sizeof(arch_zone_highest_possible_pfn));//初始化区域最高可能的页框号数组为0

	start_pfn = PHYS_PFN(memblock_start_of_DRAM());//获取 DRAM 的起始物理地址对应的页框号
	descending = arch_has_descending_max_zone_pfns();//检查系统架构是否使用降序排列的最大区域页框号

	for (i = 0; i < MAX_NR_ZONES; i++) {//遍历所有内存区域，MAX_NR_ZONES 是系统支持的最大内存区域数量
		if (descending)//如果区域按降序排列，则从最后一个区域开始处理
			zone = MAX_NR_ZONES - i - 1;
		else//否则从第一个区域开始处理
			zone = i;

		if (zone == ZONE_MOVABLE)//跳过 ZONE_MOVABLE 区域
			continue;

		end_pfn = max(max_zone_pfn[zone], start_pfn);//计算当前区域的结束页框号，取最大值，以确保区域覆盖所有可能的页框
		arch_zone_lowest_possible_pfn[zone] = start_pfn;//将当前区域的起始页框号存储到 arch_zone_lowest_possible_pfn 数组中
		arch_zone_highest_possible_pfn[zone] = end_pfn;//将当前区域的结束页框号存储到 arch_zone_highest_possible_pfn 数组中

		start_pfn = end_pfn;//更新下一个区域的起始页框号为当前区域的结束页框号
	}

	/* Find the PFNs that ZONE_MOVABLE begins at in each node */
	memset(zone_movable_pfn, 0, sizeof(zone_movable_pfn));//清空zone_movable_pfn数组，用于存储ZONE_MOVABLE区域的起始页框号
	find_zone_movable_pfns_for_nodes();//查找并记录每个节点中ZONE_MOVABLE区域的起始页框号

	/* Print out the zone ranges */
	pr_info("Zone ranges:\n");//打印出各个内存区域的范围信息
	for (i = 0; i < MAX_NR_ZONES; i++) {//再次遍历所有内存区域，打印每个区域的范围
		if (i == ZONE_MOVABLE)//跳过 ZONE_MOVABLE 区域
			continue;
		pr_info("  %-8s ", zone_names[i]);//打印区域的名称
		if (arch_zone_lowest_possible_pfn[i] ==
				arch_zone_highest_possible_pfn[i])//如果区域为空（即起始和结束页框号相同），打印 "empty"
			pr_cont("empty\n");
		else//否则打印区域的内存范围
			pr_cont("[mem %#018Lx-%#018Lx]\n",
				(u64)arch_zone_lowest_possible_pfn[i]
					<< PAGE_SHIFT,
				((u64)arch_zone_highest_possible_pfn[i]
					<< PAGE_SHIFT) - 1);
	}

	/* Print out the PFNs ZONE_MOVABLE begins at in each node */
	pr_info("Movable zone start for each node\n");//打印出每个节点中ZONE_MOVABLE区域的起始页框号
	for (i = 0; i < MAX_NUMNODES; i++) {//遍历所有可能的节点
		if (zone_movable_pfn[i])//如果该节点有定义ZONE_MOVABLE起始页框号，打印其地址
			pr_info("  Node %d: %#018Lx\n", i,
			       (u64)zone_movable_pfn[i] << PAGE_SHIFT);
	}

	/*
	 * Print out the early node map, and initialize the
	 * subsection-map relative to active online memory ranges to
	 * enable future "sub-section" extensions of the memory map.
	 */
	pr_info("Early memory node ranges\n");//打印出早期节点的内存范围，并初始化子节映射，以支持未来的内存扩展
	for_each_mem_pfn_range(i, MAX_NUMNODES, &start_pfn, &end_pfn, &nid) {//遍历系统中每个内存页框号范围，获取对应的节点ID
		pr_info("  node %3d: [mem %#018Lx-%#018Lx]\n", nid,
			(u64)start_pfn << PAGE_SHIFT,
			((u64)end_pfn << PAGE_SHIFT) - 1);//打印每个节点的内存范围
		subsection_map_init(start_pfn, end_pfn - start_pfn);//初始化子节映射，以支持内存的子节扩展
	}

	/* 初始化每个节点的内存管理数据结构 */
	mminit_verify_pageflags_layout();// 验证页面标志的布局
	setup_nr_node_ids();// 设置节点 ID 的数量
	set_pageblock_order();//用于设置内核中 pageblock 的大小阶数(order)

	for_each_node(nid) {//遍历所有节点，初始化每个节点的内存管理数据
		pg_data_t *pgdat;

		if (!node_online(nid)) {//如果内存节点还没有在线，则分配内存节点的数据结构
			/* Allocator not initialized yet */
			pgdat = arch_alloc_nodedata(nid);//为内存节点分配内存管理数据结构
			if (!pgdat)//如果分配失败，打印错误信息并触发 panic
				panic("Cannot allocate %zuB for node %d.\n",
				       sizeof(*pgdat), nid);
			arch_refresh_nodedata(nid, pgdat);//刷新内存节点的数据结构状态
		}

		pgdat = NODE_DATA(nid);//获取节点的数据结构
		free_area_init_node(nid);//初始化节点的空闲内存区域

		/*
		 * No sysfs hierarcy will be created via register_one_node()
		 *for memory-less node because here it's not marked as N_MEMORY
		 *and won't be set online later. The benefit is userspace
		 *program won't be confused by sysfs files/directories of
		 *memory-less node. The pgdat will get fully initialized by
		 *hotadd_init_pgdat() when memory is hotplugged into this node.
		 */
		if (pgdat->node_present_pages) {//如果节点有物理内存页，设置节点状态为 N_MEMORY
			node_set_state(nid, N_MEMORY);
			check_for_memory(pgdat);//检查节点的内存
		}
	}

	calc_nr_kernel_pages();//计算内核页的数量
	memmap_init();//初始化内存页page结构

	/* 如果系统只有一个节点，禁用内存哈希分布 */
	fixup_hashdist();//修复哈希分布
}

/**
 * node_map_pfn_alignment - determine the maximum internode alignment
 *
 * This function should be called after node map is populated and sorted.
 * It calculates the maximum power of two alignment which can distinguish
 * all the nodes.
 *
 * For example, if all nodes are 1GiB and aligned to 1GiB, the return value
 * would indicate 1GiB alignment with (1 << (30 - PAGE_SHIFT)).  If the
 * nodes are shifted by 256MiB, 256MiB.  Note that if only the last node is
 * shifted, 1GiB is enough and this function will indicate so.
 *
 * This is used to test whether pfn -> nid mapping of the chosen memory
 * model has fine enough granularity to avoid incorrect mapping for the
 * populated node map.
 *
 * Return: the determined alignment in pfn's.  0 if there is no alignment
 * requirement (single node).
 */
unsigned long __init node_map_pfn_alignment(void)
{
	unsigned long accl_mask = 0, last_end = 0;
	unsigned long start, end, mask;
	int last_nid = NUMA_NO_NODE;
	int i, nid;

	for_each_mem_pfn_range(i, MAX_NUMNODES, &start, &end, &nid) {
		if (!start || last_nid < 0 || last_nid == nid) {
			last_nid = nid;
			last_end = end;
			continue;
		}

		/*
		 * Start with a mask granular enough to pin-point to the
		 * start pfn and tick off bits one-by-one until it becomes
		 * too coarse to separate the current node from the last.
		 */
		mask = ~((1 << __ffs(start)) - 1);
		while (mask && last_end <= (start & (mask << 1)))
			mask <<= 1;

		/* accumulate all internode masks */
		accl_mask |= mask;
	}

	/* convert mask to number of pages */
	return ~accl_mask + 1;
}

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT
static void __init deferred_free_range(unsigned long pfn,
				       unsigned long nr_pages)
{
	struct page *page;
	unsigned long i;

	if (!nr_pages)
		return;

	page = pfn_to_page(pfn);

	/* Free a large naturally-aligned chunk if possible */
	if (nr_pages == MAX_ORDER_NR_PAGES && IS_MAX_ORDER_ALIGNED(pfn)) {
		for (i = 0; i < nr_pages; i += pageblock_nr_pages)
			set_pageblock_migratetype(page + i, MIGRATE_MOVABLE);
		__free_pages_core(page, MAX_PAGE_ORDER);
		return;
	}

	/* Accept chunks smaller than MAX_PAGE_ORDER upfront */
	accept_memory(PFN_PHYS(pfn), PFN_PHYS(pfn + nr_pages));

	for (i = 0; i < nr_pages; i++, page++, pfn++) {
		if (pageblock_aligned(pfn))
			set_pageblock_migratetype(page, MIGRATE_MOVABLE);
		__free_pages_core(page, 0);
	}
}

/* Completion tracking for deferred_init_memmap() threads */
static atomic_t pgdat_init_n_undone __initdata;
static __initdata DECLARE_COMPLETION(pgdat_init_all_done_comp);

static inline void __init pgdat_init_report_one_done(void)
{
	if (atomic_dec_and_test(&pgdat_init_n_undone))
		complete(&pgdat_init_all_done_comp);
}

/*
 * Returns true if page needs to be initialized or freed to buddy allocator.
 *
 * We check if a current MAX_PAGE_ORDER block is valid by only checking the
 * validity of the head pfn.
 */
static inline bool __init deferred_pfn_valid(unsigned long pfn)
{
	if (IS_MAX_ORDER_ALIGNED(pfn) && !pfn_valid(pfn))
		return false;
	return true;
}

/*
 * Free pages to buddy allocator. Try to free aligned pages in
 * MAX_ORDER_NR_PAGES sizes.
 */
static void __init deferred_free_pages(unsigned long pfn,
				       unsigned long end_pfn)
{
	unsigned long nr_free = 0;//记录连续可释放页的数量。

	for (; pfn < end_pfn; pfn++) {//遍历从 pfn 到 end_pfn 的页帧。
		if (!deferred_pfn_valid(pfn)) {//如果当前页帧号无效
			deferred_free_range(pfn - nr_free, nr_free);//释放从当前页帧号减去累计数量开始的页
			nr_free = 0;
		} else if (IS_MAX_ORDER_ALIGNED(pfn)) {//如果当前页帧号按最大页块边界对齐：
			deferred_free_range(pfn - nr_free, nr_free);//释放从当前页帧号减去累计数量开始的页。
			nr_free = 1;//重置计数器，当前页可释放。
		} else {
			nr_free++;//增加连续可释放页的数量。
		}
	}
	/* Free the last block of pages to allocator */
	deferred_free_range(pfn - nr_free, nr_free);//释放最后一批累计的页。
}

/*
 * Initialize struct pages.  We minimize pfn page lookups and scheduler checks
 * by performing it only once every MAX_ORDER_NR_PAGES.
 * Return number of pages initialized.
 * 初始化一段物理内存页帧的元数据 (struct page)
 */
static unsigned long  __init deferred_init_pages(struct zone *zone,
						 unsigned long pfn,
						 unsigned long end_pfn)
{
	int nid = zone_to_nid(zone);//获取当前 zone 所属的节点 ID (NUMA 节点)
	unsigned long nr_pages = 0;//统计初始化的页数。
	int zid = zone_idx(zone);//获取当前 zone 的索引。
	struct page *page = NULL;

	for (; pfn < end_pfn; pfn++) {//遍历指定范围内的页帧号。
		if (!deferred_pfn_valid(pfn)) {//如果当前页帧号无效，则跳过处理。
			page = NULL;//清空当前 page 指针。
			continue;
		} else if (!page || IS_MAX_ORDER_ALIGNED(pfn)) {//如果 page 指针为空或页帧号对齐到最大页块边界，则重新获取 page。
			page = pfn_to_page(pfn);//将页帧号转换为 page 指针。
		} else {
			page++;//否则，移动到下一个 page。
		}
		__init_single_page(page, pfn, zid, nid);//初始化单个页。
		nr_pages++;
	}
	return nr_pages;//返回初始化的页数。
}

/*
 * This function is meant to pre-load the iterator for the zone init.
 * Specifically it walks through the ranges until we are caught up to the
 * first_init_pfn value and exits there. If we never encounter the value we
 * return false indicating there are no valid ranges left.
 */
static bool __init
deferred_init_mem_pfn_range_in_zone(u64 *i, struct zone *zone,
				    unsigned long *spfn, unsigned long *epfn,
				    unsigned long first_init_pfn)
{
	u64 j;

	/*
	 * Start out by walking through the ranges in this zone that have
	 * already been initialized. We don't need to do anything with them
	 * so we just need to flush them out of the system.
	 */
	for_each_free_mem_pfn_range_in_zone(j, zone, spfn, epfn) {
		if (*epfn <= first_init_pfn)
			continue;
		if (*spfn < first_init_pfn)
			*spfn = first_init_pfn;
		*i = j;
		return true;
	}

	return false;
}

/*
 * Initialize and free pages. We do it in two loops: first we initialize
 * struct page, then free to buddy allocator, because while we are
 * freeing pages we can access pages that are ahead (computing buddy
 * page in __free_one_page()).
 *
 * In order to try and keep some memory in the cache we have the loop
 * broken along max page order boundaries. This way we will not cause
 * any issues with the buddy page computation.
 */
static unsigned long __init
deferred_init_maxorder(u64 *i, struct zone *zone, unsigned long *start_pfn,
		       unsigned long *end_pfn)
{
	unsigned long mo_pfn = ALIGN(*start_pfn + 1, MAX_ORDER_NR_PAGES);//计算最大页块 (MAX_ORDER) 的对齐地址，mo_pfn 是对齐后的页帧号。
	unsigned long spfn = *start_pfn, epfn = *end_pfn;//备份起始页帧号 (spfn) 和结束页帧号 (epfn)。
	unsigned long nr_pages = 0;//初始化计数器，用于记录初始化的页数量。
	u64 j = *i;//备份迭代器，用于遍历 zone 的内存范围。

	/* 第一轮：初始化页的内容 */
	for_each_free_mem_pfn_range_in_zone_from(j, zone, start_pfn, end_pfn) {//遍历 zone 中从 `start_pfn` 开始的空闲内存范围。
		unsigned long t;

		if (mo_pfn <= *start_pfn)//如果 mo_pfn 小于等于 start_pfn，说明对齐范围已经处理完毕，退出循环。
			break;

		t = min(mo_pfn, *end_pfn);//t 为当前范围的结束页帧号，取 mo_pfn 和 end_pfn 的较小值。
		nr_pages += deferred_init_pages(zone, *start_pfn, t);//初始化从 start_pfn 到 t 范围内的页，并累加初始化的页数。

		if (mo_pfn < *end_pfn) {//如果 mo_pfn 小于 end_pfn，说明还有未对齐的页需要处理。
			*start_pfn = mo_pfn;//更新 start_pfn 为对齐后的页帧号。
			break;
		}
	}

	/* 第二轮：释放页以供伙伴分配器使用 */
	swap(j, *i);//交换迭代器，准备重新遍历。

	for_each_free_mem_pfn_range_in_zone_from(j, zone, &spfn, &epfn) {//遍历 zone 中从 spfn 开始的空闲内存范围。
		unsigned long t;

		if (mo_pfn <= spfn)//如果 mo_pfn 小于等于 spfn，说明对齐范围已经处理完毕，退出循环。
			break;

		t = min(mo_pfn, epfn);//t 为当前范围的结束页帧号，取 mo_pfn 和 epfn 的较小值。
		deferred_free_pages(spfn, t);//释放 spfn 到 t 范围内的页，供伙伴分配器使用。

		if (mo_pfn <= epfn)//如果 mo_pfn 小于等于 epfn，说明当前范围已处理完毕，退出循环。
			break;
	}

	return nr_pages;//返回已初始化的页数量。
}

static void __init
deferred_init_memmap_chunk(unsigned long start_pfn, unsigned long end_pfn,
			   void *arg)
{
	unsigned long spfn, epfn;//当前处理的页帧范围：起始页帧号 (spfn) 和结束页帧号 (epfn)
	struct zone *zone = arg;//指向当前处理的内存区域 (zone) 的指针
	u64 i;//用于存储页帧范围初始化状态

	deferred_init_mem_pfn_range_in_zone(&i, zone, &spfn, &epfn, start_pfn);//调用函数获取 zone 内从 start_pfn 开始的有效页帧范围，并更新 spfn 和 epfn 的值。

	/*
	 * Initialize and free pages in MAX_PAGE_ORDER sized increments so that
	 * we can avoid introducing any issues with the buddy allocator.
	 * 按 MAX_PAGE_ORDER 的大小增量初始化和释放页面，以避免对伙伴分配器引入问题。
	 */
	while (spfn < end_pfn) {
		deferred_init_maxorder(&i, zone, &spfn, &epfn);//按最大页块大小 (MAX_PAGE_ORDER) 初始化页帧，并释放到伙伴分配器中。
		cond_resched();//主动检查是否需要让出 CPU，避免阻塞调度器。
	}
}

/* An arch may override for more concurrency. */
__weak int __init
deferred_page_init_max_threads(const struct cpumask *node_cpumask)
{
	return 1;
}

/* Initialise remaining memory on a node */
static int __init deferred_init_memmap(void *data)
{
	pg_data_t *pgdat = data;//获取与节点关联的内存描述符
	const struct cpumask *cpumask = cpumask_of_node(pgdat->node_id);//获取对应节点的 CPU 掩码
	unsigned long spfn = 0, epfn = 0;//起始页帧号和结束页帧号
	unsigned long first_init_pfn, flags;
	unsigned long start = jiffies;//记录开始时间
	struct zone *zone;//内存区域指针
	int zid, max_threads;
	u64 i;

	/* 将内存初始化线程绑定到本地节点的 CPU，以提高效率 */
	if (!cpumask_empty(cpumask))
		set_cpus_allowed_ptr(current, cpumask);

	pgdat_resize_lock(pgdat, &flags);//加锁，防止内存区域大小被修改
	first_init_pfn = pgdat->first_deferred_pfn;//获取起始页帧号
	if (first_init_pfn == ULONG_MAX) {//如果没有需要初始化的页
		pgdat_resize_unlock(pgdat, &flags);//释放锁
		pgdat_init_report_one_done();//报告节点初始化已完成
		return 0;
	}

	/* Sanity check boundaries */
	BUG_ON(pgdat->first_deferred_pfn < pgdat->node_start_pfn);//起始页帧号不能小于节点起始页帧号
	BUG_ON(pgdat->first_deferred_pfn > pgdat_end_pfn(pgdat));//起始页帧号不能超出节点范围
	pgdat->first_deferred_pfn = ULONG_MAX;//重置起始页帧号

	/*
	 * Once we unlock here, the zone cannot be grown anymore, thus if an
	 * interrupt thread must allocate this early in boot, zone must be
	 * pre-grown prior to start of deferred page initialization.
	 * 在这里解锁后，zone 的大小将不再被修改。如果在启动阶段有中断线程
	 * 需要分配内存，zone 必须提前扩展。
	 */
	pgdat_resize_unlock(pgdat, &flags);//释放内存区域大小锁

	/* 只对最高级别的 zone 进行延迟初始化 */
	for (zid = 0; zid < MAX_NR_ZONES; zid++) {//遍历当前节点的所有内存区域
		zone = pgdat->node_zones + zid;//获取当前节点的内存区域
		if (first_init_pfn < zone_end_pfn(zone))// 判断页帧号是否在该 zone 内
			break;
	}

	/* 如果 zone 为空，说明内存初始化已经完成 */
	if (!deferred_init_mem_pfn_range_in_zone(&i, zone, &spfn, &epfn,
						 first_init_pfn))//以zone为单位更新页帧范围
		goto zone_empty;

	max_threads = deferred_page_init_max_threads(cpumask);//计算最大线程数,当前为1

	while (spfn < epfn) {
		unsigned long epfn_align = ALIGN(epfn, PAGES_PER_SECTION);//将结束页帧号对齐到节边界
		struct padata_mt_job job = {//配置多线程初始化任务(以zone为单位进行)
			.thread_fn   = deferred_init_memmap_chunk,//初始化函数
			.fn_arg      = zone,//传递 zone 结构体
			.start       = spfn,//起始页帧号
			.size        = epfn_align - spfn,//初始化的页帧数
			.align       = PAGES_PER_SECTION,//每个线程的最小对齐大小
			.min_chunk   = PAGES_PER_SECTION,//每次最小初始化页数
			.max_threads = max_threads,//最大线程数
			.numa_aware  = false,//关闭 NUMA 感知模式
		};

		padata_do_multithreaded(&job);//启动多线程初始化
		deferred_init_mem_pfn_range_in_zone(&i, zone, &spfn, &epfn,
						    epfn_align);// 更新页帧范围
	}
zone_empty:
	/* Sanity check that the next zone really is unpopulated */
	WARN_ON(++zid < MAX_NR_ZONES && populated_zone(++zone));//检查下一个 zone 是否为空

	pr_info("node %d deferred pages initialised in %ums\n",
		pgdat->node_id, jiffies_to_msecs(jiffies - start));//打印初始化完成信息

	pgdat_init_report_one_done();//报告节点的延迟初始化已完成
	return 0;
}

/*
 * If this zone has deferred pages, try to grow it by initializing enough
 * deferred pages to satisfy the allocation specified by order, rounded up to
 * the nearest PAGES_PER_SECTION boundary.  So we're adding memory in increments
 * of SECTION_SIZE bytes by initializing struct pages in increments of
 * PAGES_PER_SECTION * sizeof(struct page) bytes.
 *
 * Return true when zone was grown, otherwise return false. We return true even
 * when we grow less than requested, to let the caller decide if there are
 * enough pages to satisfy the allocation.
 */
bool __init deferred_grow_zone(struct zone *zone, unsigned int order)
{
	unsigned long nr_pages_needed = ALIGN(1 << order, PAGES_PER_SECTION);
	pg_data_t *pgdat = zone->zone_pgdat;
	unsigned long first_deferred_pfn = pgdat->first_deferred_pfn;
	unsigned long spfn, epfn, flags;
	unsigned long nr_pages = 0;
	u64 i;

	/* Only the last zone may have deferred pages */
	if (zone_end_pfn(zone) != pgdat_end_pfn(pgdat))
		return false;

	pgdat_resize_lock(pgdat, &flags);

	/*
	 * If someone grew this zone while we were waiting for spinlock, return
	 * true, as there might be enough pages already.
	 */
	if (first_deferred_pfn != pgdat->first_deferred_pfn) {
		pgdat_resize_unlock(pgdat, &flags);
		return true;
	}

	/* If the zone is empty somebody else may have cleared out the zone */
	if (!deferred_init_mem_pfn_range_in_zone(&i, zone, &spfn, &epfn,
						 first_deferred_pfn)) {
		pgdat->first_deferred_pfn = ULONG_MAX;
		pgdat_resize_unlock(pgdat, &flags);
		/* Retry only once. */
		return first_deferred_pfn != ULONG_MAX;
	}

	/*
	 * Initialize and free pages in MAX_PAGE_ORDER sized increments so
	 * that we can avoid introducing any issues with the buddy
	 * allocator.
	 */
	while (spfn < epfn) {
		/* update our first deferred PFN for this section */
		first_deferred_pfn = spfn;

		nr_pages += deferred_init_maxorder(&i, zone, &spfn, &epfn);
		touch_nmi_watchdog();

		/* We should only stop along section boundaries */
		if ((first_deferred_pfn ^ spfn) < PAGES_PER_SECTION)
			continue;

		/* If our quota has been met we can stop here */
		if (nr_pages >= nr_pages_needed)
			break;
	}

	pgdat->first_deferred_pfn = spfn;
	pgdat_resize_unlock(pgdat, &flags);

	return nr_pages > 0;
}

#endif /* CONFIG_DEFERRED_STRUCT_PAGE_INIT */

#ifdef CONFIG_CMA
void __init init_cma_reserved_pageblock(struct page *page)
{
	unsigned i = pageblock_nr_pages;
	struct page *p = page;

	do {
		__ClearPageReserved(p);
		set_page_count(p, 0);
	} while (++p, --i);

	set_pageblock_migratetype(page, MIGRATE_CMA);
	set_page_refcounted(page);
	__free_pages(page, pageblock_order);

	adjust_managed_page_count(page, pageblock_nr_pages);
	page_zone(page)->cma_pages += pageblock_nr_pages;
}
#endif

void set_zone_contiguous(struct zone *zone)
{
	unsigned long block_start_pfn = zone->zone_start_pfn;
	unsigned long block_end_pfn;

	block_end_pfn = pageblock_end_pfn(block_start_pfn);
	for (; block_start_pfn < zone_end_pfn(zone);
			block_start_pfn = block_end_pfn,
			 block_end_pfn += pageblock_nr_pages) {

		block_end_pfn = min(block_end_pfn, zone_end_pfn(zone));

		if (!__pageblock_pfn_to_page(block_start_pfn,
					     block_end_pfn, zone))
			return;
		cond_resched();
	}

	/* We confirm that there is no hole */
	zone->contiguous = true;
}

void __init page_alloc_init_late(void)//系统启动后期内存页面分配初始化函数
{
	struct zone *zone;
	int nid;

#ifdef CONFIG_DEFERRED_STRUCT_PAGE_INIT

	/* 将未完成的节点数量设置为具有内存的节点数量 */
	atomic_set(&pgdat_init_n_undone, num_node_state(N_MEMORY));
	for_each_node_state(nid, N_MEMORY) {//遍历所有有内存的节点
		kthread_run(deferred_init_memmap, NODE_DATA(nid), "pgdatinit%d", nid);//为每个内存节点启动内核线程，异步初始化内存页结构
	}

	/* 等待所有线程完成初始化  */
	wait_for_completion(&pgdat_init_all_done_comp);

	/*
	 * 完成延迟初始化的页面后，永久禁用按需的 struct page 初始化
	 */
	static_branch_disable(&deferred_pages);

	/* Reinit limits that are based on free pages after the kernel is up */
	files_maxfiles_init();//根据新的页面状态更新文件描述符上限
#endif

	buffer_init();//初始化内核缓冲区

	/* Discard memblock private memory */
	memblock_discard();//丢弃 memblock 分配的私有内存，以释放保留的内存

	for_each_node_state(nid, N_MEMORY)
		shuffle_free_memory(NODE_DATA(nid));//随机化空闲内存页，提高内存安全性

	for_each_populated_zone(zone)
		set_zone_contiguous(zone);//设置每个内存区域的连续标志

	/* 在所有 struct pages 初始化完成后，初始化 page_ext 结构 */
	if (deferred_struct_pages)
		page_ext_init();//如果存在延迟 struct page 初始化，则初始化 page_ext 数据

	page_alloc_sysctl_init();//注册 sysctl 接口以管理页面分配的系统参数
}

/*
 * Adaptive scale is meant to reduce sizes of hash tables on large memory
 * machines. As memory size is increased the scale is also increased but at
 * slower pace.  Starting from ADAPT_SCALE_BASE (64G), every time memory
 * quadruples the scale is increased by one, which means the size of hash table
 * only doubles, instead of quadrupling as well.
 * Because 32-bit systems cannot have large physical memory, where this scaling
 * makes sense, it is disabled on such platforms.
 */
#if __BITS_PER_LONG > 32
#define ADAPT_SCALE_BASE	(64ul << 30)
#define ADAPT_SCALE_SHIFT	2
#define ADAPT_SCALE_NPAGES	(ADAPT_SCALE_BASE >> PAGE_SHIFT)
#endif

/*
 * allocate a large system hash table from bootmem
 * - it is assumed that the hash table must contain an exact power-of-2
 *   quantity of entries
 * - limit is the number of hash buckets, not the total allocation sizea
 * 从启动内存中分配一个大的哈希表
 * - 假设哈希表必须包含确切的 2 的幂的条目数量
 * - 限制是哈希桶的数量，而不是总分配大小
 */
void *__init alloc_large_system_hash(const char *tablename,
				     unsigned long bucketsize,
				     unsigned long numentries,
				     int scale,
				     int flags,
				     unsigned int *_hash_shift,
				     unsigned int *_hash_mask,
				     unsigned long low_limit,
				     unsigned long high_limit)
{
	unsigned long long max = high_limit;//设置最大值为 high_limit，用于限制分配的最大数量
	unsigned long log2qty, size;//log2qty 存储条目数量的对数值，size 存储哈希表的总大小
	void *table;// 定义指针用于存储哈希表的基地址
	gfp_t gfp_flags;//定义 GFP 标志，用于内存分配的控制
	bool virt;//标识是否使用虚拟内存分配
	bool huge;// 标识是否使用大页分配

	/* allow the kernel cmdline to have a say */
	if (!numentries) {//如果 numentries 为 0，则由系统自动决定条目数量
		/* round applicable memory size up to nearest megabyte */
		numentries = nr_kernel_pages;//默认使用系统内核页数作为条目数量

		/* It isn't necessary when PAGE_SIZE >= 1MB */
		if (PAGE_SIZE < SZ_1M)// 如果页面大小小于 1MB
			numentries = round_up(numentries, SZ_1M / PAGE_SIZE);//将页面数量向上调整到 1MB 的倍数

#if __BITS_PER_LONG > 32
		if (!high_limit) {// 如果 high_limit 未设置
			unsigned long adapt;//定义变量用于调整比例

			for (adapt = ADAPT_SCALE_NPAGES; adapt < numentries;
			     adapt <<= ADAPT_SCALE_SHIFT)//根据系统规模调整比例
				scale++;
		}
#endif

		/* limit to 1 bucket per 2^scale bytes of low memory */
		if (scale > PAGE_SHIFT)//根据scale调整条目数量，限制每个桶的大小
			numentries >>= (scale - PAGE_SHIFT);
		else
			numentries <<= (PAGE_SHIFT - scale);

		if (unlikely((numentries * bucketsize) < PAGE_SIZE))//如果总大小小于一页
			numentries = PAGE_SIZE / bucketsize;// 调整条目数量，至少保证一页
	}
	numentries = roundup_pow_of_two(numentries);//将条目数量调整为2的幂

	/* limit allocation size to 1/16 total memory by default */
	if (max == 0) {//如果最大值未设置
		max = ((unsigned long long)nr_all_pages << PAGE_SHIFT) >> 4;//默认限制为总内存的 1/16
		do_div(max, bucketsize);//计算最大条目数量
	}
	max = min(max, 0x80000000ULL);// 限制最大值不超过 2GB

	if (numentries < low_limit)// 确保条目数量不少于低限制
		numentries = low_limit;
	if (numentries > max)// 确保条目数量不超过最大限制
		numentries = max;

	log2qty = ilog2(numentries);// 计算条目数量的对数值

	gfp_flags = (flags & HASH_ZERO) ? GFP_ATOMIC | __GFP_ZERO : GFP_ATOMIC;// 设置 GFP 标志，根据是否需要零初始化
	do {
		virt = false;//初始化为非虚拟内存分配
		size = bucketsize << log2qty;// 计算哈希表的总大小
		if (flags & HASH_EARLY) {//如果设置了 HASH_EARLY 标志
			if (flags & HASH_ZERO)//根据 HASH_ZERO 标志选择内存分配方式
				table = memblock_alloc(size, SMP_CACHE_BYTES);//分配并初始化内存块
			else
				table = memblock_alloc_raw(size,
							   SMP_CACHE_BYTES);//仅分配内存块，不初始化
		} else if (get_order(size) > MAX_PAGE_ORDER || hashdist) {//如果大小超过最大页面阶数或启用了哈希分布
			table = vmalloc_huge(size, gfp_flags);//使用虚拟内存分配大页
			virt = true;//标记为虚拟分配
			if (table)
				huge = is_vm_area_hugepages(table);//检查是否为大页分配
		} else {
			/*
			 * If bucketsize is not a power-of-two, we may free
			 * some pages at the end of hash table which
			 * alloc_pages_exact() automatically does
			 */
			table = alloc_pages_exact(size, gfp_flags);//精确分配页面大小
			kmemleak_alloc(table, size, 1, gfp_flags);// 记录内存泄漏信息
		}
	} while (!table && size > PAGE_SIZE && --log2qty);//如果分配失败且大小大于一页，则减小 log2qty 重试

	if (!table)//如果最终分配仍失败
		panic("Failed to allocate %s hash table\n", tablename);//触发内核 panic 错误

	pr_info("%s hash table entries: %ld (order: %d, %lu bytes, %s)\n",
		tablename, 1UL << log2qty, ilog2(size) - PAGE_SHIFT, size,
		virt ? (huge ? "vmalloc hugepage" : "vmalloc") : "linear");//打印哈希表的分配信息，包括条目数、内存分配方式等

	if (_hash_shift)//如果传入了 hash_shift 指针
		*_hash_shift = log2qty;// 设置为条目数量的对数值
	if (_hash_mask)//如果传入了 hash_mask 指针
		*_hash_mask = (1 << log2qty) - 1;//设置为条目数量掩码

	return table;//返回分配的哈希表指针
}

void __init memblock_free_pages(struct page *page, unsigned long pfn,
							unsigned int order)
{
	if (IS_ENABLED(CONFIG_DEFERRED_STRUCT_PAGE_INIT)) {
		int nid = early_pfn_to_nid(pfn);

		if (!early_page_initialised(pfn, nid))
			return;
	}

	if (!kmsan_memblock_free_pages(page, order)) {
		/* KMSAN will take care of these pages. */
		return;
	}

	/* pages were reserved and not allocated */
	if (mem_alloc_profiling_enabled()) {
		union codetag_ref *ref = get_page_tag_ref(page);

		if (ref) {
			set_codetag_empty(ref);
			put_page_tag_ref(ref);
		}
	}

	__free_pages_core(page, order);
}

DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_ALLOC_DEFAULT_ON, init_on_alloc);
EXPORT_SYMBOL(init_on_alloc);

DEFINE_STATIC_KEY_MAYBE(CONFIG_INIT_ON_FREE_DEFAULT_ON, init_on_free);
EXPORT_SYMBOL(init_on_free);

static bool _init_on_alloc_enabled_early __read_mostly
				= IS_ENABLED(CONFIG_INIT_ON_ALLOC_DEFAULT_ON);
static int __init early_init_on_alloc(char *buf)
{

	return kstrtobool(buf, &_init_on_alloc_enabled_early);
}
early_param("init_on_alloc", early_init_on_alloc);

static bool _init_on_free_enabled_early __read_mostly
				= IS_ENABLED(CONFIG_INIT_ON_FREE_DEFAULT_ON);
static int __init early_init_on_free(char *buf)
{
	return kstrtobool(buf, &_init_on_free_enabled_early);
}
early_param("init_on_free", early_init_on_free);

DEFINE_STATIC_KEY_MAYBE(CONFIG_DEBUG_VM, check_pages_enabled);

/*
 * Enable static keys related to various memory debugging and hardening options.
 * Some override others, and depend on early params that are evaluated in the
 * order of appearance. So we need to first gather the full picture of what was
 * enabled, and then make decisions.
 */
static void __init mem_debugging_and_hardening_init(void)
{
	bool page_poisoning_requested = false;
	bool want_check_pages = false;

#ifdef CONFIG_PAGE_POISONING
	/*
	 * Page poisoning is debug page alloc for some arches. If
	 * either of those options are enabled, enable poisoning.
	 */
	if (page_poisoning_enabled() ||
	     (!IS_ENABLED(CONFIG_ARCH_SUPPORTS_DEBUG_PAGEALLOC) &&
	      debug_pagealloc_enabled())) {
		static_branch_enable(&_page_poisoning_enabled);
		page_poisoning_requested = true;
		want_check_pages = true;
	}
#endif

	if ((_init_on_alloc_enabled_early || _init_on_free_enabled_early) &&
	    page_poisoning_requested) {
		pr_info("mem auto-init: CONFIG_PAGE_POISONING is on, "
			"will take precedence over init_on_alloc and init_on_free\n");
		_init_on_alloc_enabled_early = false;
		_init_on_free_enabled_early = false;
	}

	if (_init_on_alloc_enabled_early) {
		want_check_pages = true;
		static_branch_enable(&init_on_alloc);
	} else {
		static_branch_disable(&init_on_alloc);
	}

	if (_init_on_free_enabled_early) {
		want_check_pages = true;
		static_branch_enable(&init_on_free);
	} else {
		static_branch_disable(&init_on_free);
	}

	if (IS_ENABLED(CONFIG_KMSAN) &&
	    (_init_on_alloc_enabled_early || _init_on_free_enabled_early))
		pr_info("mem auto-init: please make sure init_on_alloc and init_on_free are disabled when running KMSAN\n");

#ifdef CONFIG_DEBUG_PAGEALLOC
	if (debug_pagealloc_enabled()) {
		want_check_pages = true;
		static_branch_enable(&_debug_pagealloc_enabled);

		if (debug_guardpage_minorder())
			static_branch_enable(&_debug_guardpage_enabled);
	}
#endif

	/*
	 * Any page debugging or hardening option also enables sanity checking
	 * of struct pages being allocated or freed. With CONFIG_DEBUG_VM it's
	 * enabled already.
	 */
	if (!IS_ENABLED(CONFIG_DEBUG_VM) && want_check_pages)
		static_branch_enable(&check_pages_enabled);
}

/* Report memory auto-initialization states for this boot. */
static void __init report_meminit(void)
{
	const char *stack;

	if (IS_ENABLED(CONFIG_INIT_STACK_ALL_PATTERN))
		stack = "all(pattern)";
	else if (IS_ENABLED(CONFIG_INIT_STACK_ALL_ZERO))
		stack = "all(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_BYREF_ALL))
		stack = "byref_all(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_BYREF))
		stack = "byref(zero)";
	else if (IS_ENABLED(CONFIG_GCC_PLUGIN_STRUCTLEAK_USER))
		stack = "__user(zero)";
	else
		stack = "off";

	pr_info("mem auto-init: stack:%s, heap alloc:%s, heap free:%s\n",
		stack, want_init_on_alloc(GFP_KERNEL) ? "on" : "off",
		want_init_on_free() ? "on" : "off");
	if (want_init_on_free())
		pr_info("mem auto-init: clearing system memory may take some time...\n");
}

static void __init mem_init_print_info(void)
{
	unsigned long physpages, codesize, datasize, rosize, bss_size;
	unsigned long init_code_size, init_data_size;

	physpages = get_num_physpages();
	codesize = _etext - _stext;
	datasize = _edata - _sdata;
	rosize = __end_rodata - __start_rodata;
	bss_size = __bss_stop - __bss_start;
	init_data_size = __init_end - __init_begin;
	init_code_size = _einittext - _sinittext;

	/*
	 * Detect special cases and adjust section sizes accordingly:
	 * 1) .init.* may be embedded into .data sections
	 * 2) .init.text.* may be out of [__init_begin, __init_end],
	 *    please refer to arch/tile/kernel/vmlinux.lds.S.
	 * 3) .rodata.* may be embedded into .text or .data sections.
	 */
#define adj_init_size(start, end, size, pos, adj) \
	do { \
		if (&start[0] <= &pos[0] && &pos[0] < &end[0] && size > adj) \
			size -= adj; \
	} while (0)

	adj_init_size(__init_begin, __init_end, init_data_size,
		     _sinittext, init_code_size);
	adj_init_size(_stext, _etext, codesize, _sinittext, init_code_size);
	adj_init_size(_sdata, _edata, datasize, __init_begin, init_data_size);
	adj_init_size(_stext, _etext, codesize, __start_rodata, rosize);
	adj_init_size(_sdata, _edata, datasize, __start_rodata, rosize);

#undef	adj_init_size

	pr_info("Memory: %luK/%luK available (%luK kernel code, %luK rwdata, %luK rodata, %luK init, %luK bss, %luK reserved, %luK cma-reserved"
#ifdef	CONFIG_HIGHMEM
		", %luK highmem"
#endif
		")\n",
		K(nr_free_pages()), K(physpages),
		codesize / SZ_1K, datasize / SZ_1K, rosize / SZ_1K,
		(init_data_size + init_code_size) / SZ_1K, bss_size / SZ_1K,
		K(physpages - totalram_pages() - totalcma_pages),
		K(totalcma_pages)
#ifdef	CONFIG_HIGHMEM
		, K(totalhigh_pages())
#endif
		);
}

/*
 * Set up kernel memory allocators
 */
void __init mm_core_init(void)
{
	/* 基于 SMP（对称多处理器）的初始化 */
	build_all_zonelists(NULL);//构建所有内存区域（zone）的列表，用于内存分配策略
	page_alloc_init_cpuhp();//初始化页面分配器中的 CPU 热插拔支持

	/*
	 * page_ext requires contiguous pages,
	 * bigger than MAX_PAGE_ORDER unless SPARSEMEM.
	 */
	page_ext_init_flatmem();//初始化平面内存模型（Flat Memory Model）的页面扩展（page_ext）
	mem_debugging_and_hardening_init();//初始化内存调试和加固
	kfence_alloc_pool_and_metadata();// 分配 KFENCE（Kernel Electric Fence）的内存池和元数据
	report_meminit();//报告内存初始化信息
	kmsan_init_shadow();//初始化 KMSAN（Kernel Memory Sanitizer）的影子内存
	stack_depot_early_init();//早期初始化栈存储（stack depot）
	mem_init();//初始化内存管理子系统(处理设备无法访问4G以上空间的问题)
	mem_init_print_info();//打印内存初始化信息
	kmem_cache_init();//初始化内存缓存（kmem cache）
	/*
	 * page_owner must be initialized after buddy is ready, and also after
	 * slab is ready so that stack_depot_init() works properly
	 */
	page_ext_init_flatmem_late();//晚期初始化平面内存模型的页面扩展（page_ext）
	kmemleak_init();//初始化 kmemleak（内存泄漏检测器）
	ptlock_cache_init();//初始化页表锁缓存
	pgtable_cache_init();//初始化页表缓存
	debug_objects_mem_init();//初始化调试对象的内存
	vmalloc_init();//初始化虚拟内存分配器（vmalloc）
	/* 如果没有延迟初始化页面扩展，现在初始化它，因为 vmap 已完全初始化 */
	if (!deferred_struct_pages)
		page_ext_init();
	/* 应在创建第一个非初始化线程之前运行 */
	init_espfix_bsp();
	/* 应在 espfix64 设置之后运行 */
	pti_init();
	kmsan_init_runtime();//初始化 KMSAN（Kernel Memory Sanitizer）运行时
	mm_cache_init();//初始化内存缓存
	execmem_init();//初始化可执行内存
}
