// SPDX-License-Identifier: BSD-3-Clause OR GPL-2.0
/******************************************************************************
 *
 * Module Name: nsload - namespace loading/expanding/contracting procedures
 *
 * Copyright (C) 2000 - 2023, Intel Corp.
 *
 *****************************************************************************/

#include <acpi/acpi.h>
#include "accommon.h"
#include "acnamesp.h"
#include "acdispat.h"
#include "actables.h"
#include "acinterp.h"

#define _COMPONENT          ACPI_NAMESPACE
ACPI_MODULE_NAME("nsload")

/* Local prototypes */
#ifdef ACPI_FUTURE_IMPLEMENTATION
acpi_status acpi_ns_unload_namespace(acpi_handle handle);

static acpi_status acpi_ns_delete_subtree(acpi_handle start_handle);
#endif

/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_load_table
 *
 * PARAMETERS:  table_index     - Index for table to be loaded
 *              node            - Owning NS node
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load one ACPI table into the namespace
 *
 ******************************************************************************/
/* 
 * acpi_ns_load_table - 将ACPI表加载到命名空间并初始化相关对象
 * @table_index: 在ACPI全局表(RSDT/XSDT)中的索引位置
 * @node:        挂载点的命名空间节点（通常为根节点）
 * 
 * 核心功能：
 * 1. 两阶段加载机制：先创建命名空间对象，后解析控制方法
 * 2. 资源生命周期管理（owner_id机制）
 * 3. 错误回滚处理（命名空间对象删除）
 * 4. 并发控制（解释器锁）
 * 
 * 特殊处理：
 * - 防止重复加载
 * - 处理前向引用问题
 * - 支持Just-In-Time方法解析
 */
acpi_status
acpi_ns_load_table(u32 table_index, struct acpi_namespace_node *node)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_load_table);

	/* 如果表已加载到命名空间，直接返回 */
	if (acpi_tb_is_table_loaded(table_index)) {//检查全局表状态标志位
		status = AE_ALREADY_EXISTS;
		goto unlock;
	}

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "**** Loading table into namespace ****\n"));

	status = acpi_tb_allocate_owner_id(table_index);//分配所有者ID（用于资源跟踪）
	if (ACPI_FAILURE(status)) {
		goto unlock;
	}

	/*
    	 * 解析表并将所有命名对象加载到命名空间。
    	 * 此时不解析控制方法。实际上，在完整加载命名空间前无法解析控制方法，
    	 * 因为控制方法可能包含前向引用（调用其他方法），此时无法确定参数数量
    	 */
	status = acpi_ns_parse_table(table_index, node);// 核心解析函数（创建Device/Scope等对象）
	if (ACPI_SUCCESS(status)) {//如果解析成功
		acpi_tb_set_table_loaded_flag(table_index, TRUE);//设置表加载状态标志位
	} else {
		/*
        	 * 错误时删除本表创建的所有命名空间对象。
        	 * 典型错误场景：
        	 * - AE_ALREADY_EXISTS: 命名空间节点冲突
        	 * - AE_NOT_FOUND: Scope操作符目标不存在（违反ACPI规范）
        	 */
		acpi_ns_delete_namespace_by_owner(acpi_gbl_root_table_list.
						  tables[table_index].owner_id);//按所有者删除命名空间对象

		acpi_tb_release_owner_id(table_index);//释放分配的所有者ID
		return_ACPI_STATUS(status);//带函数跟踪的返回宏
	}

unlock://统一错误处理标签
	if (ACPI_FAILURE(status)) {//最终状态检查
		return_ACPI_STATUS(status);//返回前执行调试输出
	}

	/*
    	 * 现在可以安全解析控制方法。此处总会进行解析以完整性检查，
    	 * 如果配置为JIT解析，则会删除控制方法解析树
    	 */
	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "**** Begin Table Object Initialization\n"));

	acpi_ex_enter_interpreter();//获取解释器互斥锁
	status = acpi_ds_initialize_objects(table_index, node);//初始化方法/区域/缓冲区等对象
	acpi_ex_exit_interpreter();//释放解释器互斥锁

	ACPI_DEBUG_PRINT((ACPI_DB_INFO,
			  "**** Completed Table Object Initialization\n"));

	return_ACPI_STATUS(status);
}

#ifdef ACPI_OBSOLETE_FUNCTIONS
/*******************************************************************************
 *
 * FUNCTION:    acpi_load_namespace
 *
 * PARAMETERS:  None
 *
 * RETURN:      Status
 *
 * DESCRIPTION: Load the name space from what ever is pointed to by DSDT.
 *              (DSDT points to either the BIOS or a buffer.)
 *
 ******************************************************************************/

acpi_status acpi_ns_load_namespace(void)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(acpi_load_name_space);

	/* There must be at least a DSDT installed */

	if (acpi_gbl_DSDT == NULL) {
		ACPI_ERROR((AE_INFO, "DSDT is not in memory"));
		return_ACPI_STATUS(AE_NO_ACPI_TABLES);
	}

	/*
	 * Load the namespace. The DSDT is required,
	 * but the SSDT and PSDT tables are optional.
	 */
	status = acpi_ns_load_table_by_type(ACPI_TABLE_ID_DSDT);
	if (ACPI_FAILURE(status)) {
		return_ACPI_STATUS(status);
	}

	/* Ignore exceptions from these */

	(void)acpi_ns_load_table_by_type(ACPI_TABLE_ID_SSDT);
	(void)acpi_ns_load_table_by_type(ACPI_TABLE_ID_PSDT);

	ACPI_DEBUG_PRINT_RAW((ACPI_DB_INIT,
			      "ACPI Namespace successfully loaded at root %p\n",
			      acpi_gbl_root_node));

	return_ACPI_STATUS(status);
}
#endif

#ifdef ACPI_FUTURE_IMPLEMENTATION
/*******************************************************************************
 *
 * FUNCTION:    acpi_ns_delete_subtree
 *
 * PARAMETERS:  start_handle        - Handle in namespace where search begins
 *
 * RETURNS      Status
 *
 * DESCRIPTION: Walks the namespace starting at the given handle and deletes
 *              all objects, entries, and scopes in the entire subtree.
 *
 *              Namespace/Interpreter should be locked or the subsystem should
 *              be in shutdown before this routine is called.
 *
 ******************************************************************************/

static acpi_status acpi_ns_delete_subtree(acpi_handle start_handle)
{
	acpi_status status;
	acpi_handle child_handle;
	acpi_handle parent_handle;
	acpi_handle next_child_handle;
	acpi_handle dummy;
	u32 level;

	ACPI_FUNCTION_TRACE(ns_delete_subtree);

	parent_handle = start_handle;
	child_handle = NULL;
	level = 1;

	/*
	 * Traverse the tree of objects until we bubble back up
	 * to where we started.
	 */
	while (level > 0) {

		/* Attempt to get the next object in this scope */

		status = acpi_get_next_object(ACPI_TYPE_ANY, parent_handle,
					      child_handle, &next_child_handle);

		child_handle = next_child_handle;

		/* Did we get a new object? */

		if (ACPI_SUCCESS(status)) {

			/* Check if this object has any children */

			if (ACPI_SUCCESS
			    (acpi_get_next_object
			     (ACPI_TYPE_ANY, child_handle, NULL, &dummy))) {
				/*
				 * There is at least one child of this object,
				 * visit the object
				 */
				level++;
				parent_handle = child_handle;
				child_handle = NULL;
			}
		} else {
			/*
			 * No more children in this object, go back up to
			 * the object's parent
			 */
			level--;

			/* Delete all children now */

			acpi_ns_delete_children(child_handle);

			child_handle = parent_handle;
			status = acpi_get_parent(parent_handle, &parent_handle);
			if (ACPI_FAILURE(status)) {
				return_ACPI_STATUS(status);
			}
		}
	}

	/* Now delete the starting object, and we are done */

	acpi_ns_remove_node(child_handle);
	return_ACPI_STATUS(AE_OK);
}

/*******************************************************************************
 *
 *  FUNCTION:       acpi_ns_unload_name_space
 *
 *  PARAMETERS:     handle          - Root of namespace subtree to be deleted
 *
 *  RETURN:         Status
 *
 *  DESCRIPTION:    Shrinks the namespace, typically in response to an undocking
 *                  event. Deletes an entire subtree starting from (and
 *                  including) the given handle.
 *
 ******************************************************************************/

acpi_status acpi_ns_unload_namespace(acpi_handle handle)
{
	acpi_status status;

	ACPI_FUNCTION_TRACE(ns_unload_name_space);

	/* Parameter validation */

	if (!acpi_gbl_root_node) {
		return_ACPI_STATUS(AE_NO_NAMESPACE);
	}

	if (!handle) {
		return_ACPI_STATUS(AE_BAD_PARAMETER);
	}

	/* This function does the real work */

	status = acpi_ns_delete_subtree(handle);
	return_ACPI_STATUS(status);
}
#endif
