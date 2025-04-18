// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: utxfinit - External interfaces for ACPICA initialization
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#define EXPORT_ACPI_INTERFACES

#include <acpi/acpi.h>
#include "accommon.h"
#include "acevents.h"
#include "acnamesp.h"
#include "acdebug.h"
#include "actables.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utxfinit")

/* For acpi_exec only */
void ae_do_object_overrides(void);

/*******************************************************************************
 *
 * FUNCTION:    acpi_initialize_subsystem
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initializes all global variables. This is the first function
 *              called, so any early initialization belongs here.
 *
 ******************************************************************************/
/* 初始化ACPI子系统核心组件 */
acpi_status ACPI_INIT_FUNCTION acpi_initialize_subsystem(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_initialize_subsystem);

	/* 阶段1：设置子系统启动标志  */
	acpi_gbl_startup_flags = ACPI_SUBSYSTEM_INITIALIZE;//标记初始化阶段开始
	ACPI_DEBUG_EXEC(acpi_ut_init_stack_ptr_trace());//调试模式时初始化堆栈跟踪

	/* Initialize the OS-Dependent layer */
	/* 阶段2：操作系统依赖层初始化 */
	status = acpi_os_initialize();//初始化OS抽象层（硬件相关操作）
	if (ACPI_FAILURE(status)) {//如果操作系统层初始化失败
		ACPI_EXCEPTION((AE_INFO, status, "During OSL initialization"));
		return_ACPI_STATUS(status);//返回错误码
	}

	/* Initialize all globals used by the subsystem */
	/* 阶段3：全局变量初始化 */
	status = acpi_ut_init_globals();//初始化ACPI全局数据结构
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"During initialization of globals"));
		return_ACPI_STATUS(status);
	}

	/* Create the default mutex objects */
	/* 阶段4：互斥锁系统初始化 */
	status = acpi_ut_mutex_initialize();//创建核心互斥锁对象
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"During Global Mutex creation"));
		return_ACPI_STATUS(status);
	}

	/*
	 * Initialize the namespace manager and
	 * the root of the namespace tree
	 */
	/* 阶段5：命名空间初始化 */
	status = acpi_ns_root_initialize();//创建命名空间根节点
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"During Namespace initialization"));
		return_ACPI_STATUS(status);
	}

	/* Initialize the global OSI interfaces list with the static names */
	/* 阶段6：OSI接口初始化 */
	status = acpi_ut_initialize_interfaces();//加载预设OS兼容接口 
	if (ACPI_FAILURE(status)) {
		ACPI_EXCEPTION((AE_INFO, status,
				"During OSI interfaces initialization"));
		return_ACPI_STATUS(status);
	}

	return_ACPI_STATUS(AE_OK);//所有阶段成功完成
}

ACPI_EXPORT_SYMBOL_INIT(acpi_initialize_subsystem)

/*******************************************************************************
 *
 * FUNCTION:    acpi_enable_subsystem
 *
 * PARAMETERS:  flags               - Init/enable Options
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Completes the subsystem initialization including hardware.
 *              Puts system into ACPI mode if it isn't already.
 *
 ******************************************************************************/
/*
 * acpi_enable_subsystem - 启用ACPI子系统核心功能
 * @flags: 控制标志位(ACPI_NO_*系列标志)
 *
 * 功能：
 * 1. 标记早期初始化阶段完成
 * 2. 启用ACPI模式(除非明确跳过)
 * 3. 初始化FACS表(固件ACPI控制结构)
 * 4. 初始化事件处理机制(固定事件和通用事件)
 * 5. 安装SCI和全局锁中断处理程序
 *
 * 返回值：
 * AE_OK - 所有请求的初始化成功完成
 * AE_* - 具体错误代码
 */
acpi_status ACPI_INIT_FUNCTION acpi_enable_subsystem(u32 flags)
{
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE(acpi_enable_subsystem);

	/*
	 * The early initialization phase is complete. The namespace is loaded,
	 * and we can now support address spaces other than Memory, I/O, and
	 * PCI_Config.
	 */
	acpi_gbl_early_initialization = FALSE;//更新全局标志,标记早期初始化阶段完成

#if (!ACPI_REDUCED_HARDWARE)//在完整硬件支持配置下执行以下代码

	/* 阶段1：启用ACPI硬件模式 */
	if (!(flags & ACPI_NO_ACPI_ENABLE)) {//检查是否跳过ACPI启用
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "[Init] Going into ACPI mode\n"));

		acpi_gbl_original_mode = acpi_hw_get_mode();//获取当前硬件模式(ACPI或非ACPI)

		status = acpi_enable();//调用硬件抽象层函数启用ACPI模式
		if (ACPI_FAILURE(status)) {
			ACPI_WARNING((AE_INFO, "AcpiEnable failed"));
			return_ACPI_STATUS(status);
		}
	}

	/*
	 * Obtain a permanent mapping for the FACS. This is required for the
	 * Global Lock and the Firmware Waking Vector
	 */
	/* 阶段2：初始化FACS(Firmware ACPI Control Structure) 
	 * FACS表: 非AML表,是 ACPI与操作系统在低功耗状态下的一些通信工作，
	 * 尤其是睡眠/唤醒（S3/S4）相关控制
	 * */
	if (!(flags & ACPI_NO_FACS_INIT)) {//检查是否跳过FACS初始化
		status = acpi_tb_initialize_facs();//对FACS表进行永久内存映射
		if (ACPI_FAILURE(status)) {
			ACPI_WARNING((AE_INFO, "Could not map the FACS table"));
			return_ACPI_STATUS(status);
		}
	}

	/*
	 * Initialize ACPI Event handling (Fixed and General Purpose)
	 *
	 * Note1: We must have the hardware and events initialized before we can
	 * execute any control methods safely. Any control method can require
	 * ACPI hardware support, so the hardware must be fully initialized before
	 * any method execution!
	 *
	 * Note2: Fixed events are initialized and enabled here. GPEs are
	 * initialized, but cannot be enabled until after the hardware is
	 * completely initialized (SCI and global_lock activated) and the various
	 * initialization control methods are run (_REG, _STA, _INI) on the
	 * entire namespace.
	 */
	/* 阶段3：初始化ACPI事件处理系统 */
	if (!(flags & ACPI_NO_EVENT_INIT)) {//检查是否跳过事件初始化
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "[Init] Initializing ACPI events\n"));

		status = acpi_ev_initialize_events();//初始化固定事件和通用事件(GPE)基础设施
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	}

	/*
	 * Install the SCI handler and Global Lock handler. This completes the
	 * hardware initialization.
	 */
	/*阶段4：安装系统控制中断处理程序*/
	if (!(flags & ACPI_NO_HANDLER_INIT)) {//检查是否跳过处理程序安装
		ACPI_DEBUG_PRINT((ACPI_DB_EXEC,
				  "[Init] Installing SCI/GL handlers\n"));

		status = acpi_ev_install_xrupt_handlers();//安装SCI(System Control Interrupt)和全局锁处理程序
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	}
#endif				/* !ACPI_REDUCED_HARDWARE */

	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL_INIT(acpi_enable_subsystem)

/*******************************************************************************
 *
 * FUNCTION:    acpi_initialize_objects
 *
 * PARAMETERS:  flags               - Init/enable Options
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Completes namespace initialization by initializing device
 *              objects and executing AML code for Regions, buffers, etc.
 *
 ******************************************************************************/
/*
 * acpi_initialize_objects - ACPI对象初始化主函数
 *
 * 该函数完成ACPI子系统的三阶段初始化：
 * 1. (过时)遗留对象初始化(ACPI 5.0之前的行为)
 * 2. 设备/区域初始化(执行_STA/_INI/_REG等方法)
 * 3. 缓存清理(释放初始化期间使用的临时内存)
 *
 * @flags: 控制初始化的标志位组合
 * 返回值:
 *   AE_OK            - 成功
 *   AE_NO_MEMORY     - 内存不足
 *   其他ACPI状态码   - 来自下级函数
 */
acpi_status ACPI_INIT_FUNCTION acpi_initialize_objects(u32 flags)
{
	acpi_status status = AE_OK;

	ACPI_FUNCTION_TRACE(acpi_initialize_objects);

#ifdef ACPI_OBSOLETE_BEHAVIORa//阶段1: 遗留对象初始化 (ACPI 5.0+默认禁用)
	/*
	 * 05/2019: Removed, initialization now happens at both object
	 * creation and table load time
	 */

	/*
	 * Initialize the objects that remain uninitialized. This
	 * runs the executable AML that may be part of the
	 * declaration of these objects: operation_regions, buffer_fields,
	 * bank_fields, Buffers, and Packages.
	 */
	/*
    	 * 注意：此部分在2019年5月后已废弃
    	 * 现代ACPI实现改为在对象创建和表加载时直接初始化
    	 */
	if (!(flags & ACPI_NO_OBJECT_INIT)) {
		status = acpi_ns_initialize_objects();
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	}
#endif

	/*
	 * Initialize all device/region objects in the namespace. This runs
	 * the device _STA and _INI methods and region _REG methods.
	 */
	/* 阶段2: 设备/区域初始化 */
	if (!(flags & (ACPI_NO_DEVICE_INIT | ACPI_NO_ADDRESS_SPACE_INIT))) {
       		/*
        	 * 关键初始化路径：
        	 * 1. 执行所有设备的_STA(状态)方法
        	 * 2. 执行_INI(初始化)方法
        	 * 3. 执行_REG(区域设置)方法
        	 */
		status = acpi_ns_initialize_devices(flags);
		if (ACPI_FAILURE(status)) {
			return_ACPI_STATUS(status);
		}
	}

	/* 阶段3: 缓存清理 */
	/*
    	 * 设计考虑：
    	 * - 表加载期间缓存了大量临时对象
    	 * - 运行时不需要保留这些缓存
    	 * - 特别重要对于非分页内存
   	 */
	status = acpi_purge_cached_objects();//释放初始化缓存

	acpi_gbl_startup_flags |= ACPI_INITIALIZED_OK;//标记初始化完成
	return_ACPI_STATUS(status);
}

ACPI_EXPORT_SYMBOL_INIT(acpi_initialize_objects)
