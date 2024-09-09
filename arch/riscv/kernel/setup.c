// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2009 Sunplus Core Technology Co., Ltd.
 *  Chen Liqin <liqin.chen@sunplusct.com>
 *  Lennox Wu <lennox.wu@sunplusct.com>
 * Copyright (C) 2012 Regents of the University of California
 * Copyright (C) 2020 FORTH-ICS/CARV
 *  Nick Kossifidis <mick@ics.forth.gr>
 */

#include <linux/acpi.h>
#include <linux/cpu.h>
#include <linux/init.h>
#include <linux/mm.h>
#include <linux/memblock.h>
#include <linux/sched.h>
#include <linux/console.h>
#include <linux/of_fdt.h>
#include <linux/sched/task.h>
#include <linux/smp.h>
#include <linux/efi.h>
#include <linux/crash_dump.h>
#include <linux/panic_notifier.h>

#include <asm/acpi.h>
#include <asm/alternative.h>
#include <asm/cacheflush.h>
#include <asm/cpufeature.h>
#include <asm/early_ioremap.h>
#include <asm/pgtable.h>
#include <asm/setup.h>
#include <asm/set_memory.h>
#include <asm/sections.h>
#include <asm/sbi.h>
#include <asm/tlbflush.h>
#include <asm/thread_info.h>
#include <asm/kasan.h>
#include <asm/efi.h>

#include "head.h"

/*
 * The lucky hart to first increment this variable will boot the other cores.
 * This is used before the kernel initializes the BSS so it can't be in the
 * BSS.
 */
atomic_t hart_lottery __section(".sdata")
#ifdef CONFIG_XIP_KERNEL
= ATOMIC_INIT(0xC001BEEF)
#endif
;
unsigned long boot_cpu_hartid;

/*
 * Place kernel memory regions on the resource tree so that
 * kexec-tools can retrieve them from /proc/iomem. While there
 * also add "System RAM" regions for compatibility with other
 * archs, and the rest of the known regions for completeness.
 */
static struct resource kimage_res = { .name = "Kernel image", };
static struct resource code_res = { .name = "Kernel code", };
static struct resource data_res = { .name = "Kernel data", };
static struct resource rodata_res = { .name = "Kernel rodata", };
static struct resource bss_res = { .name = "Kernel bss", };
#ifdef CONFIG_CRASH_DUMP
static struct resource elfcorehdr_res = { .name = "ELF Core hdr", };
#endif

static int __init add_resource(struct resource *parent,
				struct resource *res)
{
	int ret = 0;

	ret = insert_resource(parent, res);
	if (ret < 0) {
		pr_err("Failed to add a %s resource at %llx\n",
			res->name, (unsigned long long) res->start);
		return ret;
	}

	return 1;
}

static int __init add_kernel_resources(void)
{
	int ret = 0;

	/*
	 * The memory region of the kernel image is continuous and
	 * was reserved on setup_bootmem, register it here as a
	 * resource, with the various segments of the image as
	 * child nodes.
	 */

	code_res.start = __pa_symbol(_text);
	code_res.end = __pa_symbol(_etext) - 1;
	code_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	rodata_res.start = __pa_symbol(__start_rodata);
	rodata_res.end = __pa_symbol(__end_rodata) - 1;
	rodata_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	data_res.start = __pa_symbol(_data);
	data_res.end = __pa_symbol(_edata) - 1;
	data_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	bss_res.start = __pa_symbol(__bss_start);
	bss_res.end = __pa_symbol(__bss_stop) - 1;
	bss_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	kimage_res.start = code_res.start;
	kimage_res.end = bss_res.end;
	kimage_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;

	ret = add_resource(&iomem_resource, &kimage_res);
	if (ret < 0)
		return ret;

	ret = add_resource(&kimage_res, &code_res);
	if (ret < 0)
		return ret;

	ret = add_resource(&kimage_res, &rodata_res);
	if (ret < 0)
		return ret;

	ret = add_resource(&kimage_res, &data_res);
	if (ret < 0)
		return ret;

	ret = add_resource(&kimage_res, &bss_res);

	return ret;
}

static void __init init_resources(void)
{
	struct memblock_region *region = NULL;//声明指向内存块区域的指针
	struct resource *res = NULL;//声明指向资源结构的指针
	struct resource *mem_res = NULL;//声明指向内存资源结构的指针
	size_t mem_res_sz = 0;//声明内存资源大小变量
	int num_resources = 0, res_idx = 0;//声明资源数量和资源索引变量
	int ret = 0;//声明返回值变量

	/* +1是因为 memblock_alloc() 可能会增加 memblock.reserved.cnt */
	num_resources = memblock.memory.cnt + memblock.reserved.cnt + 1;//计算内存资源总数
	res_idx = num_resources - 1;//初始化资源索引为最后一个内存资源

	mem_res_sz = num_resources * sizeof(*mem_res);//计算内存资源结构所需的大小
	mem_res = memblock_alloc(mem_res_sz, SMP_CACHE_BYTES);//分配内存资源结构数组
	if (!mem_res)
		panic("%s: Failed to allocate %zu bytes\n", __func__, mem_res_sz);//输出错误信息并终止

	/*
	 * Start by adding the reserved regions, if they overlap
	 * with /memory regions, insert_resource later on will take
	 * care of it.
	 */
	ret = add_kernel_resources();//添加内核资源
	if (ret < 0)
		goto error;//添加失败，跳转到 error 标签进行错误处理

#ifdef CONFIG_CRASH_DUMP
	if (elfcorehdr_size > 0) {
		elfcorehdr_res.start = elfcorehdr_addr;//设置内核崩溃转储区域的起始地址
		elfcorehdr_res.end = elfcorehdr_addr + elfcorehdr_size - 1;//设置内核崩溃转储区域的结束地址
		elfcorehdr_res.flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;//设置资源标志
		add_resource(&iomem_resource, &elfcorehdr_res);//将崩溃转储资源添加到 IO 内存资源中
	}
#endif

	for_each_reserved_mem_region(region) {// 遍历每个保留的内存区域
		res = &mem_res[res_idx--];//获取下一个资源结构指针

		res->name = "Reserved";//设置资源名称为 "Reserved"
		res->flags = IORESOURCE_MEM | IORESOURCE_EXCLUSIVE;//设置资源标志为保留内存
		res->start = __pfn_to_phys(memblock_region_reserved_base_pfn(region));//获取保留内存区域的起始物理地址
		res->end = __pfn_to_phys(memblock_region_reserved_end_pfn(region)) - 1;//获取保留内存区域的结束物理地址

		/*
		 * Ignore any other reserved regions within
		 * system memory.
		 */
		if (memblock_is_memory(res->start)) {
			/* Re-use this pre-allocated resource */
			res_idx++;
			continue;
		}

		ret = add_resource(&iomem_resource, res);//将保留内存资源添加到 IO 内存资源中
		if (ret < 0)
			goto error;//如果出错，跳转到 error 标签进行错误处理
	}

	/* 将 /memory 区域添加到资源树 */
	for_each_mem_region(region) {//遍历每个内存区域
		res = &mem_res[res_idx--];// 获取下一个资源结构指针

		if (unlikely(memblock_is_nomap(region))) {//检查内存区域是否设置为 no-map
			res->name = "Reserved";//设置资源名称为 "Reserved"
			res->flags = IORESOURCE_MEM | IORESOURCE_EXCLUSIVE;//设置资源标志为保留内存
		} else {
			res->name = "System RAM";//设置资源名称为 "System RAM"
			res->flags = IORESOURCE_SYSTEM_RAM | IORESOURCE_BUSY;//设置资源标志为系统 RAM
		}

		res->start = __pfn_to_phys(memblock_region_memory_base_pfn(region));//获取内存区域的起始物理地址
		res->end = __pfn_to_phys(memblock_region_memory_end_pfn(region)) - 1;//获取内存区域的结束物理地址

		ret = add_resource(&iomem_resource, res);//将内存资源添加到 IO 内存资源中
		if (ret < 0)
			goto error;//如果出错，跳转到 error 标签进行错误处理
	}

	/* 清理任何未使用的预分配资源 */
	if (res_idx >= 0)//如果有未使用的资源
		memblock_free(mem_res, (res_idx + 1) * sizeof(*mem_res));//释放这些资源结构
	return;

 error:
	/* Better an empty resource tree than an inconsistent one */
	release_child_resources(&iomem_resource);//释放 IO 内存资源中的所有子资源
	memblock_free(mem_res, mem_res_sz);//释放预分配的资源结构
}


static void __init parse_dtb(void)
{
	/* Early scan of device tree from init memory */
	if (early_init_dt_scan(dtb_early_va)) {//一个早期的设备树扫描函数，dtb_early_va 是设备树在内存中的早期虚拟地址。此函数尝试从该地址读取并解析设备树。解析成功则返回 true，否则返回 false。
		const char *name = of_flat_dt_get_machine_name();//设备树中查找机器模型名称，如果成功找到则返回指向该名称的指针。

		if (name) {
			pr_info("Machine model: %s\n", name);//输出机器模型名称
			dump_stack_set_arch_desc("%s (DT)", name);//设置堆栈转储的架构描述
		}
	} else {
		pr_err("No DTB passed to the kernel\n");//输出错误信息，表示未传递DTB给内核
	}

#ifdef CONFIG_CMDLINE_FORCE
	strscpy(boot_command_line, CONFIG_CMDLINE, COMMAND_LINE_SIZE);//强制将内核命令行设置为 `CONFIG_CMDLINE`
	pr_info("Forcing kernel command line to: %s\n", boot_command_line);//输出信息，显示被强制设置的内核命令行
#endif
}

extern void __init init_rt_signal_env(void);

void __init setup_arch(char **cmdline_p)
{
	parse_dtb();//解析设备树（DTB）
	setup_initial_init_mm(_stext, _etext, _edata, _end);//设置初始内存管理，传入内核文本段和数据段的起始和结束地址

	*cmdline_p = boot_command_line;//将引导命令行参数指针指向全局的引导命令行

	early_ioremap_setup();//早期IO映射设置
	sbi_init();//初始化 RISC-V 的 SBI（Supervisor Binary Interface）
	jump_label_init();//初始化跳转标签，用于优化内核代码路径
	parse_early_param();//解析早期参数

	efi_init();//初始化 EFI（可扩展固件接口）
	paging_init();//创建页表映射，保留内存等

	/* Parse the ACPI tables for possible boot-time configuration */
	acpi_boot_table_init();//解析ACPI表，用于引导时的配置

#if IS_ENABLED(CONFIG_BUILTIN_DTB)
	unflatten_and_copy_device_tree();//展开并复制设备树（如果启用了内建 DTB）
#else
	unflatten_device_tree();//将设备树从扁平结构（FDT）展开内核使用的层次化数据结构
#endif
	misc_mem_init();//初始化通用内存管理

	init_resources();//初始化内存资源到资源树中

#ifdef CONFIG_KASAN
	kasan_init();//初始化 KASAN（Kernel Address Sanitizer）
#endif

#ifdef CONFIG_SMP
	setup_smp();//初始化 SMP（对称多处理器）
#endif
	/*如果 ACPI 没有禁用，则初始化 ACPI 的 RINTC（RISC-V Interrupt Controller）映射*/
	if (!acpi_disabled)
		acpi_init_rintc_map();

	riscv_init_cbo_blocksizes();//初始化 RISC-V 的 CBO（Cache Block Operation）缓存行大小
	riscv_fill_hwcap();//填充 RISC-V 硬件能力(指令扩展能力)
	init_rt_signal_env();//初始化实时信号环境(主要计算信号框架和信号帧总大小)
	apply_boot_alternatives();//应用引导时的替换方案（如替换特定指令）
	/*如果启用了 RISC-V ISA ZICBOM 扩展并且该扩展可用，则支持非一致性 */
	if (IS_ENABLED(CONFIG_RISCV_ISA_ZICBOM) &&
	    riscv_isa_extension_available(NULL, ZICBOM))
		riscv_noncoherent_supported();
	riscv_set_dma_cache_alignment();//设置 DMA 缓存对齐

	riscv_user_isa_enable();//启用用户态的 RISC-V ISA 扩展
}

bool arch_cpu_is_hotpluggable(int cpu)
{
	return cpu_has_hotplug(cpu);
}

void free_initmem(void)
{
	if (IS_ENABLED(CONFIG_STRICT_KERNEL_RWX)) {
		set_kernel_memory(lm_alias(__init_begin), lm_alias(__init_end), set_memory_rw_nx);
		if (IS_ENABLED(CONFIG_64BIT))
			set_kernel_memory(__init_begin, __init_end, set_memory_nx);
	}

	free_initmem_default(POISON_FREE_INITMEM);
}

static int dump_kernel_offset(struct notifier_block *self,
			      unsigned long v, void *p)
{
	pr_emerg("Kernel Offset: 0x%lx from 0x%lx\n",
		 kernel_map.virt_offset,
		 KERNEL_LINK_ADDR);

	return 0;
}

static struct notifier_block kernel_offset_notifier = {
	.notifier_call = dump_kernel_offset
};

static int __init register_kernel_offset_dumper(void)
{
	if (IS_ENABLED(CONFIG_RANDOMIZE_BASE))
		atomic_notifier_chain_register(&panic_notifier_list,
					       &kernel_offset_notifier);

	return 0;
}
device_initcall(register_kernel_offset_dumper);
