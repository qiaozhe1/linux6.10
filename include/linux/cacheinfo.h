/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_CACHEINFO_H
#define _LINUX_CACHEINFO_H

#include <linux/bitops.h>
#include <linux/cpumask.h>
#include <linux/smp.h>

struct device_node;
struct attribute;

enum cache_type {
	CACHE_TYPE_NOCACHE = 0,
	CACHE_TYPE_INST = BIT(0),
	CACHE_TYPE_DATA = BIT(1),
	CACHE_TYPE_SEPARATE = CACHE_TYPE_INST | CACHE_TYPE_DATA,
	CACHE_TYPE_UNIFIED = BIT(2),
};

extern unsigned int coherency_max_size;

/**
 * struct cacheinfo - represent a cache leaf node
 * @id: This cache's id. It is unique among caches with the same (type, level).
 * @type: type of the cache - data, inst or unified
 * @level: represents the hierarchy in the multi-level cache
 * @coherency_line_size: size of each cache line usually representing
 *	the minimum amount of data that gets transferred from memory
 * @number_of_sets: total number of sets, a set is a collection of cache
 *	lines sharing the same index
 * @ways_of_associativity: number of ways in which a particular memory
 *	block can be placed in the cache
 * @physical_line_partition: number of physical cache lines sharing the
 *	same cachetag
 * @size: Total size of the cache
 * @shared_cpu_map: logical cpumask representing all the cpus sharing
 *	this cache node
 * @attributes: bitfield representing various cache attributes
 * @fw_token: Unique value used to determine if different cacheinfo
 *	structures represent a single hardware cache instance.
 * @disable_sysfs: indicates whether this node is visible to the user via
 *	sysfs or not
 * @priv: pointer to any private data structure specific to particular
 *	cache design
 *
 * While @of_node, @disable_sysfs and @priv are used for internal book
 * keeping, the remaining members form the core properties of the cache
 */
struct cacheinfo {//描述与系统中的缓存层次结构相关的信息。它提供了缓存的基本属性
	unsigned int id;//缓存的唯一标识符
	enum cache_type type;//缓存类型（数据、指令或联合）
	unsigned int level;//缓存级别（L1、L2 等）
	unsigned int coherency_line_size;//缓存一致性行的大小（以字节为单位）
	unsigned int number_of_sets;//缓存中的总集合数
	unsigned int ways_of_associativity;//缓存的关联度（几路组相联）
	unsigned int physical_line_partition;//缓存物理分区的大小
	unsigned int size;//缓存的总大小（以字节为单位）
	cpumask_t shared_cpu_map;//共享此缓存的 CPU 掩码
	unsigned int attributes;//缓存属性，表示写策略和分配策略等
#define CACHE_WRITE_THROUGH	BIT(0)//表示写穿缓存策略
#define CACHE_WRITE_BACK	BIT(1)//表示写回缓存策略
#define CACHE_WRITE_POLICY_MASK		\
	(CACHE_WRITE_THROUGH | CACHE_WRITE_BACK)//写策略掩码，用于筛选写穿和写回
#define CACHE_READ_ALLOCATE	BIT(2)//表示读取时分配缓存
#define CACHE_WRITE_ALLOCATE	BIT(3)//表示写入时分配缓存
#define CACHE_ALLOCATE_POLICY_MASK	\
	(CACHE_READ_ALLOCATE | CACHE_WRITE_ALLOCATE)//分配策略掩码，用于筛选读取或写入分配
#define CACHE_ID		BIT(4)//用于标识缓存 ID 的掩码
	void *fw_token;//固件提供的标识缓存的令牌
	bool disable_sysfs;//是否禁用与该缓存相关的 sysfs 接口
	void *priv;//指向私有数据的指针，供缓存相关的特定实现使用
};

struct cpu_cacheinfo {//用于描述每个 CPU 的缓存信息
	struct cacheinfo *info_list;//指向包含缓存信息的结构体数组的指针，每个数组元素代表一个缓存级别的信息
	unsigned int per_cpu_data_slice_size;//每个 CPU 数据切片的大小，用于描述缓存中分配给每个 CPU 的数据量
	unsigned int num_levels;//缓存的级别数量，例如 L1、L2 等缓存级别的数量
	unsigned int num_leaves;//缓存的叶子节点数量，包括所有的统一缓存和分裂缓存
	bool cpu_map_populated;//标记是否已经填充了 CPU 缓存映射的信息
	bool early_ci_levels;//标记是否使用了早期的缓存级别信息来初始化 CPU 缓存
};

struct cpu_cacheinfo *get_cpu_cacheinfo(unsigned int cpu);
int early_cache_level(unsigned int cpu);
int init_cache_level(unsigned int cpu);
int init_of_cache_level(unsigned int cpu);
int populate_cache_leaves(unsigned int cpu);
int cache_setup_acpi(unsigned int cpu);
bool last_level_cache_is_valid(unsigned int cpu);
bool last_level_cache_is_shared(unsigned int cpu_x, unsigned int cpu_y);
int fetch_cache_info(unsigned int cpu);
int detect_cache_attributes(unsigned int cpu);
#ifndef CONFIG_ACPI_PPTT
/*
 * acpi_get_cache_info() is only called on ACPI enabled
 * platforms using the PPTT for topology. This means that if
 * the platform supports other firmware configuration methods
 * we need to stub out the call when ACPI is disabled.
 * ACPI enabled platforms not using PPTT won't be making calls
 * to this function so we need not worry about them.
 */
static inline
int acpi_get_cache_info(unsigned int cpu,
			unsigned int *levels, unsigned int *split_levels)
{
	return -ENOENT;
}
#else
int acpi_get_cache_info(unsigned int cpu,
			unsigned int *levels, unsigned int *split_levels);
#endif

const struct attribute_group *cache_get_priv_group(struct cacheinfo *this_leaf);

/*
 * Get the id of the cache associated with @cpu at level @level.
 * cpuhp lock must be held.
 */
static inline int get_cpu_cacheinfo_id(int cpu, int level)
{
	struct cpu_cacheinfo *ci = get_cpu_cacheinfo(cpu);
	int i;

	for (i = 0; i < ci->num_leaves; i++) {
		if (ci->info_list[i].level == level) {
			if (ci->info_list[i].attributes & CACHE_ID)
				return ci->info_list[i].id;
			return -1;
		}
	}

	return -1;
}

#ifdef CONFIG_ARM64
#define use_arch_cache_info()	(true)
#else
#define use_arch_cache_info()	(false)
#endif

#ifndef CONFIG_ARCH_HAS_CPU_CACHE_ALIASING
#define cpu_dcache_is_aliasing()	false
#else
#include <asm/cachetype.h>
#endif

#endif /* _LINUX_CACHEINFO_H */
