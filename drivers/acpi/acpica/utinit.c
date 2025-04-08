// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: utinit - Common ACPI subsystem initialization
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acevents.h"
#include "actables.h"

#define _COMPONENT          ACPI_UTILITIES
ACPI_MODULE_NAME("utinit")

/* Local prototypes */
static void acpi_ut_terminate(void);

#if (!ACPI_REDUCED_HARDWARE)

static void acpi_ut_free_gpe_lists(void);

#else

#define acpi_ut_free_gpe_lists()
#endif				/* !ACPI_REDUCED_HARDWARE */

#if (!ACPI_REDUCED_HARDWARE)
/******************************************************************************
 *
 * FUNCTION:    acpi_ut_free_gpe_lists
 *
 * PARAMETERS:  none
 *
 * RETURN:      none
 *
 * DESCRIPTION: Free global GPE lists
 *
 ******************************************************************************/

static void acpi_ut_free_gpe_lists(void)
{
	struct acpi_gpe_block_info *gpe_block;
	struct acpi_gpe_block_info *next_gpe_block;
	struct acpi_gpe_xrupt_info *gpe_xrupt_info;
	struct acpi_gpe_xrupt_info *next_gpe_xrupt_info;

	/* Free global GPE blocks and related info structures */

	gpe_xrupt_info = acpi_gbl_gpe_xrupt_list_head;
	while (gpe_xrupt_info) {
		gpe_block = gpe_xrupt_info->gpe_block_list_head;
		while (gpe_block) {
			next_gpe_block = gpe_block->next;
			ACPI_FREE(gpe_block->event_info);
			ACPI_FREE(gpe_block->register_info);
			ACPI_FREE(gpe_block);

			gpe_block = next_gpe_block;
		}
		next_gpe_xrupt_info = gpe_xrupt_info->next;
		ACPI_FREE(gpe_xrupt_info);
		gpe_xrupt_info = next_gpe_xrupt_info;
	}
}
#endif				/* !ACPI_REDUCED_HARDWARE */

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_init_globals
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Initialize ACPICA globals. All globals that require specific
 *              initialization should be initialized here. This allows for
 *              a warm restart.
 *
 ******************************************************************************/

acpi_status acpi_ut_init_globals(void)//初始化ACPI子系统所有全局数据结构
{
	acpi_status status;
	u32 i;

	ACPI_FUNCTION_TRACE(ut_init_globals);

	/* Create all memory caches */
	/* 阶段1：创建内存对象缓存池 */
	status = acpi_ut_create_caches();//创建ACPI对象缓存（操作对象重用池）
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Address Range lists */
	/* 阶段2：初始化地址范围跟踪列表 */
	for (i = 0; i < ACPI_ADDRESS_RANGE_MAX; i++) {//遍历所有地址空间类型
		acpi_gbl_address_range_list[i] = NULL;//初始化地址范围链表头
	}

	/* Mutex locked flags */
	/* 阶段3：初始化互斥锁元数据 */
	for (i = 0; i < ACPI_NUM_MUTEX; i++) {//遍历所有预定义互斥锁
		acpi_gbl_mutex_info[i].mutex = NULL;//清空互斥锁指针
		acpi_gbl_mutex_info[i].thread_id = ACPI_MUTEX_NOT_ACQUIRED;//标记未锁定
		acpi_gbl_mutex_info[i].use_count = 0;//使用计数器归零
	}

	/* 阶段4：初始化Owner ID位图 */
	for (i = 0; i < ACPI_NUM_OWNERID_MASKS; i++) {//清空所有权掩码
		acpi_gbl_owner_id_mask[i] = 0;
	}

	/* Last owner_ID is never valid */

	acpi_gbl_owner_id_mask[ACPI_NUM_OWNERID_MASKS - 1] = 0x80000000;//最高位置1防止越界

	/* Event counters */
	/* 阶段5：重置事件计数器 */
	acpi_method_count = 0;//ACPI方法执行次数统计
	acpi_sci_count = 0;//系统控制中断计数 
	acpi_gpe_count = 0;//通用事件计数

	for (i = 0; i < ACPI_NUM_FIXED_EVENTS; i++) {//固定事件计数器清零
		acpi_fixed_event_count[i] = 0;
	}

#if (!ACPI_REDUCED_HARDWARE)//完整硬件支持配置

	/* GPE/SCI support */
	/* 阶段6：初始化GPE/SCI子系统 */
	acpi_gbl_all_gpes_initialized = FALSE;//GPE未完成初始化标记
	acpi_gbl_gpe_xrupt_list_head = NULL;//GPE中断列表头指针
	acpi_gbl_gpe_fadt_blocks[0] = NULL;//FADT定义的GPE0块
	acpi_gbl_gpe_fadt_blocks[1] = NULL;//FADT定义的GPE1块
	acpi_current_gpe_count = 0;//当前活跃GPE数量

	acpi_gbl_global_event_handler = NULL;//全局事件回调函数
	acpi_gbl_sci_handler_list = NULL;//SCI处理程序链表 

#endif				/* !ACPI_REDUCED_HARDWARE */

	/* Global handlers */
	/* 阶段7：初始化全局回调函数 */
	acpi_gbl_global_notify[0].handler = NULL;//系统通知处理函数
	acpi_gbl_global_notify[1].handler = NULL;//设备通知处理函数
	acpi_gbl_exception_handler = NULL;//异常处理回调
	acpi_gbl_init_handler = NULL;//初始化过程回调
	acpi_gbl_table_handler = NULL;//ACPI表操作回调
	acpi_gbl_interface_handler = NULL;//OSI接口处理回调

	/* Global Lock support */
	/* 阶段8：初始化全局锁结构 */
	acpi_gbl_global_lock_semaphore = NULL;//全局锁信号量
	acpi_gbl_global_lock_mutex = NULL;//全局锁互斥量
	acpi_gbl_global_lock_acquired = FALSE;//全局锁获取状态
	acpi_gbl_global_lock_handle = 0;//锁持有者句柄 
	acpi_gbl_global_lock_present = FALSE;//硬件全局锁存在标志

	/* Miscellaneous variables */
	/* 阶段9：杂项全局变量初始化 */
	acpi_gbl_DSDT = NULL;//DSDT表指针 
	acpi_gbl_cm_single_step = FALSE;//控制方法单步调试标志
	acpi_gbl_shutdown = FALSE;//系统关机状态标记 
	acpi_gbl_ns_lookup_count = 0;//命名空间查找次数统计
	acpi_gbl_ps_find_count = 0;//解析树节点查找计数
	acpi_gbl_acpi_hardware_present = TRUE;//ACPI硬件存在标志
	acpi_gbl_last_owner_id_index = 0;//最后分配的Owner ID索引
	acpi_gbl_next_owner_id_offset = 0;//Owner ID分配偏移量
	acpi_gbl_debugger_configuration = DEBUGGER_THREADING;//调试器模式
	acpi_gbl_osi_mutex = NULL;//OSI接口互斥锁

	/* Hardware oriented */
	/* 阶段10：硬件相关状态 */
	acpi_gbl_events_initialized = FALSE;//ACPI事件子系统就绪标记
	acpi_gbl_system_awake_and_running = TRUE;//系统运行状态

	/* Namespace */
	/* 阶段11：初始化命名空间根节点 */
	acpi_gbl_root_node = NULL;//命名空间根节点指针
	acpi_gbl_root_node_struct.name.integer = ACPI_ROOT_NAME;//根节点名称
	acpi_gbl_root_node_struct.descriptor_type = ACPI_DESC_TYPE_NAMED;//描述符类型
	acpi_gbl_root_node_struct.type = ACPI_TYPE_DEVICE;//节点类型为设备
	acpi_gbl_root_node_struct.parent = NULL;//无父节点
	acpi_gbl_root_node_struct.child = NULL;//初始化无子节点
	acpi_gbl_root_node_struct.peer = NULL;//初始化无同级节点
	acpi_gbl_root_node_struct.object = NULL;//关联对象为空

#ifdef ACPI_DISASSEMBLER//反汇编器支持
	acpi_gbl_external_list = NULL;//外部对象列表
	acpi_gbl_num_external_methods = 0;//外部方法计数
	acpi_gbl_resolved_external_methods = 0;//已解析外部方法计数
#endif

#ifdef ACPI_DEBUG_OUTPUT//调试输出支持
	acpi_gbl_lowest_stack_pointer = ACPI_CAST_PTR(acpi_size, ACPI_SIZE_MAX);//栈深度追踪
#endif

#ifdef ACPI_DBG_TRACK_ALLOCATIONS
	acpi_gbl_display_final_mem_stats = FALSE;//最终内存统计显示开关 
	acpi_gbl_disable_mem_tracking = FALSE;//内存追踪禁用标志 
#endif

	return_ACPI_STATUS(AE_OK);
}

/******************************************************************************
 *
 * FUNCTION:    acpi_ut_terminate
 *
 * PARAMETERS:  none
 *
 * RETURN:      none
 *
 * DESCRIPTION: Free global memory
 *
 ******************************************************************************/

static void acpi_ut_terminate(void)
{
	ACPI_FUNCTION_TRACE(ut_terminate);

	acpi_ut_free_gpe_lists();
	acpi_ut_delete_address_lists();
	return_VOID;
}

/*******************************************************************************
 *
 * FUNCTION:    acpi_ut_subsystem_shutdown
 *
 * PARAMETERS:  None
 *
 * RETURN:      None
 *
 * DESCRIPTION: Shutdown the various components. Do not delete the mutex
 *              objects here, because the AML debugger may be still running.
 *
 ******************************************************************************/

void acpi_ut_subsystem_shutdown(void)
{
	ACPI_FUNCTION_TRACE(ut_subsystem_shutdown);

	/* Just exit if subsystem is already shutdown */

	if (acpi_gbl_shutdown) {
		ACPI_ERROR((AE_INFO, "ACPI Subsystem is already terminated"));
		return_VOID;
	}

	/* Subsystem appears active, go ahead and shut it down */

	acpi_gbl_shutdown = TRUE;
	acpi_gbl_startup_flags = 0;
	ACPI_DEBUG_PRINT((ACPI_DB_INFO, "Shutting down ACPI Subsystem\n"));

#ifndef ACPI_ASL_COMPILER

	/* Close the acpi_event Handling */

	acpi_ev_terminate();

	/* Delete any dynamic _OSI interfaces */

	acpi_ut_interface_terminate();
#endif

	/* Close the Namespace */

	acpi_ns_terminate();

	/* Delete the ACPI tables */

	acpi_tb_terminate();

	/* Close the globals */

	acpi_ut_terminate();

	/* Purge the local caches */

	(void)acpi_ut_delete_caches();
	return_VOID;
}
