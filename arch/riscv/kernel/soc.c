// SPDX-License-Identifier: GPL-2.0-or-later
/*
 * Copyright (C) 2020 Western Digital Corporation or its affiliates.
 */
#include <linux/init.h>
#include <linux/libfdt.h>
#include <linux/pgtable.h>
#include <asm/soc.h>

/*
 * This is called extremly early, before parse_dtb(), to allow initializing
 * SoC hardware before memory or any device driver initialization.
 */
void __init soc_early_init(void)
{
	void (*early_fn)(const void *fdt);//声明一个函数指针 early_fn，用于指向初始化函数
	const struct of_device_id *s;//声明一个指向设备匹配表项的指针s
	const void *fdt = dtb_early_va;//将设备树 blob 的虚拟地址赋值给 fdt

	for (s = (void *)&__soc_early_init_table_start;
	     (void *)s < (void *)&__soc_early_init_table_end; s++) {//遍历 __soc_early_init_table_start 和 __soc_early_init_table_end 之间的所有表项
		if (!fdt_node_check_compatible(fdt, 0, s->compatible)) {//检查设备树中的第一个节点是否与当前表项兼容
			early_fn = s->data;//如果兼容，将表项中的数据（函数指针）赋值给 early_fn
			early_fn(fdt);//调用 early_fn 函数并传递设备树 blob 指针
			return;
		}
	}
}
