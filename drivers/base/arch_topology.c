// SPDX-License-Identifier: GPL-2.0
/*
 * Arch specific cpu topology information
 *
 * Copyright (C) 2016, ARM Ltd.
 * Written by: Juri Lelli, ARM Ltd.
 */

#include <linux/acpi.h>
#include <linux/cacheinfo.h>
#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/device.h>
#include <linux/of.h>
#include <linux/slab.h>
#include <linux/sched/topology.h>
#include <linux/cpuset.h>
#include <linux/cpumask.h>
#include <linux/init.h>
#include <linux/rcupdate.h>
#include <linux/sched.h>
#include <linux/units.h>

#define CREATE_TRACE_POINTS
#include <trace/events/hw_pressure.h>

static DEFINE_PER_CPU(struct scale_freq_data __rcu *, sft_data);
static struct cpumask scale_freq_counters_mask;
static bool scale_freq_invariant;
DEFINE_PER_CPU(unsigned long, capacity_freq_ref) = 1;
EXPORT_PER_CPU_SYMBOL_GPL(capacity_freq_ref);

static bool supports_scale_freq_counters(const struct cpumask *cpus)
{
	return cpumask_subset(cpus, &scale_freq_counters_mask);
}

bool topology_scale_freq_invariant(void)
{
	return cpufreq_supports_freq_invariance() ||
	       supports_scale_freq_counters(cpu_online_mask);
}

static void update_scale_freq_invariant(bool status)
{
	if (scale_freq_invariant == status)
		return;

	/*
	 * Task scheduler behavior depends on frequency invariance support,
	 * either cpufreq or counter driven. If the support status changes as
	 * a result of counter initialisation and use, retrigger the build of
	 * scheduling domains to ensure the information is propagated properly.
	 */
	if (topology_scale_freq_invariant() == status) {
		scale_freq_invariant = status;
		rebuild_sched_domains_energy();
	}
}

void topology_set_scale_freq_source(struct scale_freq_data *data,
				    const struct cpumask *cpus)
{
	struct scale_freq_data *sfd;
	int cpu;

	/*
	 * Avoid calling rebuild_sched_domains() unnecessarily if FIE is
	 * supported by cpufreq.
	 */
	if (cpumask_empty(&scale_freq_counters_mask))
		scale_freq_invariant = topology_scale_freq_invariant();

	rcu_read_lock();

	for_each_cpu(cpu, cpus) {
		sfd = rcu_dereference(*per_cpu_ptr(&sft_data, cpu));

		/* Use ARCH provided counters whenever possible */
		if (!sfd || sfd->source != SCALE_FREQ_SOURCE_ARCH) {
			rcu_assign_pointer(per_cpu(sft_data, cpu), data);
			cpumask_set_cpu(cpu, &scale_freq_counters_mask);
		}
	}

	rcu_read_unlock();

	update_scale_freq_invariant(true);
}
EXPORT_SYMBOL_GPL(topology_set_scale_freq_source);

void topology_clear_scale_freq_source(enum scale_freq_source source,
				      const struct cpumask *cpus)
{
	struct scale_freq_data *sfd;
	int cpu;

	rcu_read_lock();

	for_each_cpu(cpu, cpus) {
		sfd = rcu_dereference(*per_cpu_ptr(&sft_data, cpu));

		if (sfd && sfd->source == source) {
			rcu_assign_pointer(per_cpu(sft_data, cpu), NULL);
			cpumask_clear_cpu(cpu, &scale_freq_counters_mask);
		}
	}

	rcu_read_unlock();

	/*
	 * Make sure all references to previous sft_data are dropped to avoid
	 * use-after-free races.
	 */
	synchronize_rcu();

	update_scale_freq_invariant(false);
}
EXPORT_SYMBOL_GPL(topology_clear_scale_freq_source);

void topology_scale_freq_tick(void)
{
	struct scale_freq_data *sfd = rcu_dereference_sched(*this_cpu_ptr(&sft_data));

	if (sfd)
		sfd->set_freq_scale();
}

DEFINE_PER_CPU(unsigned long, arch_freq_scale) = SCHED_CAPACITY_SCALE;
EXPORT_PER_CPU_SYMBOL_GPL(arch_freq_scale);

void topology_set_freq_scale(const struct cpumask *cpus, unsigned long cur_freq,
			     unsigned long max_freq)
{
	unsigned long scale;
	int i;

	if (WARN_ON_ONCE(!cur_freq || !max_freq))
		return;

	/*
	 * If the use of counters for FIE is enabled, just return as we don't
	 * want to update the scale factor with information from CPUFREQ.
	 * Instead the scale factor will be updated from arch_scale_freq_tick.
	 */
	if (supports_scale_freq_counters(cpus))
		return;

	scale = (cur_freq << SCHED_CAPACITY_SHIFT) / max_freq;

	for_each_cpu(i, cpus)
		per_cpu(arch_freq_scale, i) = scale;
}

DEFINE_PER_CPU(unsigned long, cpu_scale) = SCHED_CAPACITY_SCALE;
EXPORT_PER_CPU_SYMBOL_GPL(cpu_scale);

void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity)
{
	per_cpu(cpu_scale, cpu) = capacity;
}

DEFINE_PER_CPU(unsigned long, hw_pressure);

/**
 * topology_update_hw_pressure() - Update HW pressure for CPUs
 * @cpus        : The related CPUs for which capacity has been reduced
 * @capped_freq : The maximum allowed frequency that CPUs can run at
 *
 * Update the value of HW pressure for all @cpus in the mask. The
 * cpumask should include all (online+offline) affected CPUs, to avoid
 * operating on stale data when hot-plug is used for some CPUs. The
 * @capped_freq reflects the currently allowed max CPUs frequency due to
 * HW capping. It might be also a boost frequency value, which is bigger
 * than the internal 'capacity_freq_ref' max frequency. In such case the
 * pressure value should simply be removed, since this is an indication that
 * there is no HW throttling. The @capped_freq must be provided in kHz.
 */
void topology_update_hw_pressure(const struct cpumask *cpus,
				      unsigned long capped_freq)
{
	unsigned long max_capacity, capacity, pressure;
	u32 max_freq;
	int cpu;

	cpu = cpumask_first(cpus);
	max_capacity = arch_scale_cpu_capacity(cpu);
	max_freq = arch_scale_freq_ref(cpu);

	/*
	 * Handle properly the boost frequencies, which should simply clean
	 * the HW pressure value.
	 */
	if (max_freq <= capped_freq)
		capacity = max_capacity;
	else
		capacity = mult_frac(max_capacity, capped_freq, max_freq);

	pressure = max_capacity - capacity;

	trace_hw_pressure_update(cpu, pressure);

	for_each_cpu(cpu, cpus)
		WRITE_ONCE(per_cpu(hw_pressure, cpu), pressure);
}
EXPORT_SYMBOL_GPL(topology_update_hw_pressure);

static ssize_t cpu_capacity_show(struct device *dev,
				 struct device_attribute *attr,
				 char *buf)
{
	struct cpu *cpu = container_of(dev, struct cpu, dev);

	return sysfs_emit(buf, "%lu\n", topology_get_cpu_scale(cpu->dev.id));
}

static void update_topology_flags_workfn(struct work_struct *work);
static DECLARE_WORK(update_topology_flags_work, update_topology_flags_workfn);

static DEVICE_ATTR_RO(cpu_capacity);

static int cpu_capacity_sysctl_add(unsigned int cpu)
{
	struct device *cpu_dev = get_cpu_device(cpu);

	if (!cpu_dev)
		return -ENOENT;

	device_create_file(cpu_dev, &dev_attr_cpu_capacity);

	return 0;
}

static int cpu_capacity_sysctl_remove(unsigned int cpu)
{
	struct device *cpu_dev = get_cpu_device(cpu);

	if (!cpu_dev)
		return -ENOENT;

	device_remove_file(cpu_dev, &dev_attr_cpu_capacity);

	return 0;
}

static int register_cpu_capacity_sysctl(void)
{
	cpuhp_setup_state(CPUHP_AP_ONLINE_DYN, "topology/cpu-capacity",
			  cpu_capacity_sysctl_add, cpu_capacity_sysctl_remove);

	return 0;
}
subsys_initcall(register_cpu_capacity_sysctl);

static int update_topology;

int topology_update_cpu_topology(void)
{
	return update_topology;
}

/*
 * Updating the sched_domains can't be done directly from cpufreq callbacks
 * due to locking, so queue the work for later.
 */
static void update_topology_flags_workfn(struct work_struct *work)
{
	update_topology = 1;
	rebuild_sched_domains();
	pr_debug("sched_domain hierarchy rebuilt, flags updated\n");
	update_topology = 0;
}

static u32 *raw_capacity;

static int free_raw_capacity(void)
{
	kfree(raw_capacity);
	raw_capacity = NULL;

	return 0;
}

void topology_normalize_cpu_scale(void)
{
	u64 capacity;//当前 CPU 的容量值
	u64 capacity_scale;//用于存储所有 CPU 的最大容量值
	int cpu;//CPU 索引

	if (!raw_capacity)//如果未定义 `raw_capacity`，直接返回
		return;

	capacity_scale = 1;//初始化容量比例为 1
	for_each_possible_cpu(cpu) {
		capacity = raw_capacity[cpu] * per_cpu(capacity_freq_ref, cpu);//计算当前 CPU 的原始容量值
		capacity_scale = max(capacity, capacity_scale);//更新最大容量值
	}

	pr_debug("cpu_capacity: capacity_scale=%llu\n", capacity_scale);
	for_each_possible_cpu(cpu) {//再次遍历每个可能的 CPU
		capacity = raw_capacity[cpu] * per_cpu(capacity_freq_ref, cpu);//重新计算当前 CPU 的容量值
		capacity = div64_u64(capacity << SCHED_CAPACITY_SHIFT,
			capacity_scale);//根据最大容量值进行归一化
		topology_set_cpu_scale(cpu, capacity);//将归一化容量值设置到拓扑结构中
		pr_debug("cpu_capacity: CPU%d cpu_capacity=%lu\n",
			cpu, topology_get_cpu_scale(cpu));
	}
}

bool __init topology_parse_cpu_capacity(struct device_node *cpu_node, int cpu)
{
	struct clk *cpu_clk;
	static bool cap_parsing_failed;
	int ret;
	u32 cpu_capacity;

	if (cap_parsing_failed)
		return false;

	ret = of_property_read_u32(cpu_node, "capacity-dmips-mhz",
				   &cpu_capacity);
	if (!ret) {
		if (!raw_capacity) {
			raw_capacity = kcalloc(num_possible_cpus(),
					       sizeof(*raw_capacity),
					       GFP_KERNEL);
			if (!raw_capacity) {
				cap_parsing_failed = true;
				return false;
			}
		}
		raw_capacity[cpu] = cpu_capacity;
		pr_debug("cpu_capacity: %pOF cpu_capacity=%u (raw)\n",
			cpu_node, raw_capacity[cpu]);

		/*
		 * Update capacity_freq_ref for calculating early boot CPU capacities.
		 * For non-clk CPU DVFS mechanism, there's no way to get the
		 * frequency value now, assuming they are running at the same
		 * frequency (by keeping the initial capacity_freq_ref value).
		 */
		cpu_clk = of_clk_get(cpu_node, 0);
		if (!PTR_ERR_OR_ZERO(cpu_clk)) {
			per_cpu(capacity_freq_ref, cpu) =
				clk_get_rate(cpu_clk) / HZ_PER_KHZ;
			clk_put(cpu_clk);
		}
	} else {
		if (raw_capacity) {
			pr_err("cpu_capacity: missing %pOF raw capacity\n",
				cpu_node);
			pr_err("cpu_capacity: partial information: fallback to 1024 for all CPUs\n");
		}
		cap_parsing_failed = true;
		free_raw_capacity();
	}

	return !ret;
}

void __weak freq_inv_set_max_ratio(int cpu, u64 max_rate)
{
}

#ifdef CONFIG_ACPI_CPPC_LIB
#include <acpi/cppc_acpi.h>

void topology_init_cpu_capacity_cppc(void)
{
	u64 capacity, capacity_scale = 0;
	struct cppc_perf_caps perf_caps;
	int cpu;

	if (likely(!acpi_cpc_valid()))
		return;

	raw_capacity = kcalloc(num_possible_cpus(), sizeof(*raw_capacity),
			       GFP_KERNEL);
	if (!raw_capacity)
		return;

	for_each_possible_cpu(cpu) {
		if (!cppc_get_perf_caps(cpu, &perf_caps) &&
		    (perf_caps.highest_perf >= perf_caps.nominal_perf) &&
		    (perf_caps.highest_perf >= perf_caps.lowest_perf)) {
			raw_capacity[cpu] = perf_caps.highest_perf;
			capacity_scale = max_t(u64, capacity_scale, raw_capacity[cpu]);

			per_cpu(capacity_freq_ref, cpu) = cppc_perf_to_khz(&perf_caps, raw_capacity[cpu]);

			pr_debug("cpu_capacity: CPU%d cpu_capacity=%u (raw).\n",
				 cpu, raw_capacity[cpu]);
			continue;
		}

		pr_err("cpu_capacity: CPU%d missing/invalid highest performance.\n", cpu);
		pr_err("cpu_capacity: partial information: fallback to 1024 for all CPUs\n");
		goto exit;
	}

	for_each_possible_cpu(cpu) {
		freq_inv_set_max_ratio(cpu,
				       per_cpu(capacity_freq_ref, cpu) * HZ_PER_KHZ);

		capacity = raw_capacity[cpu];
		capacity = div64_u64(capacity << SCHED_CAPACITY_SHIFT,
				     capacity_scale);
		topology_set_cpu_scale(cpu, capacity);
		pr_debug("cpu_capacity: CPU%d cpu_capacity=%lu\n",
			cpu, topology_get_cpu_scale(cpu));
	}

	schedule_work(&update_topology_flags_work);
	pr_debug("cpu_capacity: cpu_capacity initialization done\n");

exit:
	free_raw_capacity();
}
#endif

#ifdef CONFIG_CPU_FREQ
static cpumask_var_t cpus_to_visit;
static void parsing_done_workfn(struct work_struct *work);
static DECLARE_WORK(parsing_done_work, parsing_done_workfn);

static int
init_cpu_capacity_callback(struct notifier_block *nb,
			   unsigned long val,
			   void *data)
{
	struct cpufreq_policy *policy = data;
	int cpu;

	if (val != CPUFREQ_CREATE_POLICY)
		return 0;

	pr_debug("cpu_capacity: init cpu capacity for CPUs [%*pbl] (to_visit=%*pbl)\n",
		 cpumask_pr_args(policy->related_cpus),
		 cpumask_pr_args(cpus_to_visit));

	cpumask_andnot(cpus_to_visit, cpus_to_visit, policy->related_cpus);

	for_each_cpu(cpu, policy->related_cpus) {
		per_cpu(capacity_freq_ref, cpu) = policy->cpuinfo.max_freq;
		freq_inv_set_max_ratio(cpu,
				       per_cpu(capacity_freq_ref, cpu) * HZ_PER_KHZ);
	}

	if (cpumask_empty(cpus_to_visit)) {
		if (raw_capacity) {
			topology_normalize_cpu_scale();
			schedule_work(&update_topology_flags_work);
			free_raw_capacity();
		}
		pr_debug("cpu_capacity: parsing done\n");
		schedule_work(&parsing_done_work);
	}

	return 0;
}

static struct notifier_block init_cpu_capacity_notifier = {
	.notifier_call = init_cpu_capacity_callback,
};

static int __init register_cpufreq_notifier(void)
{
	int ret;

	/*
	 * On ACPI-based systems skip registering cpufreq notifier as cpufreq
	 * information is not needed for cpu capacity initialization.
	 */
	if (!acpi_disabled)
		return -EINVAL;

	if (!alloc_cpumask_var(&cpus_to_visit, GFP_KERNEL))
		return -ENOMEM;

	cpumask_copy(cpus_to_visit, cpu_possible_mask);

	ret = cpufreq_register_notifier(&init_cpu_capacity_notifier,
					CPUFREQ_POLICY_NOTIFIER);

	if (ret)
		free_cpumask_var(cpus_to_visit);

	return ret;
}
core_initcall(register_cpufreq_notifier);

static void parsing_done_workfn(struct work_struct *work)
{
	cpufreq_unregister_notifier(&init_cpu_capacity_notifier,
					 CPUFREQ_POLICY_NOTIFIER);
	free_cpumask_var(cpus_to_visit);
}

#else
core_initcall(free_raw_capacity);
#endif

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
/*
 * This function returns the logic cpu number of the node.
 * There are basically three kinds of return values:
 * (1) logic cpu number which is > 0.
 * (2) -ENODEV when the device tree(DT) node is valid and found in the DT but
 * there is no possible logical CPU in the kernel to match. This happens
 * when CONFIG_NR_CPUS is configure to be smaller than the number of
 * CPU nodes in DT. We need to just ignore this case.
 * (3) -1 if the node does not exist in the device tree
 */
static int __init get_cpu_for_node(struct device_node *node)
{
	struct device_node *cpu_node;
	int cpu;

	cpu_node = of_parse_phandle(node, "cpu", 0);
	if (!cpu_node)
		return -1;

	cpu = of_cpu_node_to_id(cpu_node);
	if (cpu >= 0)
		topology_parse_cpu_capacity(cpu_node, cpu);
	else
		pr_info("CPU node for %pOF exist but the possible cpu range is :%*pbl\n",
			cpu_node, cpumask_pr_args(cpu_possible_mask));

	of_node_put(cpu_node);
	return cpu;
}

static int __init parse_core(struct device_node *core, int package_id,
			     int cluster_id, int core_id)
{
	char name[20];
	bool leaf = true;
	int i = 0;
	int cpu;
	struct device_node *t;

	do {
		snprintf(name, sizeof(name), "thread%d", i);//生成 thread 名称
		t = of_get_child_by_name(core, name);//获取名称为 `name` 的 thread 子节点
		if (t) {
			leaf = false;//如果找到子线程，设置为非叶子核心
			cpu = get_cpu_for_node(t);//获取与线程关联的 CPU ID
			if (cpu >= 0) {
				cpu_topology[cpu].package_id = package_id;//设置 CPU 的 package ID
				cpu_topology[cpu].cluster_id = cluster_id;//设置 CPU 的 cluster ID
				cpu_topology[cpu].core_id = core_id;//设置 CPU 的核心 ID
				cpu_topology[cpu].thread_id = i;//设置 CPU 的线程 ID
			} else if (cpu != -ENODEV) {
				pr_err("%pOF: Can't get CPU for thread\n", t);//输出错误信息，无法为线程获取 CPU
				of_node_put(t);//释放设备节点引用
				return -EINVAL;//返回无效参数错误
			}
			of_node_put(t);
		}
		i++;//检查下一个线程
	} while (t);

	cpu = get_cpu_for_node(core);//获取与核心关联的 CPU ID
	if (cpu >= 0) {
		if (!leaf) {
			pr_err("%pOF: Core has both threads and CPU\n",
			       core);//不是叶子节点，输出错误信息，核心同时包含线程和 CPU
			return -EINVAL;//返回无效参数错误
		}

		cpu_topology[cpu].package_id = package_id;//设置 CPU 的 package ID
		cpu_topology[cpu].cluster_id = cluster_id;//设置 CPU 的 cluster ID
		cpu_topology[cpu].core_id = core_id;//设置 CPU 的核心 ID
	} else if (leaf && cpu != -ENODEV) {
		pr_err("%pOF: Can't get CPU for leaf core\n", core);//输出错误信息，无法为叶子核心获取 CPU
		return -EINVAL;//返回无效参数错误
	}

	return 0;
}

static int __init parse_cluster(struct device_node *cluster, int package_id,
				int cluster_id, int depth)
{
	char name[20];
	bool leaf = true;
	bool has_cores = false;
	struct device_node *c;
	int core_id = 0;
	int i, ret;

	/*
	 * First check for child clusters; we currently ignore any
	 * information about the nesting of clusters and present the
	 * scheduler with a flat list of them.
	 * 首先检查子 cluster；当前实现忽略 cluster 的嵌套结构，
	 * 并向调度器提供一个平面列表.
	 */
	i = 0;
	do {
		snprintf(name, sizeof(name), "cluster%d", i);//生成 cluster 名称
		c = of_get_child_by_name(cluster, name);//获取名称为 `name` 的cluster
		if (c) {
			leaf = false;//如果找到cluster，设置为非叶子节点
			ret = parse_cluster(c, package_id, i, depth + 1);// 递归解析cluster
			if (depth > 0)
				pr_warn("Topology for clusters of clusters not yet supported\n");//发出警告，嵌套 cluster 不受支持
			of_node_put(c);//释放设备节点引用
			if (ret != 0)
				return ret;
		}
		i++;
	} while (c);

	/* 现在检查核心 */
	i = 0;
	do {
		snprintf(name, sizeof(name), "core%d", i);//生成核心名称
		c = of_get_child_by_name(cluster, name);//获取名称为 `name` 的核心节点
		if (c) {
			has_cores = true;//如果找到核心节点，设置标志位

			if (depth == 0) {
				pr_err("%pOF: cpu-map children should be clusters\n",
				       c);// 输出错误信息，cpu-map 的子节点应为 cluster
				of_node_put(c);//释放设备节点引用
				return -EINVAL;//返回无效参数错误
			}

			if (leaf) {
				ret = parse_core(c, package_id, cluster_id,
						 core_id++);//解析核心节点信息
			} else {
				pr_err("%pOF: Non-leaf cluster with core %s\n",
				       cluster, name);//输出错误信息，非叶子 cluster 不应包含核心
				ret = -EINVAL;//返回无效参数错误
			}

			of_node_put(c);// 释放设备节点引用
			if (ret != 0)
				return ret;//解析失败,返回错误代码
		}
		i++;//检查下一个核心节点
	} while (c);

	if (leaf && !has_cores)//如果是叶子 cluster 且没有核心
		pr_warn("%pOF: empty cluster\n", cluster);// 发出警告，表示 cluster 为空

	return 0;
}
/*用于解析与指定设备节点（socket）相关的 cluster 信息。*/
static int __init parse_socket(struct device_node *socket)
{
	char name[20];
	struct device_node *c;
	bool has_socket = false;
	int package_id = 0, ret;

	do {
		snprintf(name, sizeof(name), "socket%d", package_id);//生成当前 socket 的名称
		c = of_get_child_by_name(socket, name);//获取名称为 `name` 的子节点
		if (c) {
			has_socket = true;//如果找到了 socket 子节点，设置标志位
			ret = parse_cluster(c, package_id, -1, 0);//解析该 socket 的 cluster 信息
			of_node_put(c);//释放设备节点引用
			if (ret != 0)//如果解析失败
				return ret;//返回错误代码
		}
		package_id++;//增加包 ID，检查下一个可能的 socket
	} while (c);// 如果没有找到子节点，退出循环

	if (!has_socket)//如果未找到任何 socket 子节点
		ret = parse_cluster(socket, 0, -1, 0);//将当前节点作为默认 socket 进行解析

	return ret;
}
/* 解析设备树 (Device Tree) 中的 CPU 拓扑结构。*/
static int __init parse_dt_topology(void)
{
	struct device_node *cn, *map;
	int ret = 0;
	int cpu;

	cn = of_find_node_by_path("/cpus");//查找设备树中路径为 "/cpus" 的节点，表示包含所有 CPU 的根节点
	if (!cn) {
		pr_err("No CPU information found in DT\n");//未找到节点，打印错误信息
		return 0;
	}

	/*
	 * When topology is provided cpu-map is essentially a root
	 * cluster with restricted subnodes.
	 * 当拓扑结构被提供时，"cpu-map" 通常是一个根簇 (cluster) 节点，其下包含受限的子节点。
	 */
	map = of_get_child_by_name(cn, "cpu-map");/ 获取 "/cpus" 节点下名为 "cpu-map" 的子节点
	if (!map)
		goto out;//若未找到 "cpu-map"，则跳转到 out 标签执行后续操作

	ret = parse_socket(map);//解析 socket 信息，map 节点代表 CPU socket 信息
	if (ret != 0)
		goto out_map;//若解析失败，跳转到 out_map 标签释放资源

	topology_normalize_cpu_scale();//归一化 CPU 拓扑结构中的每个CPU的性能值

	/*
	 * Check that all cores are in the topology; the SMP code will
	 * only mark cores described in the DT as possible.
	 * 检查所有核心是否都在拓扑结构中；SMP 代码仅会标记设备树中描述的核心为可能存在。
	 */
	for_each_possible_cpu(cpu)
		if (cpu_topology[cpu].package_id < 0) {//如果某个CPU的package ID为 -1，说明没有被正确初始化
			ret = -EINVAL;//返回无效参数错误
			break;
		}

out_map:
	of_node_put(map);//释放对 "cpu-map" 节点的引用
out:
	of_node_put(cn);//释放对 "/cpus" 节点的引用
	return ret;
}
#endif

/*
 * cpu topology table
 */
struct cpu_topology cpu_topology[NR_CPUS];
EXPORT_SYMBOL_GPL(cpu_topology);

const struct cpumask *cpu_coregroup_mask(int cpu)
{
	const cpumask_t *core_mask = cpumask_of_node(cpu_to_node(cpu));

	/* Find the smaller of NUMA, core or LLC siblings */
	if (cpumask_subset(&cpu_topology[cpu].core_sibling, core_mask)) {
		/* not numa in package, lets use the package siblings */
		core_mask = &cpu_topology[cpu].core_sibling;
	}

	if (last_level_cache_is_valid(cpu)) {
		if (cpumask_subset(&cpu_topology[cpu].llc_sibling, core_mask))
			core_mask = &cpu_topology[cpu].llc_sibling;
	}

	/*
	 * For systems with no shared cpu-side LLC but with clusters defined,
	 * extend core_mask to cluster_siblings. The sched domain builder will
	 * then remove MC as redundant with CLS if SCHED_CLUSTER is enabled.
	 */
	if (IS_ENABLED(CONFIG_SCHED_CLUSTER) &&
	    cpumask_subset(core_mask, &cpu_topology[cpu].cluster_sibling))
		core_mask = &cpu_topology[cpu].cluster_sibling;

	return core_mask;
}

const struct cpumask *cpu_clustergroup_mask(int cpu)
{
	/*
	 * Forbid cpu_clustergroup_mask() to span more or the same CPUs as
	 * cpu_coregroup_mask().
	 */
	if (cpumask_subset(cpu_coregroup_mask(cpu),
			   &cpu_topology[cpu].cluster_sibling))
		return topology_sibling_cpumask(cpu);

	return &cpu_topology[cpu].cluster_sibling;
}
/* 用于更新指定 CPU 的兄弟关系掩码 */
void update_siblings_masks(unsigned int cpuid)
{
	struct cpu_topology *cpu_topo, *cpuid_topo = &cpu_topology[cpuid];//`cpuid_topo` 表示目标 CPU 的拓扑信息，`cpu_topo` 用于迭代其他 CPU
	int cpu, ret;

	ret = detect_cache_attributes(cpuid);//检测目标 CPU 的缓存属性
	if (ret && ret != -ENOENT)
		pr_info("Early cacheinfo allocation failed, ret = %d\n", ret);

	/* update core and thread sibling masks */
	for_each_online_cpu(cpu) {//遍历所有在线 CPU
		cpu_topo = &cpu_topology[cpu];//获取当前 CPU 的拓扑信息

		if (last_level_cache_is_shared(cpu, cpuid)) {//检查目标 CPU 和当前 CPU 是否共享最后一级缓存
			cpumask_set_cpu(cpu, &cpuid_topo->llc_sibling);//将当前 CPU 添加到目标 CPU 的 LLC 兄弟掩码中
			cpumask_set_cpu(cpuid, &cpu_topo->llc_sibling);// 将目标 CPU 添加到当前 CPU 的 LLC 兄弟掩码中
		}

		if (cpuid_topo->package_id != cpu_topo->package_id)//如果目标 CPU 和当前 CPU 不在同一个物理包中
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->core_sibling);//将目标 CPU 添加到当前 CPU 的核心兄弟掩码中
		cpumask_set_cpu(cpu, &cpuid_topo->core_sibling);//将当前 CPU 添加到目标 CPU 的核心兄弟掩码中

		if (cpuid_topo->cluster_id != cpu_topo->cluster_id)//如果目标 CPU 和当前 CPU 不在同一个簇中
			continue;

		if (cpuid_topo->cluster_id >= 0) {//如果目标 CPU 的簇 ID 有效
			cpumask_set_cpu(cpu, &cpuid_topo->cluster_sibling);//将当前 CPU 添加到目标 CPU 的簇兄弟掩码中
			cpumask_set_cpu(cpuid, &cpu_topo->cluster_sibling);//将目标 CPU 添加到当前 CPU 的簇兄弟掩码中
		}

		if (cpuid_topo->core_id != cpu_topo->core_id)//如果目标 CPU 和当前 CPU 不在同一个核心中
			continue;

		cpumask_set_cpu(cpuid, &cpu_topo->thread_sibling);//将目标 CPU 添加到当前 CPU 的线程兄弟掩码中
		cpumask_set_cpu(cpu, &cpuid_topo->thread_sibling);//将当前 CPU 添加到目标 CPU 的线程兄弟掩码中
	}
}

static void clear_cpu_topology(int cpu)
{
	struct cpu_topology *cpu_topo = &cpu_topology[cpu];//获取当前 CPU 的拓扑结构信息
	/*清除并重新设置当前 CPU 在 LLC (Last Level Cache) 同级掩码中的位置*/
	cpumask_clear(&cpu_topo->llc_sibling);//清除 LLC 同级掩码
	cpumask_set_cpu(cpu, &cpu_topo->llc_sibling);//将当前 CPU 添加到其 LLC 同级掩码中
	/*清除并重新设置当前 CPU 在簇 (Cluster) 同级掩码中的位置*/
	cpumask_clear(&cpu_topo->cluster_sibling);//清除簇同级掩码
	cpumask_set_cpu(cpu, &cpu_topo->cluster_sibling);//将当前 CPU 添加到其簇同级掩码中
	/*清除并重新设置当前 CPU 在核心 (Core) 同级掩码中的位置*/
	cpumask_clear(&cpu_topo->core_sibling);//清除核心同级掩码
	cpumask_set_cpu(cpu, &cpu_topo->core_sibling);//将当前 CPU 添加到其核心同级掩码中
	/*清除并重新设置当前 CPU 在线程 (Thread) 同级掩码中的位置*/
	cpumask_clear(&cpu_topo->thread_sibling);//清除线程同级掩码
	cpumask_set_cpu(cpu, &cpu_topo->thread_sibling);//将当前 CPU 添加到其线程同级掩码中
}

void __init reset_cpu_topology(void)//用于重置系统中所有可能 CPU 的拓扑信息。
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {// 遍历系统中所有可能的 CPU
		struct cpu_topology *cpu_topo = &cpu_topology[cpu];//获取当前 CPU 的拓扑结构信息
		/*重置当前 CPU 的拓扑 ID，所有 ID 初始化为 -1 表示无效状态*/
		cpu_topo->thread_id = -1;//重置线程 ID
		cpu_topo->core_id = -1;//重置核心 ID
		cpu_topo->cluster_id = -1;//重置簇 ID
		cpu_topo->package_id = -1;//重置封装 ID

		clear_cpu_topology(cpu);// 清除当前 CPU 的拓扑信息
	}
}

void remove_cpu_topology(unsigned int cpu)
{
	int sibling;

	for_each_cpu(sibling, topology_core_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_core_cpumask(sibling));
	for_each_cpu(sibling, topology_sibling_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_sibling_cpumask(sibling));
	for_each_cpu(sibling, topology_cluster_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_cluster_cpumask(sibling));
	for_each_cpu(sibling, topology_llc_cpumask(cpu))
		cpumask_clear_cpu(cpu, topology_llc_cpumask(sibling));

	clear_cpu_topology(cpu);
}

__weak int __init parse_acpi_topology(void)
{
	return 0;
}

#if defined(CONFIG_ARM64) || defined(CONFIG_RISCV)
/*用于初始化 CPU 的拓扑信息，首先尝试通过 ACPI 或设备树（DT）来解析 CPU 的拓扑，
 * 如果解析失败则重置拓扑信息，避免使用部分数据。
 * */
void __init init_cpu_topology(void)
{
	int cpu, ret;

	reset_cpu_topology();//重置当前的 CPU 拓扑信息，以确保在从头开始初始化时没有残留的状态。
	ret = parse_acpi_topology();//尝试从 ACPI（高级配置和电源接口）中解析 CPU 的拓扑结构。
	if (!ret)
		ret = of_have_populated_dt() && parse_dt_topology();//如果没有 ACPI 拓扑信息，且设备树已经填充，则尝试从设备树解析拓扑信息

	if (ret) {//如果解析失败，重置 CPU 拓扑信息，避免使用部分无效的信息
		/*
		 * Discard anything that was parsed if we hit an error so we
		 * don't use partial information. But do not return yet to give
		 * arch-specific early cache level detection a chance to run.
		 * 如果解析过程中出错，则丢弃已解析的内容，避免使用部分信息。
		 * 但不立即返回，仍然允许架构特定的早期缓存级别检测运行。
		 */
		reset_cpu_topology();
	}

	for_each_possible_cpu(cpu) {//遍历所有可能的 CPU，获取其缓存信息
		ret = fetch_cache_info(cpu);//尝试从硬件中读取 CPU 的缓存级别和大小等信息。
		if (!ret)
			continue;//如果缓存信息成功读取（ret 为 0），则继续下一个 CPU 的处理。
		else if (ret != -ENOENT)//如果缓存信息读取失败且返回的错误不是 -ENOENT（表示没有找到该缓存信息），则打印错误信息。
			pr_err("Early cacheinfo failed, ret = %d\n", ret);
		return;
	}
}

void store_cpu_topology(unsigned int cpuid)//用于存储指定 CPU 的拓扑信息
{
	struct cpu_topology *cpuid_topo = &cpu_topology[cpuid];//获取指定 CPU 的拓扑结构信息指针

	if (cpuid_topo->package_id != -1)//如果该 CPU 的 package_id 已经被设置，表示拓扑信息已经存在
		goto topology_populated;//跳转到拓扑信息更新部分

	cpuid_topo->thread_id = -1;//将线程 ID 设置为 -1，表示该 CPU 不是超线程
	cpuid_topo->core_id = cpuid;//设置核心 ID 为当前 CPU 的 ID，假设每个核心对应一个 CPU
	cpuid_topo->package_id = cpu_to_node(cpuid);//设置 package ID 为与该 CPU 相关的节点 ID

	pr_debug("CPU%u: package %d core %d thread %d\n",
		 cpuid, cpuid_topo->package_id, cpuid_topo->core_id,
		 cpuid_topo->thread_id);// 打印当前 CPU 的拓扑信息用于调试

topology_populated:
	update_siblings_masks(cpuid);//更新兄弟掩码，用于反映同一个核心或同一个套装中的 CPU
}
#endif
