// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (c) 2002-3 Patrick Mochel
 * Copyright (c) 2002-3 Open Source Development Labs
 */

#include <linux/device.h>
#include <linux/init.h>
#include <linux/memory.h>
#include <linux/of.h>
#include <linux/backing-dev.h>

#include "base.h"

/**
 * driver_init - initialize driver model.
 *
 * Call the driver model init functions to initialize their
 * subsystems. Called early from init/main.c.
 */
void __init driver_init(void)
{
	/* These are the core pieces */
	bdi_init(&noop_backing_dev_info);//初始化后台设备信息（backing device info）
	devtmpfs_init();//初始化 devtmpfs 文件系统，用于设备节点的自动管理
	devices_init();//初始化设备管理框架
	buses_init();//初始化总线框架，用于设备和驱动程序的匹配
	classes_init();//初始化设备类系统，用于设备分组和管理
	firmware_init();//初始化固件加载系统
	hypervisor_init();//初始化虚拟化相关的设备

	/* These are also core pieces, but must come after the
	 * core core pieces.
	 */
	of_core_init();//初始化设备树核心支持
	platform_bus_init();//初始化平台总线，用于与 SoC 相关的设备
	auxiliary_bus_init();//初始化辅助总线，用于辅助设备和驱动程序
	memory_dev_init();//初始化内存设备管理
	node_dev_init();//初始化节点设备（如 NUMA 节点）
	cpu_dev_init();//初始化 CPU 设备
	container_dev_init();//初始化容器设备，用于逻辑设备的分组
}
