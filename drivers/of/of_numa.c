// SPDX-License-Identifier: GPL-2.0
/*
 * OF NUMA Parsing support.
 *
 * Copyright (C) 2015 - 2016 Cavium Inc.
 */

#define pr_fmt(fmt) "OF: NUMA: " fmt

#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/nodemask.h>

#include <asm/numa.h>

/* define default numa node to 0 */
#define DEFAULT_NODE 0

/*
 * Even though we connect cpus to numa domains later in SMP
 * init, we need to know the node ids now for all cpus.
 * 用于从设备树中解析与 CPU 相关的 NUMA 节点信息
*/
static void __init of_numa_parse_cpu_nodes(void)
{
	u32 nid;
	int r;
	struct device_node *np;//定义一个指向设备节点结构体的指针np，用于遍历设备树中的CPU节点

	for_each_of_cpu_node(np) {//遍历设备树中的所有 CPU 节点
		r = of_property_read_u32(np, "numa-node-id", &nid);//从当前 CPU 节点的属性中读取 "numa-node-id"，并将值存储在 nid 中
		if (r)//如果读取失败，跳过此节点，继续下一个 CPU 节点
			continue;

		pr_debug("CPU on %u\n", nid);//打印调试信息，显示当前 CPU 所属的 NUMA 节点 ID
		if (nid >= MAX_NUMNODES)//检查节点 ID 是否超过了系统允许的最大节点数
			pr_warn("Node id %u exceeds maximum value\n", nid);
		else
			node_set(nid, numa_nodes_parsed);//如果节点ID合法，则将其标记为已解析
	}
}

static int __init of_numa_parse_memory_nodes(void)
{
	struct device_node *np = NULL;//定义设备节点指针，初始化为 NULL。
	struct resource rsrc;//定义一个资源结构体，用于存储内存节点的资源信息。
	u32 nid;//用于存储 NUMA 节点 ID
	int i, r;

	for_each_node_by_type(np, "memory") {//遍历设备树中所有类型为 "memory" 的节点。
		r = of_property_read_u32(np, "numa-node-id", &nid);//从当前内存节点中读取 "numa-node-id" 属性，并将其存储到nid中。
		if (r == -EINVAL)//如果返回 -EINVAL，表示属性不存在。
			/*
			 * property doesn't exist if -EINVAL, continue
			 * looking for more memory nodes with
			 * "numa-node-id" property
			 */
			continue;

		if (nid >= MAX_NUMNODES) {// 检查nid是否超出最大节点数限制。
			pr_warn("Node id %u exceeds maximum value\n", nid);
			r = -EINVAL;
		}

		for (i = 0; !r && !of_address_to_resource(np, i, &rsrc); i++)// 遍历当前节点的地址资源，将资源起始和结束地址添加到对应的NUMA节点。
			r = numa_add_memblk(nid, rsrc.start, rsrc.end + 1);//将内存块添加到 NUMA 节点。

		if (!i || r) {//检查是否未找到资源或添加内存块时出错。
			of_node_put(np);//释放节点引用。
			pr_err("bad property in memory node\n");
			return r ? : -EINVAL;// 如果 r 不为 0，返回 r，否则返回 -EINVAL
		}
	}

	return 0;// 返回 0 表示成功。
}
/* 解析 "numa-distance-map-v1" 格式的设备节点 */
static int __init of_numa_parse_distance_map_v1(struct device_node *map)
{
	const __be32 *matrix;//用于存储距离矩阵属性的数据。
	int entry_count;//用于存储距离矩阵的元素个数
	int i;

	pr_info("parsing numa-distance-map-v1\n");//打印信息，表示正在解析 "numa-distance-map-v1"

	matrix = of_get_property(map, "distance-matrix", NULL);// 获取设备节点 `map` 中的 "distance-matrix" 属性。
	if (!matrix) {
		pr_err("No distance-matrix property in distance-map\n");//如果属性不存在，则打印错误信息并返回 -EINVAL。
		return -EINVAL;
	}

	entry_count = of_property_count_u32_elems(map, "distance-matrix");//计算 "distance-matrix" 属性中的元素数量。
	if (entry_count <= 0) {//如果元素数量小于等于 0，则打印错误信息并返回 -EINVAL。
		pr_err("Invalid distance-matrix\n");
		return -EINVAL;
	}

	for (i = 0; i + 2 < entry_count; i += 3) {//循环遍历距离矩阵中的元素，每次处理 3 个元素（表示一个条目）。
		u32 nodea, nodeb, distance;//定义变量用于存储节点 A、节点 B 以及距离。
		/*读取矩阵中的节点 A、节点 B 和距离值。*/
		nodea = of_read_number(matrix, 1);
		matrix++;
		nodeb = of_read_number(matrix, 1);
		matrix++;
		distance = of_read_number(matrix, 1);
		matrix++;
		/*
		 * 检查距离值的有效性：
		 * 1. 如果两个节点相同，但距离不是 LOCAL_DISTANCE，则错误。
		 * 2. 如果两个节点不同，但距离小于等于 LOCAL_DISTANCE，则错误。
		 * */
		if ((nodea == nodeb && distance != LOCAL_DISTANCE) ||
		    (nodea != nodeb && distance <= LOCAL_DISTANCE)) {
			pr_err("Invalid distance[node%d -> node%d] = %d\n",
			       nodea, nodeb, distance);
			return -EINVAL;//如果上述条件满足，则打印错误信息并返回 -EINVAL。
		}

		node_set(nodea, numa_nodes_parsed);//将节点 A 设置为已解析的 NUMA 节点。

		numa_set_distance(nodea, nodeb, distance);//设置节点 A 到节点 B 的距离。

		/* 设置节点 B 到节点 A 的距离为与节点 A 到节点 B 相同。 */
		if (nodeb > nodea)
			numa_set_distance(nodeb, nodea, distance);
	}

	return 0;
}

static int __init of_numa_parse_distance_map(void)
{
	int ret = 0;
	struct device_node *np;//定义一个指向设备节点的指针变量np

	np = of_find_compatible_node(NULL, NULL,
				     "numa-distance-map-v1");//查找设备树中兼容 "numa-distance-map-v1" 的节点
	if (np)
		ret = of_numa_parse_distance_map_v1(np);//如果找到了匹配的节点，调用函数 of_numa_parse_distance_map_v1 解析距离映射。

	of_node_put(np);//释放对设备节点的引用，防止内存泄漏。
	return ret;
}

int of_node_to_nid(struct device_node *device)
{
	struct device_node *np;
	u32 nid;
	int r = -ENODATA;

	np = of_node_get(device);

	while (np) {
		r = of_property_read_u32(np, "numa-node-id", &nid);
		/*
		 * -EINVAL indicates the property was not found, and
		 *  we walk up the tree trying to find a parent with a
		 *  "numa-node-id".  Any other type of error indicates
		 *  a bad device tree and we give up.
		 */
		if (r != -EINVAL)
			break;

		np = of_get_next_parent(np);
	}
	if (np && r)
		pr_warn("Invalid \"numa-node-id\" property in node %pOFn\n",
			np);
	of_node_put(np);

	/*
	 * If numa=off passed on command line, or with a defective
	 * device tree, the nid may not be in the set of possible
	 * nodes.  Check for this case and return NUMA_NO_NODE.
	 */
	if (!r && nid < MAX_NUMNODES && node_possible(nid))
		return nid;

	return NUMA_NO_NODE;
}

int __init of_numa_init(void)
{
	int r;

	of_numa_parse_cpu_nodes();//解析设备树中的 CPU 节点，初始化 NUMA 中的 CPU 相关信息
	r = of_numa_parse_memory_nodes();//解析设备树中的内存节点，初始化 NUMA 中的内存区域
	if (r)// 如果解析内存节点失败，则返回错误码 r
		return r;
	return of_numa_parse_distance_map();//解析设备树中的距离映射表，初始化 NUMA 节点之间的距离
}
