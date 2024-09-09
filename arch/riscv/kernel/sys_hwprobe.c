// SPDX-License-Identifier: GPL-2.0-only
/*
 * The hwprobe interface, for allowing userspace to probe to see which features
 * are supported by the hardware.  See Documentation/arch/riscv/hwprobe.rst for
 * more details.
 */
#include <linux/syscalls.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/hwprobe.h>
#include <asm/sbi.h>
#include <asm/switch_to.h>
#include <asm/uaccess.h>
#include <asm/unistd.h>
#include <asm/vector.h>
#include <vdso/vsyscall.h>

/*
 * 用于获取指定 CPU 掩码中的硬件 ID。
 * */
static void hwprobe_arch_id(struct riscv_hwprobe *pair,
			    const struct cpumask *cpus)
{
	u64 id = -1ULL;//初始化变量 id 为 -1ULL，表示未知的 ID 值
	bool first = true;// 标志位，指示是否是第一次处理 CPU
	int cpu;//CPU 的索引

	for_each_cpu(cpu, cpus) {// 遍历指定的 CPU 掩码中的每个 CPU
		u64 cpu_id;//用于存储当前 CPU 的硬件 ID

		switch (pair->key) {//根据传入的键（pair->key）选择对应的硬件 ID 类型
		case RISCV_HWPROBE_KEY_MVENDORID:
			cpu_id = riscv_cached_mvendorid(cpu);//获取当前 CPU 的厂商 ID
			break;
		case RISCV_HWPROBE_KEY_MIMPID:
			cpu_id = riscv_cached_mimpid(cpu);//获取当前 CPU 的实现 ID
			break;
		case RISCV_HWPROBE_KEY_MARCHID:
			cpu_id = riscv_cached_marchid(cpu);//获取当前 CPU 的架构 ID
			break;
		}

		if (first) {//如果是处理第一个 CPU，则将 id 设置为当前的 cpu_id
			id = cpu_id;
			first = false;// 更新 first 标志位，表明已经处理过第一个 CPU
		}

		/*
		 * If there's a mismatch for the given set, return -1 in the
		 * value.
		 */
		if (id != cpu_id) {//如果当前 CPU 的 ID 与之前的 ID 不一致，则将 id 设置为 -1ULL 并退出循环
			id = -1ULL;
			break;
		}
	}

	pair->value = id;//将最终的 ID 赋值给传入的 pair 结构的 value 字段
}
/* 对指定的 RISC-V ISA 扩展进行硬件探测 */
static void hwprobe_isa_ext0(struct riscv_hwprobe *pair,
			     const struct cpumask *cpus)
{
	int cpu;// CPU 标识符
	u64 missing = 0;//用于记录缺失的扩展

	pair->value = 0;
	if (has_fpu())//检查是否有浮点单元 (FPU)
		pair->value |= RISCV_HWPROBE_IMA_FD;//如果有，设置对应的探测标志

	if (riscv_isa_extension_available(NULL, c))// 检查是否支持压缩指令集 (C 扩展)
		pair->value |= RISCV_HWPROBE_IMA_C;//如果支持，设置相应标志

	if (has_vector())//检查是否支持向量扩展
		pair->value |= RISCV_HWPROBE_IMA_V;//如果支持，设置向量扩展标志

	/*
	 * Loop through and record extensions that 1) anyone has, and 2) anyone
	 * doesn't have.
	 */
	for_each_cpu(cpu, cpus) {//遍历指定 CPU 掩码内的所有 CPU
		struct riscv_isainfo *isainfo = &hart_isa[cpu];//获取当前CPU的ISA信息

#define EXT_KEY(ext)									\	// 宏定义，用于检查指定的扩展是否可用
	do {										\	
		if (__riscv_isa_extension_available(isainfo->isa, RISCV_ISA_EXT_##ext))	\	//如果可用，设置探测标志
			pair->value |= RISCV_HWPROBE_EXT_##ext;				\
		else									\
			missing |= RISCV_HWPROBE_EXT_##ext;				\	//如果不可用，记录为缺失
	} while (false)

		/*
		 * Only use EXT_KEY() for extensions which can be exposed to userspace,
		 * regardless of the kernel's configuration, as no other checks, besides
		 * presence in the hart_isa bitmap, are made.
		 * 检测并记录支持的扩展
		 */
		EXT_KEY(ZBA);
		EXT_KEY(ZBB);
		EXT_KEY(ZBS);
		EXT_KEY(ZICBOZ);
		EXT_KEY(ZBC);

		EXT_KEY(ZBKB);
		EXT_KEY(ZBKC);
		EXT_KEY(ZBKX);
		EXT_KEY(ZKND);
		EXT_KEY(ZKNE);
		EXT_KEY(ZKNH);
		EXT_KEY(ZKSED);
		EXT_KEY(ZKSH);
		EXT_KEY(ZKT);
		EXT_KEY(ZIHINTNTL);
		EXT_KEY(ZTSO);
		EXT_KEY(ZACAS);
		EXT_KEY(ZICOND);
		EXT_KEY(ZIHINTPAUSE);

		if (has_vector()) {//如果支持向量扩展，检查更多向量扩展的支持情况
			EXT_KEY(ZVBB);
			EXT_KEY(ZVBC);
			EXT_KEY(ZVKB);
			EXT_KEY(ZVKG);
			EXT_KEY(ZVKNED);
			EXT_KEY(ZVKNHA);
			EXT_KEY(ZVKNHB);
			EXT_KEY(ZVKSED);
			EXT_KEY(ZVKSH);
			EXT_KEY(ZVKT);
			EXT_KEY(ZVFH);
			EXT_KEY(ZVFHMIN);
		}

		if (has_fpu()) {// 如果支持浮点单元，检查更多浮点扩展的支持情况
			EXT_KEY(ZFH);
			EXT_KEY(ZFHMIN);
			EXT_KEY(ZFA);
		}
#undef EXT_KEY // 取消定义的EXT_KEY宏，避免命名冲突
	}

	/* Now turn off reporting features if any CPU is missing it. */
	pair->value &= ~missing;//关闭所有缺失的扩展标志，确保只报告所有 CPU 都支持的扩展
}
/*
 * 用于检测给定的 CPU 集合是否支持指定的硬件扩展特性。
 * */
static bool hwprobe_ext0_has(const struct cpumask *cpus, unsigned long ext)
{
	struct riscv_hwprobe pair;//定义一个 riscv_hwprobe 结构体变量 pair，用于存储硬件探测结果

	hwprobe_isa_ext0(&pair, cpus);//将硬件探测结果存储在 pair 结构体中
	return (pair.value & ext);//检查探测结果中的 value 是否包含指定的扩展（ext），返回 true 或 false
}

#if defined(CONFIG_RISCV_PROBE_UNALIGNED_ACCESS)
/*
 * 用于检测给定的 CPU 是否对齐异常访问速度一致
 * 如果所有指定 CPU 的性能值一致，函数将返回该性能值
 * 如果性能值不一致或无法检测到性能值，则返回未知状态 RISCV_HWPROBE_MISALIGNED_UNKNOWN
 * */
static u64 hwprobe_misaligned(const struct cpumask *cpus)
{
	int cpu;
	u64 perf = -1ULL;// 初始化 perf 变量为 -1ULL，表示初始状态未检测

	for_each_cpu(cpu, cpus) {//遍历所有在 cpus 掩码中的 CPU
		int this_perf = per_cpu(misaligned_access_speed, cpu);//获取当前 CPU 的 misaligned_access_speed 值

		if (perf == -1ULL)// 如果 perf 仍然为初始值 -1ULL，则将其设置为当前 CPU 的性能值
			perf = this_perf;

		if (perf != this_perf) {// 如果当前 CPU 的性能值与之前记录的不一致，将 perf 设置为未知状态并终止遍历
			perf = RISCV_HWPROBE_MISALIGNED_UNKNOWN;
			break;
		}
	}

	if (perf == -1ULL)
		return RISCV_HWPROBE_MISALIGNED_UNKNOWN;//如果遍历后 perf 仍然为 -1ULL，说明没有有效性能值，返回未知状态

	return perf;
}
#else
static u64 hwprobe_misaligned(const struct cpumask *cpus)
{
	if (IS_ENABLED(CONFIG_RISCV_EFFICIENT_UNALIGNED_ACCESS))
		return RISCV_HWPROBE_MISALIGNED_FAST;

	if (IS_ENABLED(CONFIG_RISCV_EMULATED_UNALIGNED_ACCESS) && unaligned_ctl_available())
		return RISCV_HWPROBE_MISALIGNED_EMULATED;

	return RISCV_HWPROBE_MISALIGNED_SLOW;
}
#endif

static void hwprobe_one_pair(struct riscv_hwprobe *pair,
			     const struct cpumask *cpus)
{
	switch (pair->key) {//根据键值决定执行哪种硬件探测操作
	case RISCV_HWPROBE_KEY_MVENDORID:// 处理机器厂商 ID
	case RISCV_HWPROBE_KEY_MARCHID://处理架构 ID
	case RISCV_HWPROBE_KEY_MIMPID:// 处理实现 ID
		hwprobe_arch_id(pair, cpus);//调用 hwprobe_arch_id 函数获取这些与CPU硬件标识相关的ID
		break;
	/*
	 * The kernel already assumes that the base single-letter ISA
	 * extensions are supported on all harts, and only supports the
	 * IMA base, so just cheat a bit here and tell that to
	 * userspace.
	 * 内核已经假设所有 hart 都支持基本的单字母 ISA 扩展，并且只支
	 * 持 IMA 基础扩展，所以这里稍微“作弊”一下，将这个信息告诉用户
	 * 空间。
	 */
	case RISCV_HWPROBE_KEY_BASE_BEHAVIOR://处理IMA基本行为键
		pair->value = RISCV_HWPROBE_BASE_BEHAVIOR_IMA;//返回 IMA 基本行为，这块假设所有的 hart 都支持基本的单字母 ISA 扩展。
		break;

	case RISCV_HWPROBE_KEY_IMA_EXT_0://处理IMA扩展键
		hwprobe_isa_ext0(pair, cpus);//调用 hwprobe_isa_ext0 获取扩展
		break;

	case RISCV_HWPROBE_KEY_CPUPERF_0://处理 CPU 性能键
		pair->value = hwprobe_misaligned(cpus);//获取关于 CPU 性能（未对齐访问）相关的信息
		break;

	case RISCV_HWPROBE_KEY_ZICBOZ_BLOCK_SIZE:// 处理 ZICBOZ 扩展块大小键
		pair->value = 0;//初始化块大小为 0
		if (hwprobe_ext0_has(cpus, RISCV_HWPROBE_EXT_ZICBOZ))//如果支持 ZICBOZ 扩展
			pair->value = riscv_cboz_block_size;//获取实际的 ZICBOZ 块大小
		break;

	/*
	 * For forward compatibility, unknown keys don't fail the whole
	 * call, but get their element key set to -1 and value set to 0
	 * indicating they're unrecognized.
	 * 为了保持向前兼容，未识别的键不会导致整个操作失败，而是将键值设置为 -1，
	 * 值设置为 0，表示它们未被识别。
	 */
	default://处理未识别的键
		pair->key = -1;//设置键为 -1 表示未知
		pair->value = 0;//设置值为 0 表示无效
		break;
	}
}

static int hwprobe_get_values(struct riscv_hwprobe __user *pairs,
			      size_t pair_count, size_t cpusetsize,
			      unsigned long __user *cpus_user,
			      unsigned int flags)
{
	size_t out;//用于遍历输出的对数量的计数器
	int ret;//保存返回值
	cpumask_t cpus;//定义 CPU 掩码，用于存储指定的 CPU 集合

	/* Check the reserved flags. */
	if (flags != 0)//检查传入的 flags 是否为 0（没有保留标志）
		return -EINVAL;

	/*
	 * The interface supports taking in a CPU mask, and returns values that
	 * are consistent across that mask. Allow userspace to specify NULL and
	 * 0 as a shortcut to all online CPUs.
	 */
	cpumask_clear(&cpus);// 清除 cpus 掩码，将其初始化为空
	if (!cpusetsize && !cpus_user) {//如果没有指定 cpusetsize 和 cpus_user，则将所有在线 CPU 复制到 cpus 中
		cpumask_copy(&cpus, cpu_online_mask);
	} else {
		if (cpusetsize > cpumask_size())// 如果 cpusetsize 超过了系统的最大掩码大小，则限制 cpusetsize 为最大值
			cpusetsize = cpumask_size();

		ret = copy_from_user(&cpus, cpus_user, cpusetsize);//从用户空间复制 CPU 掩码到 cpus 变量中
		if (ret)
			return -EFAULT;//如果复制失败，则返回 -EFAULT 错误

		/*
		 * Userspace must provide at least one online CPU, without that
		 * there's no way to define what is supported.
		 * 用户空间必须提供至少一个在线 CPU，否则无法定义支持的内容
		 */
		cpumask_and(&cpus, &cpus, cpu_online_mask);//将指定的 CPU 掩码与在线 CPU 掩码求交集
		if (cpumask_empty(&cpus))// 如果掩码为空，返回 -EINVAL 错误
			return -EINVAL;
	}

	for (out = 0; out < pair_count; out++, pairs++) {//遍历所有传入的键值对，获取硬件探测值
		struct riscv_hwprobe pair;//定义一个 riscv_hwprobe 结构体，用于保存当前键值对

		if (get_user(pair.key, &pairs->key))//从用户空间读取键值对的键
			return -EFAULT;

		pair.value = 0;//初始化值为 0
		hwprobe_one_pair(&pair, &cpus);//对指定的 CPU 掩码进行硬件探测，更新 pair.value
		/* 将键值对的键和值写回到用户空间 */
		ret = put_user(pair.key, &pairs->key);
		if (ret == 0)
			ret = put_user(pair.value, &pairs->value);

		if (ret)//如果写入失败，返回 -EFAULT 错误
			return -EFAULT;
	}

	return 0;//返回 0 表示成功
}
/*获取 CPU 信息的实现函数*/
static int hwprobe_get_cpus(struct riscv_hwprobe __user *pairs,
			    size_t pair_count, size_t cpusetsize,
			    unsigned long __user *cpus_user,
			    unsigned int flags)
{
	cpumask_t cpus, one_cpu;//定义 CPU 掩码，用于存储 CPU 集合
	bool clear_all = false;// 标志是否清除所有探测键值对
	size_t i;
	int ret;

	if (flags != RISCV_HWPROBE_WHICH_CPUS)//检查标志位是否合法
		return -EINVAL;// 如果标志位不正确，返回无效参数错误

	if (!cpusetsize || !cpus_user)//检查 CPU 集大小和用户空间指针是否有效
		return -EINVAL;

	if (cpusetsize > cpumask_size())// 如果cpu集大小大于最大掩码大小
		cpusetsize = cpumask_size();//调整为最大掩码大小

	ret = copy_from_user(&cpus, cpus_user, cpusetsize);//从用户空间复制 CPU 掩码到内核空间
	if (ret)
		return -EFAULT;

	if (cpumask_empty(&cpus))//如果用户提供的CPU集合为空
		cpumask_copy(&cpus, cpu_online_mask);//复制当前在线的 CPU 集合

	cpumask_and(&cpus, &cpus, cpu_online_mask);//获取在线 CPU 集合的交集

	cpumask_clear(&one_cpu);//清除单个 CPU 掩码

	for (i = 0; i < pair_count; i++) {//遍历所有探测键值对
		struct riscv_hwprobe pair, tmp;// 定义探测键值对和临时探测键值对结构
		int cpu;//用于存储 CPU 号

		ret = copy_from_user(&pair, &pairs[i], sizeof(pair));//从用户空间复制探测键值对
		if (ret)
			return -EFAULT;

		if (!riscv_hwprobe_key_is_valid(pair.key)) {//检查探测键值对的键是否有效
			clear_all = true;//如果无效，设置清除所有探测键值对的标志
			pair = (struct riscv_hwprobe){ .key = -1, };// 将探测对的键设置为 -1，表示无效
			ret = copy_to_user(&pairs[i], &pair, sizeof(pair));//将无效的探测对复制回用户空间
			if (ret)
				return -EFAULT;
		}

		if (clear_all)//如果需要清除所有探测对
			continue;//跳过后续操作，继续下一个探测对

		tmp = (struct riscv_hwprobe){ .key = pair.key, };//初始化临时探测键值对，仅设置键

		for_each_cpu(cpu, &cpus) {//遍历所有在线且有效的 CPU
			cpumask_set_cpu(cpu, &one_cpu);//将当前 CPU 添加到单个 CPU 掩码中

			hwprobe_one_pair(&tmp, &one_cpu);// 对单个 CPU 执行探测

			if (!riscv_hwprobe_pair_cmp(&tmp, &pair))//如果临时键值对与原键值对不相同
				cpumask_clear_cpu(cpu, &cpus);//则从 cpus 集合中清除该 CPU

			cpumask_clear_cpu(cpu, &one_cpu);// 清除单个 CPU 掩码，准备进行下一次探测
		}
	}

	if (clear_all)//如果设置了清除所有探测键值对的标志
		cpumask_clear(&cpus);//清除所有 CPU

	ret = copy_to_user(cpus_user, &cpus, cpusetsize);//将最终的 CPU 集合复制回用户空间
	if (ret)
		return -EFAULT;

	return 0;
}
/*用户态进行 RISC-V 硬件探测*/
static int do_riscv_hwprobe(struct riscv_hwprobe __user *pairs,//探测的键值对
			    size_t pair_count, size_t cpusetsize,//pair_count 表示探测键值对的数量，cpusetsize 表示 CPU 集大小
			    unsigned long __user *cpus_user,//用户传入的 CPU 集合指针
			    unsigned int flags)//探测的标志位
{
	if (flags & RISCV_HWPROBE_WHICH_CPUS)//如果标志位中包含 RISCV_HWPROBE_WHICH_CPUS，表示要探测 CPU 信息
		return hwprobe_get_cpus(pairs, pair_count, cpusetsize,
					cpus_user, flags);// 调用 hwprobe_get_cpus 获取 CPU 信息

	return hwprobe_get_values(pairs, pair_count, cpusetsize,
				  cpus_user, flags);// 否则，获取硬件探测的具体值
}

#ifdef CONFIG_MMU

static int __init init_hwprobe_vdso_data(void)
{
	struct vdso_data *vd = __arch_get_k_vdso_data();
	struct arch_vdso_data *avd = &vd->arch_data;
	u64 id_bitsmash = 0;
	struct riscv_hwprobe pair;
	int key;

	/*
	 * Initialize vDSO data with the answers for the "all CPUs" case, to
	 * save a syscall in the common case.
	 */
	for (key = 0; key <= RISCV_HWPROBE_MAX_KEY; key++) {
		pair.key = key;
		hwprobe_one_pair(&pair, cpu_online_mask);

		WARN_ON_ONCE(pair.key < 0);

		avd->all_cpu_hwprobe_values[key] = pair.value;
		/*
		 * Smash together the vendor, arch, and impl IDs to see if
		 * they're all 0 or any negative.
		 */
		if (key <= RISCV_HWPROBE_KEY_MIMPID)
			id_bitsmash |= pair.value;
	}

	/*
	 * If the arch, vendor, and implementation ID are all the same across
	 * all harts, then assume all CPUs are the same, and allow the vDSO to
	 * answer queries for arbitrary masks. However if all values are 0 (not
	 * populated) or any value returns -1 (varies across CPUs), then the
	 * vDSO should defer to the kernel for exotic cpu masks.
	 */
	avd->homogeneous_cpus = id_bitsmash != 0 && id_bitsmash != -1;
	return 0;
}

arch_initcall_sync(init_hwprobe_vdso_data);

#endif /* CONFIG_MMU */

SYSCALL_DEFINE5(riscv_hwprobe, struct riscv_hwprobe __user *, pairs,
		size_t, pair_count, size_t, cpusetsize, unsigned long __user *,
		cpus, unsigned int, flags)
{
	return do_riscv_hwprobe(pairs, pair_count, cpusetsize,
				cpus, flags);
}
