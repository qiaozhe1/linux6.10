/* SPDX-License-Identifier: GPL-2.0 */
/*
 * include/linux/arch_topology.h - arch specific cpu topology information
 */
#ifndef _LINUX_ARCH_TOPOLOGY_H_
#define _LINUX_ARCH_TOPOLOGY_H_

#include <linux/types.h>
#include <linux/percpu.h>

void topology_normalize_cpu_scale(void);
int topology_update_cpu_topology(void);

#ifdef CONFIG_ACPI_CPPC_LIB
void topology_init_cpu_capacity_cppc(void);
#endif

struct device_node;
bool topology_parse_cpu_capacity(struct device_node *cpu_node, int cpu);

DECLARE_PER_CPU(unsigned long, cpu_scale);

static inline unsigned long topology_get_cpu_scale(int cpu)
{
	return per_cpu(cpu_scale, cpu);
}

void topology_set_cpu_scale(unsigned int cpu, unsigned long capacity);

DECLARE_PER_CPU(unsigned long, capacity_freq_ref);

static inline unsigned long topology_get_freq_ref(int cpu)
{
	return per_cpu(capacity_freq_ref, cpu);
}

DECLARE_PER_CPU(unsigned long, arch_freq_scale);

static inline unsigned long topology_get_freq_scale(int cpu)
{
	return per_cpu(arch_freq_scale, cpu);
}

void topology_set_freq_scale(const struct cpumask *cpus, unsigned long cur_freq,
			     unsigned long max_freq);
bool topology_scale_freq_invariant(void);

enum scale_freq_source {
	SCALE_FREQ_SOURCE_CPUFREQ = 0,
	SCALE_FREQ_SOURCE_ARCH,
	SCALE_FREQ_SOURCE_CPPC,
};

struct scale_freq_data {
	enum scale_freq_source source;
	void (*set_freq_scale)(void);
};

void topology_scale_freq_tick(void);
void topology_set_scale_freq_source(struct scale_freq_data *data, const struct cpumask *cpus);
void topology_clear_scale_freq_source(enum scale_freq_source source, const struct cpumask *cpus);

DECLARE_PER_CPU(unsigned long, hw_pressure);

static inline unsigned long topology_get_hw_pressure(int cpu)
{
	return per_cpu(hw_pressure, cpu);
}

void topology_update_hw_pressure(const struct cpumask *cpus,
				      unsigned long capped_freq);

struct cpu_topology {
	int thread_id;//线程 ID，用于标识 CPU 在超线程中的 ID
	int core_id;//核心 ID，用于标识该 CPU 属于哪个物理核心
	int cluster_id;//集群 ID，用于标识该 CPU 所属的集群
	int package_id;//封装 ID，用于标识该 CPU 所属的物理处理器封装
	cpumask_t thread_sibling;//线程兄弟掩码，表示共享同一物理核心的所有线程.当一个物理核心支持超线程时，thread_sibling 掩码可用于找出共享该物理核心的所有逻辑处理器。
	cpumask_t core_sibling;//核心兄弟掩码，表示同一个物理包中的所有核心.用于找出同一个物理处理器封装内的所有核心，通常用于核心间的资源调度和管理。
	cpumask_t cluster_sibling;//集群兄弟掩码，表示同一集群中的所有核心.帮助在集群内进行调度，以便于提高缓存的利用率和内存访问的效率。
	cpumask_t llc_sibling;//共享 LLC（最后一级缓存，通常是 L3 缓存）的 CPU 掩码.
};

#ifdef CONFIG_GENERIC_ARCH_TOPOLOGY
extern struct cpu_topology cpu_topology[NR_CPUS];

#define topology_physical_package_id(cpu)	(cpu_topology[cpu].package_id)
#define topology_cluster_id(cpu)	(cpu_topology[cpu].cluster_id)
#define topology_core_id(cpu)		(cpu_topology[cpu].core_id)
#define topology_core_cpumask(cpu)	(&cpu_topology[cpu].core_sibling)
#define topology_sibling_cpumask(cpu)	(&cpu_topology[cpu].thread_sibling)
#define topology_cluster_cpumask(cpu)	(&cpu_topology[cpu].cluster_sibling)
#define topology_llc_cpumask(cpu)	(&cpu_topology[cpu].llc_sibling)
void init_cpu_topology(void);
void store_cpu_topology(unsigned int cpuid);
const struct cpumask *cpu_coregroup_mask(int cpu);
const struct cpumask *cpu_clustergroup_mask(int cpu);
void update_siblings_masks(unsigned int cpu);
void remove_cpu_topology(unsigned int cpuid);
void reset_cpu_topology(void);
int parse_acpi_topology(void);
void freq_inv_set_max_ratio(int cpu, u64 max_rate);
#endif

#endif /* _LINUX_ARCH_TOPOLOGY_H_ */
